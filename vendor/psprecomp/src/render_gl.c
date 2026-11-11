/* psprecomp -- the OpenGL backend: GL 3.3 core, or GLES 3.0 for Android.
 *
 * The shape follows PPSSPP's GPU backends (read for reference, not copied):
 *
 *  - The GE interpreter keeps doing everything up to screen space (ge.c), so
 *    this backend and the software one see the same vertices. The software
 *    backend stays the oracle to diff against.
 *  - A framebuffer manager: each framebuffer address the GE draws to becomes
 *    an FBO, at `scale` times the PSP's resolution. sceDisplaySetFrameBuf
 *    shows the FBO at the display address. A texture whose address lies in a
 *    framebuffer samples that FBO directly (render to texture), so nothing is
 *    read back to guest memory.
 *  - A texture cache: decoded once through psp_tex_fetch() (the one place that
 *    knows the PSP's formats), keyed by address, format, size and CLUT, and
 *    re-decoded when a hash of its memory changes.
 *  - A render thread that owns the context. The GE runs on whichever guest
 *    thread submitted the list, and a GL context can only be current on one
 *    thread, so the GE side records commands into a frame and the render
 *    thread plays them back -- one frame behind, as PPSSPP's render managers
 *    do. Recording is cheap; it is the playback that the GPU does.
 *
 * Not done: points and lines, colour test, logic ops, dithering,
 * mipmaps, framebuffers read by the CPU, block transfers into framebuffers,
 * and blend modes GL cannot express (doubled alphas and absolute difference),
 * which are approximated.
 */

#include "psprecomp/render.h"
#include "psprecomp/frame_pacing.h"
#include "psprecomp/transform_interpolation.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"

#include "hle/host_thread.h"
#include "host_pacing.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the GL subset this file uses ---------------------------------------- */

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#ifdef _WIN32
#define GLCALL __stdcall
#else
#define GLCALL
#endif

#define GL_ZERO                     0
#define GL_ONE                      1
#define GL_TRIANGLES                0x0004
#define GL_TRIANGLE_STRIP           0x0005
#define GL_NEVER                    0x0200
#define GL_LESS                     0x0201
#define GL_EQUAL                    0x0202
#define GL_LEQUAL                   0x0203
#define GL_GREATER                  0x0204
#define GL_NOTEQUAL                 0x0205
#define GL_GEQUAL                   0x0206
#define GL_ALWAYS                   0x0207
#define GL_SRC_COLOR                0x0300
#define GL_ONE_MINUS_SRC_COLOR      0x0301
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_DST_ALPHA                0x0304
#define GL_ONE_MINUS_DST_ALPHA      0x0305
#define GL_DST_COLOR                0x0306
#define GL_ONE_MINUS_DST_COLOR      0x0307
#define GL_CULL_FACE                0x0B44
#define GL_DEPTH_TEST               0x0B71
#define GL_BLEND                    0x0BE2
#define GL_SCISSOR_TEST             0x0C11
#define GL_STENCIL_TEST             0x0B90
#define GL_KEEP                     0x1E00
#define GL_REPLACE                  0x1E01
#define GL_INCR                     0x1E02
#define GL_DECR                     0x1E03
#define GL_INVERT                   0x150A
#define GL_UNPACK_ALIGNMENT         0x0CF5
#define GL_PACK_ALIGNMENT           0x0D05
#define GL_TEXTURE_2D               0x0DE1
#define GL_UNSIGNED_BYTE            0x1401
#define GL_FLOAT                    0x1406
#define GL_RGBA                     0x1908
#define GL_NEAREST                  0x2600
#define GL_LINEAR                   0x2601
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_REPEAT                   0x2901
#define GL_CONSTANT_COLOR           0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_FUNC_ADD                 0x8006
#define GL_MIN                      0x8007
#define GL_MAX                      0x8008
#define GL_FUNC_SUBTRACT            0x800A
#define GL_FUNC_REVERSE_SUBTRACT    0x800B
#define GL_RGBA8                    0x8058
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_TEXTURE0                 0x84C0
#define GL_DEPTH_CLAMP              0x864F
#define GL_ARRAY_BUFFER             0x8892
#define GL_STREAM_DRAW              0x88E0
#define GL_DEPTH24_STENCIL8         0x88F0
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_READ_FRAMEBUFFER         0x8CA8
#define GL_DRAW_FRAMEBUFFER         0x8CA9
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_FRAMEBUFFER              0x8D40
#define GL_RENDERBUFFER             0x8D41
#define GL_COLOR_BUFFER_BIT         0x4000
#define GL_DEPTH_BUFFER_BIT         0x0100
#define GL_STENCIL_BUFFER_BIT       0x0400
#define GL_ANY_SAMPLES_PASSED        0x8C2F
#define GL_QUERY_RESULT              0x8866
#define GL_QUERY_RESULT_AVAILABLE    0x8867

/* Every entry point, loaded through the host. */
#define GL_FUNCS(X) \
    X(void,   Enable, (GLenum)) \
    X(void,   Disable, (GLenum)) \
    X(GLenum, GetError, (void)) \
    X(void,   Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void,   Scissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void,   ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,   Clear, (GLbitfield)) \
    X(void,   ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void,   DepthMask, (GLboolean)) \
    X(void,   DepthFunc, (GLenum)) \
    X(void,   StencilFunc, (GLenum, GLint, GLuint)) \
    X(void,   StencilOp, (GLenum, GLenum, GLenum)) \
    X(void,   StencilMask, (GLuint)) \
    X(void,   BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void,   BlendEquationSeparate, (GLenum, GLenum)) \
    X(void,   BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,   PixelStorei, (GLenum, GLint)) \
    X(void,   ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void,   GenTextures, (GLsizei, GLuint *)) \
    X(void,   DeleteTextures, (GLsizei, const GLuint *)) \
    X(void,   BindTexture, (GLenum, GLuint)) \
    X(void,   ActiveTexture, (GLenum)) \
    X(void,   TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void,   CompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
    X(void,   GetIntegerv, (GLenum, GLint *)) \
    X(const unsigned char *, GetStringi, (GLenum, GLuint)) \
    X(void,   TexParameteri, (GLenum, GLenum, GLint)) \
    X(void,   GenFramebuffers, (GLsizei, GLuint *)) \
    X(void,   DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void,   BindFramebuffer, (GLenum, GLuint)) \
    X(void,   DrawBuffers, (GLsizei, const GLenum *)) \
    X(void,   ReadBuffer, (GLenum)) \
    X(void,   FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void,   BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void,   GenRenderbuffers, (GLsizei, GLuint *)) \
    X(void,   DeleteRenderbuffers, (GLsizei, const GLuint *)) \
    X(void,   BindRenderbuffer, (GLenum, GLuint)) \
    X(void,   GetRenderbufferParameteriv, (GLenum, GLenum, GLint *)) \
    X(void,   RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void,   FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void,   GenBuffers, (GLsizei, GLuint *)) \
    X(void,   BindBuffer, (GLenum, GLuint)) \
    X(void,   BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void,   GenVertexArrays, (GLsizei, GLuint *)) \
    X(void,   BindVertexArray, (GLuint)) \
    X(void,   VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void,   EnableVertexAttribArray, (GLuint)) \
    X(void,   DrawArrays, (GLenum, GLint, GLsizei)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void,   ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void,   CompileShader, (GLuint)) \
    X(void,   GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void,   GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, CreateProgram, (void)) \
    X(void,   AttachShader, (GLuint, GLuint)) \
    X(void,   LinkProgram, (GLuint)) \
    X(void,   GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void,   GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,   UseProgram, (GLuint)) \
    X(GLint,  GetUniformLocation, (GLuint, const GLchar *)) \
    X(void,   Uniform1i, (GLint, GLint)) \
    X(void,   Uniform1f, (GLint, GLfloat)) \
    X(void,   Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void,   Uniform4i, (GLint, GLint, GLint, GLint, GLint)) \
    X(void,   Uniform3f, (GLint, GLfloat, GLfloat, GLfloat))

#define GL_DECLARE(ret, name, args) static ret (GLCALL *gl##name) args;
GL_FUNCS(GL_DECLARE)

/* Optional diagnostics never turn a missing query symbol into a GPU failure. */
static void (GLCALL *effectGenQueries)(GLsizei,GLuint *);
static void (GLCALL *effectDeleteQueries)(GLsizei,const GLuint *);
static void (GLCALL *effectBeginQuery)(GLenum,GLuint);
static void (GLCALL *effectEndQuery)(GLenum);
static void (GLCALL *effectGetQueryObjectuiv)(GLuint,GLenum,GLuint *);

/* ---- shared between the two threads ---------------------------------------- */

#define SW 480
#define SH 272

static psp_gl_host g_host;
static int         g_scale = 1;

/* A growable byte buffer. */
typedef struct { uint8_t *p; size_t n, cap; } gbuf;

static void *gbuf_add(gbuf *b, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 1 << 16;
        while (cap < b->n + n) cap *= 2;
        uint8_t *p = (uint8_t *)realloc(b->p, cap);
        if (!p) { fputs("render_gl: out of memory\n", stderr); abort(); }
        b->p = p;
        b->cap = cap;
    }
    void *at = b->p + b->n;
    b->n += n;
    return at;
}

/* One vertex as the GPU gets it: clip-space position (perspective comes from
 * w), colour, normalised texture coordinate, fog factor. */
typedef struct {
    float    x, y, z, w;
    uint32_t rgba;
    float    u, v;
    float    fog;
    float    depth, depth_base; /* effective PSP screen depth and triangle reference */
    float    local[3];
    int      local_valid;
} gvert;

/* The GL state of one draw, resolved on the GE side to GL enums so that the
 * render thread only applies it, and consecutive draws in the same state
 * merge into one. Always memset before filling: draws are compared bytewise. */
typedef struct {
    int32_t tex_kind, tex_id;              /* 0 none, 1 cached, 2 framebuffer, 3 feedback, 4 framebuffer CLUT32 */
    int32_t clut[4];                       /* immutable palette texture id, shift, mask, start */
    int32_t wrap_s, wrap_t, min_f, mag_f;
    int32_t mode[4];                       /* flags, texture function, alpha func, ref | mask << 8 */
    float   env[3], fogc[3];
    int32_t blend, bsrc, bdst, beq;
    float   bcol[4];
    float   source_factor[3];            /* independent PSP FIX source RGB */
    int32_t depth_test, depth_func, depth_mask;
    float   depth_range[2];
    uint32_t zb_addr;
    int32_t zb_stride;
    int32_t cmask[4];
    int32_t stencil, stencil_func, stencil_ref, stencil_mask, stencil_write;
    int32_t stencil_fail,stencil_zfail,stencil_zpass;
    int32_t needs_dest_alpha;
    int32_t sc[4];                         /* scissor x, y, w, h in PSP pixels */
} gstate;

enum { MODE_TEX = 1, MODE_TEXALPHA = 2, MODE_DOUBLE = 4, MODE_FOG = 8, MODE_ATEST = 16,
       MODE_CLEAR = 32, MODE_5551 = 64, MODE_565 = 128, MODE_4444 = 256, MODE_DEPTHCLAMP = 512,
       MODE_DEPTHRANGE = 1024 };

/* The command stream. Every command starts with its op and size. */
enum { C_FB_NEW = 1, C_FB_FREE, C_FB_BIND, C_TEX_NEW, C_TEX_FREE, C_DRAW, C_PRESENT };

typedef struct { uint32_t op, size; } chead;
typedef struct { chead h; int32_t id, w, hgt, fmt; } c_fb_new;
typedef struct { chead h; int32_t id; } c_id;
/* The render thread frees pixels. gl_fmt: 0 for RGBA8, else a compressed GL
 * format of `bytes` bytes (a texture pack's picture). */
typedef struct { chead h; int32_t id, w, hgt; uint32_t *pixels; uint32_t gl_fmt, bytes;
    uint64_t source_hash,clut_key; uint32_t source_addr; } c_tex_new;
/* Labels travel with commands, outside gstate so batch matching is unchanged. */
typedef struct { chead h; gstate s; uint32_t first, count; psp_render_transform transform;
    uint32_t fb_addr,tex_addr,tex_addr_last,source_draws; int32_t fb_stride,fb_fmt; } c_draw;
/* fb >= 0: show that framebuffer (its first 480x272); else show `pixels`. */
typedef struct { chead h; int32_t fb; uint32_t *pixels; } c_present;

typedef struct { gbuf cmd, vtx; } gframe;

static gframe     g_frames[2];
static int        g_rec;                 /* the frame being recorded (GE side) */
static int        g_pending = -1;        /* handed to the render thread, not yet played */
static host_mutex g_m = HOST_MUTEX_INIT;
static host_cond  g_cv = HOST_COND_INIT;
static int        g_started, g_init_result, g_quit;
/* CPU-owned request, worker-owned result, all protected by g_m. */
static struct {int pending,done,result,scale,fps,stretch;} g_reconfigure;
/* Chosen before worker creation; immutable until that session is stopped. */
static int        g_effect_requested;
static host_thread g_render_thread;
static char g_error[512];
static int g_max_texture,g_max_renderbuffer,g_attachment_width,g_attachment_height;
void psp_render_gl_attachment_size(int *width,int *height) {
    host_mutex_lock(&g_m);if(width)*width=g_attachment_width;if(height)*height=g_attachment_height;host_mutex_unlock(&g_m);
}
static void gpu_fail(const char *reason) {
    host_mutex_lock(&g_m);
    int first=!g_error[0];if(first)snprintf(g_error,sizeof g_error,"%s",reason);
    host_cond_broadcast(&g_cv);host_mutex_unlock(&g_m);
    if(first)fprintf(stderr,"render_gl: %s\n",reason);
}
const char *psp_render_gl_error(void) {
    host_mutex_lock(&g_m);const char *p=g_error[0]?g_error:NULL;host_mutex_unlock(&g_m);return p;
}
int psp_render_gl_wait_idle(void) {
    host_mutex_lock(&g_m);
    while(g_started && g_pending>=0 && !g_error[0])host_cond_wait(&g_cv,&g_m);
    int failed=!!g_error[0];host_mutex_unlock(&g_m);return failed?-1:0;
}

/* The overlay and captures, guarded by g_m. */
static uint32_t *g_ovl;
static int       g_ovl_w, g_ovl_h, g_ovl_dw, g_ovl_dh, g_ovl_dirty;
static uint32_t  g_capture[SW * SH];
static int       g_have_capture;
static uint64_t g_capture_requested,g_capture_done,g_presented,g_readbacks;
static int g_last_present_fb=-1,g_last_present_valid;
static uint32_t g_last_ram[SW*SH];

static void *cmd_add(uint32_t op, size_t size) {
    size = (size + 7) & ~(size_t)7;
    chead *h = (chead *)gbuf_add(&g_frames[g_rec].cmd, size);
    memset(h, 0, size);
    h->op = op;
    h->size = (uint32_t)size;
    return h;
}

/* ---- GE side: framebuffers ------------------------------------------------- */

typedef struct {
    uint32_t addr;
    int      stride, fmt, w, h, id;
    int      drawn;
} gfb;

#define MAX_FB 32
static gfb      g_fbs[MAX_FB];
static int      g_nfb, g_next_fb = 1;
static uint32_t g_tgt_addr, g_tgt_stride;
static int      g_tgt_fmt;
static int      g_bound = -1;            /* the framebuffer bound in this frame's commands */
static uint64_t g_flip;

static int fb_bytes(int fmt) { return fmt == 3 ? 4 : 2; }

static gfb *fb_find(uint32_t addr) {
    for (int i = 0; i < g_nfb; i++) if (g_fbs[i].addr == addr) return &g_fbs[i];
    return NULL;
}

/* The framebuffer the next draw lands in, created on first use. FBP, FBW and
 * PSM arrive one at a time, so the target is only resolved at a draw. */
static gfb *fb_target(const psp_draw_state *st) {
    if (!g_tgt_addr || !g_tgt_stride) return NULL;
    if(g_tgt_stride>(uint32_t)(g_max_texture/g_scale) || g_tgt_stride>(uint32_t)(g_max_renderbuffer/g_scale)) {
        gpu_fail("Selected resolution exceeds GPU framebuffer dimensions");return NULL;
    }
    gfb *f = fb_find(g_tgt_addr);
    if (f && (f->stride != (int)g_tgt_stride || f->fmt != g_tgt_fmt)) {
        c_id *c = (c_id *)cmd_add(C_FB_FREE, sizeof *c);
        c->id = f->id;
        *f = g_fbs[--g_nfb];
        f = NULL;
        g_bound = -1;
    }
    if (!f) {
        if (g_nfb == MAX_FB) {
            c_id *c = (c_id *)cmd_add(C_FB_FREE, sizeof *c);
            c->id = g_fbs[0].id;
            g_fbs[0] = g_fbs[--g_nfb];
            g_bound = -1;
        }
        f = &g_fbs[g_nfb++];
        memset(f, 0, sizeof *f);
        f->addr = g_tgt_addr;
        f->stride = (int)g_tgt_stride;
        f->fmt = g_tgt_fmt;
        f->w = f->stride;
        f->h = st->sc_y1 + 1 > SH ? st->sc_y1 + 1 : SH;
        if (f->h > 512) f->h = 512;
        f->id = g_next_fb++;
        c_fb_new *c = (c_fb_new *)cmd_add(C_FB_NEW, sizeof *c);
        c->id = f->id;
        c->w = f->w;
        c->hgt = f->h;
        c->fmt = f->fmt;
    }
    if (g_bound != f->id) {
        c_id *c = (c_id *)cmd_add(C_FB_BIND, sizeof *c);
        c->id = f->id;
        g_bound = f->id;
    }
    f->drawn = 1;
    return f;
}

/* The framebuffer that texture address `addr` lies in, and where. */
static gfb *fb_for_texture(uint32_t addr, int *ox, int *oy) {
    if ((addr & 0x0F000000u) != 0x04000000u) return NULL;
    addr = 0x04000000u | (addr & 0x001FFFFFu);
    /* Exact targets win over an older framebuffer's overlapping allocation. */
    gfb *exact=fb_find(addr);
    if(exact && exact->drawn){*ox=*oy=0;return exact;}
    for (int i = 0; i < g_nfb; i++) {
        gfb *f = &g_fbs[i];
        const uint32_t size = (uint32_t)(f->stride * f->h * fb_bytes(f->fmt));
        if (!f->drawn || addr < f->addr || addr >= f->addr + size) continue;
        const uint32_t px = (addr - f->addr) / (uint32_t)fb_bytes(f->fmt);
        *ox = (int)(px % (uint32_t)f->stride);
        *oy = (int)(px / (uint32_t)f->stride);
        return f;
    }
    return NULL;
}

/* ---- GE side: the texture cache ------------------------------------------- */

typedef struct {
    uint32_t addr;
    int      fmt, w, h, bw, swizzle;
    uint64_t clut_key, hash;
    uint64_t checked, used;              /* g_flip of the last hash, and of the last use */
    int      id;
    int      repl;                       /* PSP_REPLACE_*: what the replacer said at its last upload */
    /* PPSSPP's maxSeenV: a texture 512 or more high drawn in through mode
     * (2D) is hashed over the rows its draws reach (at least 272), since games
     * declare 512 for a 480x272 picture; texture packs are keyed that way. 0:
     * not known yet; tex_h once a draw went further or was not 2D. */
    int      seen_v;                     /* the picture still stands for all tex_h rows */
} gtex;

static psp_texture_replacer volatile g_replacer;    /* psp_render_gl_set_replacer */
void psp_render_gl_set_replacer(psp_texture_replacer fn) { g_replacer = fn; }

#define MAX_TEX 1024
static gtex g_tex[MAX_TEX];
static int  g_ntex, g_next_tex = 1;
static int  g_last_tex = -1;             /* the cache entry of the previous draw */
static uint64_t g_uploads, g_upload_bytes;

static const int TEX_BITS[8] = { 16, 16, 16, 32, 4, 8, 16, 32 };

static uint64_t hash_bytes(const uint8_t *p, size_t n) {
    /* Four independent lanes, so the multiplies overlap. */
    uint64_t h[4] = { 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int k = 0; k < 4; k++) {
            uint64_t w;
            memcpy(&w, p + i + k * 8, 8);
            h[k] = (h[k] ^ w) * 0x100000001B3ull;
            h[k] ^= h[k] >> 29;
        }
    uint64_t r = h[0] ^ (h[1] << 1) ^ (h[2] << 2) ^ (h[3] << 3) ^ n;
    for (; i < n; i++) r = (r ^ p[i]) * 0x100000001B3ull;
    return r;
}

/* Bytes a texture covers in memory, from its address. */
static uint32_t tex_span(const psp_draw_state *st) {
    uint32_t rowb = (uint32_t)st->tex_bw * (uint32_t)TEX_BITS[st->tex_fmt] / 8;
    uint32_t rows = (uint32_t)st->tex_h;
    if (st->tex_swizzle) { rowb = (rowb + 15) & ~15u; rows = (rows + 7) & ~7u; }
    return rowb * rows;
}

static uint64_t clut_key(const psp_draw_state *st) {
    if (st->tex_fmt < PSP_TPSM_T4 || !st->clut) return 0;
    const uint64_t params = (uint64_t)st->clut_fmt | (uint64_t)st->clut_shift << 2 |
                            (uint64_t)st->clut_mask << 8 | (uint64_t)st->clut_start << 16;
    return hash_bytes(st->clut, 1024) ^ (params * 0x9E3779B97F4A7C15ull);
}

/* Palette versions are immutable resources in the recorded stream. Reusing
 * one mutable palette upload would make an earlier draw see a later CLUT,
 * particularly when scene playback makes all resources before drawing. */
enum { MAX_FB_CLUT=32 };
static struct { uint8_t bytes[1024];uint64_t hash,used;int fmt,id; } g_fb_clut[MAX_FB_CLUT];
static int g_nfb_clut;
static int framebuffer_palette(const psp_draw_state *st) {
    uint64_t hash=hash_bytes(st->clut,1024);
    for(int i=0;i<g_nfb_clut;i++)if(g_fb_clut[i].hash==hash && g_fb_clut[i].fmt==st->clut_fmt &&
       !memcmp(g_fb_clut[i].bytes,st->clut,1024)){g_fb_clut[i].used=g_flip;return g_fb_clut[i].id;}
    uint32_t *pixels=(uint32_t *)calloc(512,4);if(!pixels)return 0;
    int entries=st->clut_fmt==3?256:512;
    for(int i=0;i<entries;i++){
        const uint8_t *p=st->clut+i*(st->clut_fmt==3?4:2);
        uint32_t raw=(uint32_t)p[0]|((uint32_t)p[1]<<8);
        if(st->clut_fmt==3)raw|=((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
        pixels[i]=psp_color_expand(raw,st->clut_fmt);
    }
    int slot=g_nfb_clut;
    if(slot==MAX_FB_CLUT){
        slot=0;for(int i=1;i<MAX_FB_CLUT;i++)if(g_fb_clut[i].used<g_fb_clut[slot].used)slot=i;
        c_id *old=(c_id *)cmd_add(C_TEX_FREE,sizeof *old);old->id=g_fb_clut[slot].id;
    }else g_nfb_clut++;
    memcpy(g_fb_clut[slot].bytes,st->clut,1024);g_fb_clut[slot].hash=hash;g_fb_clut[slot].fmt=st->clut_fmt;
    g_fb_clut[slot].used=g_flip;int id=g_fb_clut[slot].id=g_next_tex++;
    c_tex_new *c=(c_tex_new *)cmd_add(C_TEX_NEW,sizeof *c);c->id=id;c->w=512;c->hgt=1;c->pixels=pixels;
    g_uploads++;g_upload_bytes+=512*4;return id;
}

static gfb *framebuffer_clut_source(const psp_draw_state *st,const psp_vertex *v,int count,int *ox,int *oy) {
    if(st->tex_fmt!=PSP_TPSM_T32 || !st->clut || st->clut_fmt<0 || st->clut_fmt>3 ||
       st->clut_shift<0 || st->clut_shift>31 || st->clut_mask<0 || st->clut_mask>255 ||
       st->clut_start<0 || st->clut_start>31 || st->tex_swizzle || (st->tex_addr&3) ||
       st->tex_w<=0 || st->tex_h<=0)return NULL;
    gfb *f=fb_for_texture(st->tex_addr,ox,oy);
    /* Only the proven one packedRGBAword per indexedtexel view. A subwidth,
     * offset row, smaller height or repeat has a different sampling domain
     * from this full framebuffer. Keep its existing RAM decoder. Tall PSP
     * declarations may cover a shorter rendered image only while this draw's
     * affine UVs stay in the rendered region. */
    if(!f || f->fmt!=PSP_TPSM_8888 || *ox || *oy || st->tex_bw!=f->stride ||
       st->tex_w!=f->stride || st->tex_h<f->h || !st->tex_clamp_u || !st->tex_clamp_v)return NULL;
    for(int i=0;i<count;i++)if(v[i].w!=0 || !isfinite(v[i].u) || !isfinite(v[i].v) ||
       v[i].u<0 || v[i].u>f->w || v[i].v<0 || v[i].v>f->h)return NULL;
    return f;
}

/* A texture pack's picture for this texture, uploaded if the host has it
 * ready: 1 if it was. t->repl says what the host answered. */
static int tex_replace(gtex *t, const psp_draw_state *st) {
    psp_texture_replacer rep = g_replacer;
    t->repl = PSP_REPLACE_NONE;
    if (!rep) return 0;
    int rw = 0, rh = 0;
    uint32_t *rpx = NULL, gl_fmt = 0, bytes = 0;
    const int hh = t->seen_v > 0 && t->seen_v < st->tex_h ? t->seen_v : st->tex_h;
    t->repl = rep(st->tex_addr, st->tex_bw, st->tex_w, hh, st->tex_fmt, st->tex_swizzle, &rw, &rh, &rpx,
                  &gl_fmt, &bytes);
    if (t->repl != PSP_REPLACE_READY) return 0;
    if (!rpx || rw <= 0 || rh <= 0 || (gl_fmt && !bytes)) {                          /* said ready, gave nothing */
        free(rpx);
        t->repl = PSP_REPLACE_NONE;
        return 0;
    }
    c_tex_new *c = (c_tex_new *)cmd_add(C_TEX_NEW, sizeof *c);
    c->id = t->id;
    c->w = rw;
    c->hgt = rh;
    c->pixels = rpx;
    c->gl_fmt = gl_fmt;
    c->bytes = bytes;
    if(g_effect_requested){c->source_addr=t->addr;c->source_hash=t->hash;c->clut_key=t->clut_key;}
    g_uploads++;
    g_upload_bytes += gl_fmt ? bytes : (uint64_t)rw * (uint64_t)rh * 4;
    return 1;
}

static void tex_upload(gtex *t, const psp_draw_state *st) {
    if (tex_replace(t, st)) return;
    uint32_t *px = (uint32_t *)malloc((size_t)st->tex_w * (size_t)st->tex_h * 4);
    if (!px) return;
    for (int y = 0; y < st->tex_h; y++)
        for (int x = 0; x < st->tex_w; x++)
            px[y * st->tex_w + x] = psp_tex_fetch(st, x, y);
    c_tex_new *c = (c_tex_new *)cmd_add(C_TEX_NEW, sizeof *c);
    c->id = t->id;
    c->w = st->tex_w;
    c->hgt = st->tex_h;
    c->pixels = px;
    if(g_effect_requested){c->source_addr=t->addr;c->source_hash=t->hash;c->clut_key=t->clut_key;}
    g_uploads++;
    g_upload_bytes += (uint64_t)st->tex_w * (uint64_t)st->tex_h * 4;
}

/* The cached texture for the current state: found, checked against memory
 * once per frame (or when it is bound anew and small), re-decoded when the
 * memory changed. Returns its id, or 0 for "no texture". max_v: how far down
 * this draw reaches, in rows, for a through-mode draw (else -1). */
static void seen_v_update(gtex *t, const psp_draw_state *st, int max_v) {
    if (st->tex_h < 512) return;
    if (max_v < 0) t->seen_v = st->tex_h;
    else if (t->seen_v == 0 && max_v > 0) t->seen_v = max_v > 272 ? max_v : 272;
    else if (max_v > t->seen_v) t->seen_v = st->tex_h;
}

static int tex_lookup(const psp_draw_state *st, int max_v) {
    if (st->tex_fmt > PSP_TPSM_T32 || st->tex_w <= 0 || st->tex_h <= 0 || st->tex_bw <= 0) return 0;
    const uint64_t ck = clut_key(st);
    gtex *t = NULL;
    int idx = -1;
    for (int i = 0; i < g_ntex; i++) {
        gtex *e = &g_tex[i];
        if (e->addr == st->tex_addr && e->fmt == st->tex_fmt && e->w == st->tex_w && e->h == st->tex_h &&
            e->bw == st->tex_bw && e->swizzle == st->tex_swizzle && e->clut_key == ck) {
            t = e; idx = i; break;
        }
    }
    const uint32_t span = tex_span(st);
    const uint8_t *mem = (const uint8_t *)psp_mem_ptr(st->tex_addr, span);
    if (!t) {
        if (g_ntex == MAX_TEX) {
            /* Full: drop the least recently used. */
            int old = 0;
            for (int i = 1; i < g_ntex; i++) if (g_tex[i].used < g_tex[old].used) old = i;
            c_id *c = (c_id *)cmd_add(C_TEX_FREE, sizeof *c);
            c->id = g_tex[old].id;
            g_tex[old] = g_tex[--g_ntex];
            if (g_last_tex == g_ntex) g_last_tex = old;
        }
        idx = g_ntex++;
        t = &g_tex[idx];
        memset(t, 0, sizeof *t);
        t->addr = st->tex_addr; t->fmt = st->tex_fmt; t->w = st->tex_w; t->h = st->tex_h;
        t->bw = st->tex_bw; t->swizzle = st->tex_swizzle; t->clut_key = ck;
        t->id = g_next_tex++;
        t->hash = mem ? hash_bytes(mem, span) : 0;
        t->checked = g_flip;
        seen_v_update(t, st, max_v);
        tex_upload(t, st);
    } else {
        const int seen = t->seen_v;
        seen_v_update(t, st, max_v);
        if (t->seen_v != seen && g_replacer) {
            /* Hashed over more rows now: another key, so the pack is asked again. */
            t->checked = g_flip;
            tex_upload(t, st);
        } else if (t->checked != g_flip || (idx != g_last_tex && span <= 32768)) {
            const uint64_t h = mem ? hash_bytes(mem, span) : t->hash;
            const int was_checked = t->checked == g_flip;
            t->checked = g_flip;
            if (h != t->hash) { t->hash = h; tex_upload(t, st); }
            /* A replacement still loading: asked again once a frame until it is there. */
            else if (t->repl == PSP_REPLACE_PENDING && !was_checked) tex_replace(t, st);
        }
    }
    t->used = g_flip;
    g_last_tex = idx;
    return t->id;
}

/* Textures nobody drew with for a while go. */
static void tex_evict(void) {
    for (int i = 0; i < g_ntex; ) {
        if (g_flip - g_tex[i].used > 600) {
            c_id *c = (c_id *)cmd_add(C_TEX_FREE, sizeof *c);
            c->id = g_tex[i].id;
            g_tex[i] = g_tex[--g_ntex];
            g_last_tex = -1;
        } else {
            i++;
        }
    }
}

/* ---- GE side: state ------------------------------------------------------ */

static const GLenum DEPTH_FUNC[8] = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL,
                                      GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL };

/* A PSP blend factor as a GL one. Codes 0 and 1 name the other colour; 6-9
 * are doubled alphas, which GL has no factor for (approximated undoubled);
 * 10 and up is the fixed colour, for which GL has one constant to share. */
static GLenum blend_factor(int code, int is_src, uint32_t fix, gstate *s, int *const_used) {
    switch (code) {
    case 0:  return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1:  return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2: case 6: return GL_SRC_ALPHA;
    case 3: case 7: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: case 8: return GL_DST_ALPHA;
    case 5: case 9: return GL_ONE_MINUS_DST_ALPHA;
    default: break;
    }
    fix &= 0xFFFFFFu;
    if (fix == 0xFFFFFFu) return GL_ONE;
    if (fix == 0) return GL_ZERO;
    const float r = (float)(fix & 0xFF) / 255.0f, g = (float)((fix >> 8) & 0xFF) / 255.0f,
                b = (float)((fix >> 16) & 0xFF) / 255.0f;
    if (*const_used) {
        if (s->bcol[0] == r && s->bcol[1] == g && s->bcol[2] == b) return GL_CONSTANT_COLOR;
        if (s->bcol[0] == 1.0f - r && s->bcol[1] == 1.0f - g && s->bcol[2] == 1.0f - b)
            return GL_ONE_MINUS_CONSTANT_COLOR;
        return GL_CONSTANT_COLOR;           /* two different fixed colours: approximate */
    }
    *const_used = 1;
    s->bcol[0] = r; s->bcol[1] = g; s->bcol[2] = b; s->bcol[3] = 1.0f;
    return GL_CONSTANT_COLOR;
}

static void rgb_of(uint32_t c, float out[3]) {
    out[0] = (float)(c & 0xFF) / 255.0f;
    out[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    out[2] = (float)((c >> 16) & 0xFF) / 255.0f;
}

/* ---- GE side: the backend ------------------------------------------------ */

static uint64_t g_draws, g_batches;
static size_t   g_last_draw = (size_t)-1;   /* offset of this frame's last C_DRAW */

/* Opt-in, bounded evidence for a live title: seconds from renderer startup,
 * sampling interval in seconds, number of complete GE frames. No GL readback.
 * PSPRECOMP_GL_CENSUS=90,2,3 records frames near seconds90,92,94. */
static uint64_t g_census_start,g_census_next,g_census_interval,g_census_flip;
static unsigned g_census_frames,g_census_lines;
static int g_census_enabled,g_census_active;
static void census_init(void) {
    g_census_enabled=g_census_active=0;g_census_lines=0;g_census_flip=UINT64_MAX;
    const char *e=getenv("PSPRECOMP_GL_CENSUS");
    if(!e || !*e)return;
    unsigned start=90,interval=2,frames=3;
    sscanf(e,"%u,%u,%u",&start,&interval,&frames);
    if(!frames)return;if(frames>8)frames=8;
    g_census_start=host_pacing_now();g_census_next=g_census_start+(uint64_t)start*1000000;
    g_census_interval=(uint64_t)(interval?interval:1)*1000000;g_census_frames=frames;g_census_enabled=1;
}
static void census_draw(int prim,int count,const psp_vertex *v,const gfb *fb,
                        const psp_draw_state *st,const gstate *s) {
    if(!g_census_enabled || g_census_lines>=4096)return;
    uint64_t now=host_pacing_now();
    if(g_census_flip!=g_flip) {
        g_census_flip=g_flip;g_census_active=0;
        if(g_census_frames && now>=g_census_next){g_census_frames--;g_census_active=1;g_census_next=now+g_census_interval;}
    }
    if(!g_census_active)return;
    float z0=v[0].z,z1=z0;
    for(int i=1;i<count;i++){if(v[i].z<z0)z0=v[i].z;if(v[i].z>z1)z1=v[i].z;}
    fprintf(stderr,"gl census: t=%.3f frame=%llu draw=%llu prim=%d/%d fb=%08X/%d/f%d/id%d "
        "z=%08X/%d/test%d/f%d/write%d/clear%d%d%d%d/range%g,%g glz=%d/%X/%d "
        "tex=%d/%08X/f%d/bw%d/%dx%d/swz%d/fn%d/a%d/kind%d/id%d "
        "blend=%d/%d,%d,%d/%06X,%06X mask=%08X stencil=%d/%d/%02X/%02X/%d,%d,%d\n",
        (double)(now-g_census_start)/1000000.0,(unsigned long long)g_flip,(unsigned long long)g_draws,
        prim,count,fb->addr,fb->stride,fb->fmt,fb->id,st->zb_addr,st->zb_stride,st->ztest,st->ztest_func,
        st->zwrite,st->clear,st->clear_color,st->clear_alpha,st->clear_depth,z0,z1,s->depth_test,s->depth_func,s->depth_mask,
        st->tex_enable,st->tex_addr,st->tex_fmt,st->tex_bw,st->tex_w,st->tex_h,st->tex_swizzle,st->tex_func,st->tex_alpha,s->tex_kind,s->tex_id,
        st->blend,st->blend_src,st->blend_dst,st->blend_op,st->blend_sfix,st->blend_dfix,st->write_mask,
        st->stencil,st->stencil_func,st->stencil_ref,st->stencil_mask,st->stencil_fail,st->stencil_zfail,st->stencil_zpass);
    g_census_lines++;
}

static void gl_target(uint32_t addr, uint32_t stride, int fmt) {
    g_tgt_addr = addr;
    g_tgt_stride = stride;
    g_tgt_fmt = fmt;
}

static void framebuffer_failure(const char *reason,const gfb *fb,const psp_draw_state *st) {
    char error[512];
    snprintf(error,sizeof error,
        "%s: fmt=%d addr=%08X stride=%d clear=%d/%d/%d/%d blend=%d(%d,%d,%d) "
        "stencil=%d(func=%d ref=%02X mask=%02X ops=%d,%d,%d) write=%08X tex=%d/%d/%08X z=%d/%d/%d",
        reason,fb->fmt,fb->addr,fb->stride,st->clear,st->clear_color,st->clear_alpha,st->clear_depth,
        st->blend,st->blend_src,st->blend_dst,st->blend_op,st->stencil,st->stencil_func,
        st->stencil_ref&255,st->stencil_mask&255,st->stencil_fail,st->stencil_zfail,st->stencil_zpass,
        st->write_mask,st->tex_enable,st->tex_fmt,st->tex_addr,st->ztest,st->ztest_func,st->zwrite);
    gpu_fail(error);
}

static void put_vertex(gframe *f, const psp_vertex *v, uint32_t rgba, const gfb *fb,
                       float us, float vs, float uo, float vo,int pose) {
    gvert *o = (gvert *)gbuf_add(&f->vtx, sizeof *o);
    const float W = v->w > 0.0f ? 1.0f / v->w : 1.0f;
    o->x = (v->fx / (float)fb->w * 2.0f - 1.0f) * W;
    o->y = (v->fy / (float)fb->h * 2.0f - 1.0f) * W;
    o->z = (v->z / 65535.0f * 2.0f - 1.0f) * W;
    o->w = W;
    o->rgba = rgba;
    o->u = (v->u + uo) * us;
    o->v = (v->v + vo) * vs;
    o->fog = v->fog;
    o->depth = v->z;
    o->depth_base = 0;
    o->local_valid=pose&&v->local_valid;
    if(o->local_valid)memcpy(o->local,v->local,sizeof o->local);else memset(o->local,0,sizeof o->local);
}

static void gl_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    if(count<=0 || !v || !st)return;
    if (prim != PSP_PRIM_SPRITES && (prim < PSP_PRIM_TRIANGLES || prim > PSP_PRIM_TRIANGLE_FAN)) return;
    if (!g_started) return;
    gfb *fb = fb_target(st);
    if (!fb) return;
    if(fb->fmt<0 || fb->fmt>3){framebuffer_failure("Unsupported framebuffer format",fb,st);return;}
    /* Quantization runs before fixed-function blending; reduced-precision
     * blending needs a separate path that quantizes the final blend result.
     * Mutating stencil also has format-specific increment/saturation rules. */
    if(fb->fmt!=3 && !st->clear && (st->blend || (st->stencil &&
       (st->stencil_fail || st->stencil_zfail || st->stencil_zpass)))) {
        framebuffer_failure("16-bit framebuffer blending and mutating stencil need an additional GPU path",fb,st);return;
    }
    g_draws++;

    gstate s;
    memset(&s, 0, sizeof s);
    s.source_factor[0]=s.source_factor[1]=s.source_factor[2]=1.0f;
    s.zb_addr=st->zb_addr;s.zb_stride=st->zb_stride;
    if(fb->fmt==1)s.mode[0]|=MODE_5551;
    if(fb->fmt==0)s.mode[0]|=MODE_565;
    if(fb->fmt==2)s.mode[0]|=MODE_4444;
    if(st->depth_clamp)s.mode[0]|=MODE_DEPTHCLAMP;
    if(st->depth_range_enable && (st->depth_range_min>0 || st->depth_range_max<65535)) {
        s.mode[0]|=MODE_DEPTHRANGE;
        s.depth_range[0]=(float)st->depth_range_min;s.depth_range[1]=(float)st->depth_range_max;
    }
    s.sc[0] = st->sc_x0;
    s.sc[1] = st->sc_y0;
    s.sc[2] = st->sc_x1 - st->sc_x0 + 1;
    s.sc[3] = st->sc_y1 - st->sc_y0 + 1;
    if (s.sc[2] <= 0 || s.sc[3] <= 0) return;
    for(int k=0;k<3;k++) {
        unsigned mask=(st->write_mask>>(8*k))&255;
        if(mask && mask!=255){gpu_fail("Partial-bit RGB write masks need an additional GPU path");return;}
        s.cmask[k]=(!st->clear || st->clear_color) && mask!=255;
    }
    /* PSP framebuffer alpha is stencil, never ordinary source alpha. */
    s.cmask[3]=0;s.stencil_write=(int32_t)((~st->write_mask>>24)&255);
    if(fb->fmt==1)s.stencil_write=(s.stencil_write&128)?255:0;
    if(fb->fmt==0)s.stencil_write=0; /* no stored stencil in RGB565 */
    if(fb->fmt==2)s.stencil_write=(s.stencil_write&240)|((s.stencil_write&240)>>4);
    static const GLenum SOP[6]={GL_KEEP,GL_ZERO,GL_REPLACE,GL_INVERT,GL_INCR,GL_DECR};
    if(st->clear && st->clear_alpha && fb->fmt!=0) {
        unsigned alpha=v[count-1].rgba>>24;
        for(int i=0;i<count;i++)if(v[i].rgba>>24!=alpha){gpu_fail("Varying-alpha clear geometry is unsupported");return;}
        if(fb->fmt==1)alpha=alpha&128?255:0;
        if(fb->fmt==2)alpha=(alpha&240)|(alpha>>4);
        s.stencil=1;s.stencil_func=GL_ALWAYS;s.stencil_ref=(int32_t)alpha;s.stencil_mask=255;
        s.stencil_fail=s.stencil_zfail=s.stencil_zpass=GL_REPLACE;
    } else if(!st->clear && st->stencil) {
        s.stencil=1;s.stencil_func=DEPTH_FUNC[st->stencil_func&7];
        s.stencil_ref=st->stencil_ref&255;s.stencil_mask=st->stencil_mask&255;
        s.stencil_fail=SOP[st->stencil_fail>=0&&st->stencil_fail<6?st->stencil_fail:0];
        s.stencil_zfail=SOP[st->stencil_zfail>=0&&st->stencil_zfail<6?st->stencil_zfail:0];
        s.stencil_zpass=SOP[st->stencil_zpass>=0&&st->stencil_zpass<6?st->stencil_zpass:0];
    }

    float us = 0, vs = 0, uo = 0, vo = 0;
    if (st->clear) {
        s.mode[0] |= MODE_CLEAR;
        if (st->clear_depth) { s.depth_test = 1; s.depth_func = GL_ALWAYS; s.depth_mask = 1; }
    } else {
        if (st->tex_enable) {
            int ox, oy;
            gfb *src = st->tex_fmt <= PSP_TPSM_8888 ? fb_for_texture(st->tex_addr, &ox, &oy) : framebuffer_clut_source(st,v,count,&ox,&oy);
            if(src && st->tex_fmt==PSP_TPSM_T32 && !(s.clut[0]=framebuffer_palette(st)))src=NULL;
            if (src) {
                s.tex_kind = st->tex_fmt==PSP_TPSM_T32?4:src==fb?3:2;
                s.tex_id = src->id;
                if(s.tex_kind==4){s.clut[1]=st->clut_shift;s.clut[2]=st->clut_mask;s.clut[3]=st->clut_start;}
                us = 1.0f / (float)src->w; vs = 1.0f / (float)src->h;
                uo = (float)ox; vo = (float)oy;
                s.wrap_s = s.wrap_t = GL_CLAMP_TO_EDGE;
            } else {
                /* Through mode (2D: no w) on a tall texture: how far down it reaches. */
                int max_v = -1;
                if (st->tex_h >= 512) {
                    float mv = 0;
                    int through = 1;
                    for (int i = 0; i < count && through; i++) {
                        if (v[i].w != 0.0f) through = 0;
                        if (v[i].v > mv) mv = v[i].v;
                    }
                    if (through) max_v = (int)ceilf(mv);
                }
                if ((s.tex_id = tex_lookup(st, max_v)) != 0) {
                    s.tex_kind = 1;
                    us = 1.0f / (float)st->tex_w; vs = 1.0f / (float)st->tex_h;
                    s.wrap_s = st->tex_clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                    s.wrap_t = st->tex_clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                }
            }
            if (s.tex_kind) {
                s.mode[0] |= MODE_TEX | (st->tex_alpha ? MODE_TEXALPHA : 0) | (st->tex_double ? MODE_DOUBLE : 0);
                s.mode[1] = st->tex_func;
                s.min_f = st->tex_min_linear ? GL_LINEAR : GL_NEAREST;
                s.mag_f = st->tex_mag_linear ? GL_LINEAR : GL_NEAREST;
                rgb_of(st->tex_env, s.env);
            }
        }
        if (st->fog) { s.mode[0] |= MODE_FOG; rgb_of(st->fog_color, s.fogc); }
        if (st->atest) {
            s.mode[0] |= MODE_ATEST;
            s.mode[2] = st->atest_func;
            s.mode[3] = (st->atest_ref & 0xFF) | (st->atest_mask & 0xFF) << 8;
        }
        if (st->ztest && st->zb_stride) {
            s.depth_test = 1;
            s.depth_func = (int32_t)DEPTH_FUNC[st->ztest_func & 7];
            s.depth_mask = st->zwrite;
        }
        if (st->blend) {
            int const_used = 0;
            s.blend = 1;
            s.bsrc = (int32_t)blend_factor(st->blend_src, 1, st->blend_sfix, &s, &const_used);
            s.bdst = (int32_t)blend_factor(st->blend_dst, 0, st->blend_dfix, &s, &const_used);
            /* GL exposes one constant color, but the PSP exposes independent
             * SFIX/DFIX. Apply SFIX after alpha testing in the shader and keep
             * the GL constant for DFIX. MIN/MAX ignore both factors. */
            if(st->blend_src>=10 && st->blend_dst>=10 && st->blend_op<3) {
                rgb_of(st->blend_sfix,s.source_factor);s.bsrc=GL_ONE;const_used=0;
                s.bdst=(int32_t)blend_factor(st->blend_dst,0,st->blend_dfix,&s,&const_used);
            }
            static const GLenum EQ[6] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT,
                                          GL_MIN, GL_MAX, GL_FUNC_ADD };
            s.beq = (int32_t)EQ[st->blend_op < 6 ? st->blend_op : 0];
            s.needs_dest_alpha=s.bsrc==GL_DST_ALPHA || s.bsrc==GL_ONE_MINUS_DST_ALPHA || s.bdst==GL_DST_ALPHA || s.bdst==GL_ONE_MINUS_DST_ALPHA;
            if(s.needs_dest_alpha && s.stencil && s.stencil_write &&
               (s.stencil_fail!=GL_KEEP || s.stencil_zfail!=GL_KEEP || s.stencil_zpass!=GL_KEEP)) {
                gpu_fail("Destination-alpha blending with simultaneous stencil updates needs per-fragment feedback");return;
            }
        }
    }

    census_draw(prim,count,v,fb,st,&s);
    /* Everything becomes a triangle list. Flat shading takes each triangle's
     * last vertex colour, as ge.c's clipper also keeps it last. */
    gframe *f = &g_frames[g_rec];
    const uint32_t first = (uint32_t)(f->vtx.n / sizeof(gvert));
    const int flat = !st->gouraud && !st->clear;
#define TRI(a, b, c) do { \
        const uint32_t pc = (c)->rgba; \
        put_vertex(f, (a), flat ? pc : (a)->rgba, fb, us, vs, uo, vo,st->transform.valid); \
        put_vertex(f, (b), flat ? pc : (b)->rgba, fb, us, vs, uo, vo,st->transform.valid); \
        put_vertex(f, (c), pc, fb, us, vs, uo, vo,st->transform.valid); \
    } while (0)
    switch (prim) {
    case PSP_PRIM_TRIANGLES:
        for (int i = 0; i + 2 < count; i += 3) TRI(&v[i], &v[i + 1], &v[i + 2]);
        break;
    case PSP_PRIM_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < count; i++) TRI(&v[i], &v[i + 1], &v[i + 2]);
        break;
    case PSP_PRIM_TRIANGLE_FAN:
        for (int i = 1; i + 1 < count; i++) TRI(&v[0], &v[i], &v[i + 1]);
        break;
    default:
        /* A sprite: two corners of a rectangle, coloured, deep and fogged by
         * the second; texture coordinates run corner to corner. */
        for (int i = 0; i + 1 < count; i += 2) {
            const psp_vertex *a = &v[i], *b = &v[i + 1];
            psp_vertex c[4];
            for (int k = 0; k < 4; k++) {
                c[k] = *b;
                c[k].w = 0;
            }
            c[0].fx = a->fx; c[0].fy = a->fy; c[0].u = a->u; c[0].v = a->v;
            c[1].fx = b->fx; c[1].fy = a->fy; c[1].u = b->u; c[1].v = a->v;
            c[2].fx = a->fx; c[2].fy = b->fy; c[2].u = a->u; c[2].v = b->v;
            c[3].fx = b->fx; c[3].fy = b->fy; c[3].u = b->u; c[3].v = b->v;
            TRI(&c[0], &c[1], &c[2]);
            TRI(&c[1], &c[3], &c[2]);
        }
        break;
    }
#undef TRI
    const uint32_t n = (uint32_t)(f->vtx.n / sizeof(gvert)) - first;
    if (!n) return;

    /* Same state, right after the previous draw with nothing in between: one
     * batch. */
    if (s.tex_kind!=3 && s.tex_kind!=4 && g_last_draw != (size_t)-1 && g_last_draw + ((sizeof(c_draw) + 7) & ~(size_t)7) == f->cmd.n) {
        c_draw *d = (c_draw *)(f->cmd.p + g_last_draw);
        if (d->first + d->count == first && !memcmp(&d->s, &s, sizeof s)) {
            int plain=!st->transform.valid&&!d->transform.valid;
            int rigid=st->transform.valid&&d->transform.valid&&
                st->transform.instance==d->transform.instance&&
                st->transform.vertex_format==d->transform.vertex_format&&st->transform.primitive==d->transform.primitive&&
                !memcmp(st->transform.world,d->transform.world,sizeof st->transform.world)&&
                !memcmp(st->transform.view,d->transform.view,sizeof st->transform.view)&&
                !memcmp(st->transform.proj,d->transform.proj,sizeof st->transform.proj)&&
                !memcmp(st->transform.viewport,d->transform.viewport,sizeof st->transform.viewport);
            if(plain||rigid){
                if(rigid){d->transform.mesh=d->transform.mesh*16777619u^st->transform.mesh;d->transform.indices=d->transform.indices*16777619u^st->transform.indices;d->transform.count+=st->transform.count;}
                d->count+=n;if(g_effect_requested){d->tex_addr_last=st->tex_addr;d->source_draws++;}return;
            }
        }
    }
    g_last_draw = f->cmd.n;
    c_draw *d = (c_draw *)cmd_add(C_DRAW, sizeof *d);
    d->s = s;
    d->first = first;
    d->count = n;
    d->transform=st->transform;
    if(g_effect_requested){
        d->fb_addr=fb->addr;d->fb_stride=fb->stride;d->fb_fmt=fb->fmt;
        d->tex_addr=d->tex_addr_last=st->tex_addr;d->source_draws=1;
    }
    g_batches++;
}

/* sceDisplaySetFrameBuf: hand the recorded frame to the render thread, with
 * what to show -- the FBO at the display address, or (for a framebuffer the
 * GE never drew, like a CPU-drawn one) guest memory. */
static void gl_present(void) {
    if (!g_started) return;
    uint32_t addr, stride, fmt;
    psp_display_get(&addr, &stride, &fmt);
    /* VRAM addresses fold their mirrors together; a framebuffer in main RAM
     * (MHFU's movies show the decoder's output straight from RAM) is left as
     * it is, and is never an FBO, so it is shown from memory. */
    if ((addr & 0x0F000000u) == 0x04000000u) addr = 0x04000000u | (addr & 0x001FFFFFu);
    c_present *c = (c_present *)cmd_add(C_PRESENT, sizeof *c);
    gfb *f = fb_find(addr);
    if (f && f->drawn) {
        c->fb = f->id;
    } else {
        c->fb = -1;
        const uint32_t bpp = fmt == 3 ? 4 : 2;
        const uint8_t *src = (const uint8_t *)psp_mem_ptr(addr, stride * SH * bpp);
        uint32_t *px = src && stride ? (uint32_t *)malloc(SW * SH * 4) : NULL;
        if (px)
            for (int y = 0; y < SH; y++)
                for (int x = 0; x < SW; x++) {
                    const uint8_t *p = src + ((size_t)y * stride + (size_t)x) * bpp;
                    const uint32_t raw = bpp == 4 ? (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24
                                                  : (uint32_t)(p[0] | p[1] << 8);
                    px[y * SW + x] = psp_color_expand(raw, (int)fmt) | 0xFF000000u;
                }
        c->pixels = px;
    }

    g_flip++;
    if ((g_flip & 63) == 0) tex_evict();

    host_mutex_lock(&g_m);
    while (g_pending >= 0) host_cond_wait(&g_cv, &g_m);
    g_pending = g_rec;
    host_cond_broadcast(&g_cv);
    host_mutex_unlock(&g_m);

    g_rec ^= 1;
    g_frames[g_rec].cmd.n = 0;
    g_frames[g_rec].vtx.n = 0;
    g_bound = -1;
    g_last_tex = -1;
    g_last_draw = (size_t)-1;
}

static int gl_capture(uint32_t *rgba) {
    if(!g_started)return -1;
    host_mutex_lock(&g_m);
    uint64_t request=++g_capture_requested;host_cond_broadcast(&g_cv);
    while(g_capture_done<request && !g_quit && !g_error[0])host_cond_wait(&g_cv,&g_m);
    const int have = g_have_capture && !g_error[0] && g_capture_done>=request;
    if (have) memcpy(rgba, g_capture, sizeof g_capture);
    host_mutex_unlock(&g_m);
    return have ? 0 : -1;
}

void psp_render_gl_overlay(const uint32_t *rgba, int w, int h) { psp_render_gl_overlay_scaled(rgba, w, h, w, h); }

void psp_render_gl_overlay_scaled(const uint32_t *rgba, int w, int h, int dst_w, int dst_h) {
    host_mutex_lock(&g_m);
    free(g_ovl);
    g_ovl = NULL;
    g_ovl_w = g_ovl_h = 0;
    if (rgba && w > 0 && h > 0 && (g_ovl = (uint32_t *)malloc((size_t)w * (size_t)h * 4)) != NULL) {
        memcpy(g_ovl, rgba, (size_t)w * (size_t)h * 4);
        g_ovl_w = w;
        g_ovl_h = h;
        g_ovl_dw = dst_w > 0 ? dst_w : w;
        g_ovl_dh = dst_h > 0 ? dst_h : h;
    }
    g_ovl_dirty = 1;
    host_mutex_unlock(&g_m);
}

static int  gl_init(int w, int h) { (void)w; (void)h; return g_started ? 0 : -1; }
static void gl_noop(void) { }

const psp_render_backend psp_render_gl = {
    "gl", gl_init, psp_render_gl_stop, gl_target, gl_draw, gl_noop, gl_present, gl_capture
};

/* ---- render thread ------------------------------------------------------- */

static const char *VS_DRAW =
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec4 a_col;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "layout(location = 3) in float a_fog;\n"
    "out vec4 v_col;\n"
    "out vec2 v_uv;\n"
    "out float v_fog;\n"
    "layout(location = 4) in vec2 a_depth;\n"
    "uniform ivec4 u_mode;\n"
    "out float v_affine_w;\n"
    "out float v_depth;\n"
    "flat out float v_depth_base;\n"
    "void main() { gl_Position = a_pos;\n"
    /* GE owns range rejection/near clipping. Keep raster depth coherent with
     * PSP depth too, rather than giving every triangle window depth0.5.
     * Clamp only the clip carrier: valid raw65535.5 still reaches the exact
     * integer16 fragment-depth path below and must not be clipped away. */
    "    gl_Position.z = clamp(a_pos.z, -a_pos.w, a_pos.w);\n"
    "    v_affine_w = a_pos.w; v_depth = (a_depth.x - a_depth.y) * a_pos.w; v_depth_base = a_depth.y;\n"
    "    v_col = a_col * a_pos.w; v_uv = a_uv; v_fog = a_fog * a_pos.w; }\n";

/* The pixel pipeline of render.c's fragment(), in the same order. */
static const char *FS_DRAW =
    "uniform sampler2D u_tex;\n"
    "uniform ivec4 u_mode;\n"
    "uniform vec3 u_env;\n"
    "uniform vec3 u_fog;\n"
    "uniform vec3 u_source_factor;\n"
    "uniform vec2 u_depth_range;\n"
    "in vec4 v_col;\n"
    "in vec2 v_uv;\n"
    "in float v_fog;\n"
    "in float v_affine_w;\n"
    "in float v_depth;\n"
    "flat in float v_depth_base;\n"
    "out vec4 o_col;\n"
    "vec4 quantize_output(vec4 c){\n"
    "    ivec3 b=ivec3(clamp(c.rgb,0.0,1.0)*255.0+0.5);\n"
    "    if((u_mode.x & 64)!=0){b>>=3;c.rgb=vec3((b<<3)|(b>>2))/255.0;}\n"
    "    else if((u_mode.x & 128)!=0){b>>=ivec3(3,2,3);c.rgb=vec3((b<<ivec3(3,2,3))|(b>>ivec3(2,4,2)))/255.0;}\n"
    "    else if((u_mode.x & 256)!=0){b>>=4;c.rgb=vec3((b<<4)|b)/255.0;}\n"
    "    return c;\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = v_col / v_affine_w;\n"
    "    float integer_depth = floor(clamp(v_depth_base + v_depth / v_affine_w, 0.0, 65535.0));\n"
    "    gl_FragDepth = integer_depth / 65535.0;\n"
    "    int f = u_mode.x;\n"
    "    if ((f & 1024) != 0 && (integer_depth < u_depth_range.x || integer_depth > u_depth_range.y)) discard;\n"
    "    if ((f & 32) != 0) { o_col = quantize_output(c); return; }\n"
    "    if ((f & 1) != 0) {\n"
    "        vec4 t = texture(u_tex, v_uv);\n"
    "        bool ta = (f & 2) != 0;\n"
    "        int fn = u_mode.y;\n"
    "        vec3 rgb;\n"
    "        float a = c.a;\n"
    "        if (fn == 0)      { rgb = t.rgb * c.rgb; if (ta) a = t.a * c.a; }\n"
    "        else if (fn == 1) { rgb = ta ? mix(c.rgb, t.rgb, t.a) : t.rgb; }\n"
    "        else if (fn == 2) { rgb = mix(c.rgb, u_env, t.rgb); if (ta) a = t.a * c.a; }\n"
    "        else if (fn == 3) { rgb = t.rgb; if (ta) a = t.a; }\n"
    "        else              { rgb = t.rgb + c.rgb; if (ta) a = t.a * c.a; }\n"
    "        if ((f & 4) != 0) rgb *= 2.0;\n"
    "        c = vec4(clamp(rgb, 0.0, 1.0), a);\n"
    "    }\n"
    "    if ((f & 8) != 0) c.rgb = mix(u_fog, c.rgb, clamp(v_fog / v_affine_w, 0.0, 1.0));\n"
    "    if ((f & 16) != 0) {\n"
    "        int m = (u_mode.w >> 8) & 255;\n"
    "        int a = int(c.a * 255.0 + 0.5) & m;\n"
    "        int r = u_mode.w & m;\n"
    "        int fn = u_mode.z;\n"
    "        bool pass = fn == 1 || (fn == 2 && a == r) || (fn == 3 && a != r) || (fn == 4 && a < r) ||\n"
    "                    (fn == 5 && a <= r) || (fn == 6 && a > r) || (fn == 7 && a >= r);\n"
    "        if (!pass) discard;\n"
    "    }\n"
    "    c.rgb *= u_source_factor;\n"
    "    o_col = quantize_output(c);\n"
    "}\n";

static const char *VS_BLIT =
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() { gl_Position = a_pos; v_uv = a_uv; }\n";

static const char *FS_BLIT =
    "uniform sampler2D u_tex;\n"
    "uniform ivec4 u_mode;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "void main() { vec4 t = texture(u_tex, v_uv); o_col = u_mode.x != 0 ? t : vec4(t.rgb, 1.0); }\n";

typedef struct { GLint wrap_s,wrap_t,min_f,mag_f; int valid; } rsampler;
typedef struct { GLuint name; rsampler sampler;
    uint32_t source_addr,upload;uint64_t source_hash,clut_key;int w,h; } rtex;
typedef struct {
    GLuint fbo,tex,depth,feedback_fbo,feedback_tex,clut_fbo,clut_tex;
    int w,h,stencil_dirty,fmt;             /* w, h in real pixels */
    rsampler sampler,feedback_sampler,clut_sampler;
    uint64_t color_version,clut_version;
    int clut_params[4];
    int clut_region[4]; /* physical texels valid for this version and palette */
    int depth_id,depth_valid;             /* logical depth currently resident here */
    uint64_t depth_version;
} rfb;

static rtex   *g_rtex;        /* texture id -> GL object and its sampler state */
static int     g_nrtex;
static rfb    *g_rfb;         /* framebuffer id -> objects */
static int     g_nrfb;
/* PSP depth storage is keyed independently from color/stencil. Each colorFBO
 * keeps its own packed attachment (required by GLES3); depth-only GPU blits
 * exchange its depth with the shared allocation when a target changes. */
typedef struct { uint32_t addr; int stride,w,h; GLuint fbo,depth; uint64_t version; } rzbuf;
static rzbuf *g_rzb;
static int g_nrzb,g_rzb_capacity,g_depth_fb=-1,g_depth_id=-1,g_depth_dirty;
static GLuint  g_prog, g_blit, g_vao, g_vbo, g_bvao, g_bvbo, g_show_tex, g_ovl_tex, g_cap_fbo, g_cap_tex;
static GLint   g_u_mode, g_u_env, g_u_fog, g_u_source_factor, g_b_mode;
static GLint   g_u_depth_range;
static int     g_cur_fb_h;
static int     g_cur_fb_id=-1;
static GLuint g_resolve;
static GLint g_resolve_alpha;
static GLuint g_depal;
static GLint g_depal_mode;
static const char *VS_RESOLVE=
    "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
static const char *FS_RESOLVE=
    "uniform float u_alpha;out vec4 o_col;void main(){o_col=vec4(0.0,0.0,0.0,u_alpha);}\n";
/* Integer lookup happens before ordinary texture filtering. The input is an
 * RGBA8888 packed PSP word, not a filtered floating-point palette index. */
static const char *FS_DEPAL=
    "uniform sampler2D u_tex;uniform sampler2D u_palette;uniform ivec4 u_clut;out vec4 o_col;\n"
    "void main(){uvec4 c=uvec4(floor(texelFetch(u_tex,ivec2(gl_FragCoord.xy),0)*255.0+0.5));\n"
    "uint word=c.r|(c.g<<8u)|(c.b<<16u)|(c.a<<24u);\n"
    "uint index=((word>>uint(u_clut.x))&uint(u_clut.y))|uint(u_clut.z<<4);\n"
    "o_col=index<512u?texelFetch(u_palette,ivec2(int(index),0),0):vec4(0.0);}\n";

static void *grow(void *p, int *n, int need, size_t one) {
    if (need < *n) return p;
    int m = *n ? *n : 64;
    while (m <= need) m *= 2;
    p = realloc(p, (size_t)m * one);
    if (!p) { fputs("render_gl: out of memory\n", stderr); abort(); }
    memset((uint8_t *)p + (size_t)*n * one, 0, (size_t)(m - *n) * one);
    *n = m;
    return p;
}

/* GPU-worker-owned, bounded visibility evidence; all hosts explicitly opt
 * in with ASPHALT_EFFECT_DIAGNOSTICS=1. Queries do not change draw output. */
enum { EFFECT_QUERY_SLOTS=128,EFFECT_QUERY_INTERVAL=120,EFFECT_QUERY_FRAME_MAX=64,EFFECT_QUERY_MAX=2048 };
typedef struct {
    gstate state;GLuint name,fb,texture;
    uint64_t frame,texture_hash,clut_key;
    uint32_t fb_addr,tex_addr,tex_addr_last,texture_source,upload,vertices,source_draws;
    int fb_stride,fb_fmt,texture_w,texture_h,pending;
} effect_query;
static struct {
    effect_query slots[EFFECT_QUERY_SLOTS];GLuint names[EFFECT_QUERY_SLOTS];
    uint64_t frame;unsigned context,enabled,issued,results,pending,frame_samples,cursor,ring_skips,limit_logged;
} g_effect;
static unsigned g_effect_context;

static void effect_diag_poll(void) {
    if(!g_effect.enabled)return;
    for(unsigned i=0;i<EFFECT_QUERY_SLOTS;i++) {
        effect_query *q=&g_effect.slots[i];
        if(!q->pending || q->frame>=g_effect.frame)continue;
        GLuint available=0;effectGetQueryObjectuiv(q->name,GL_QUERY_RESULT_AVAILABLE,&available);
        if(!available)continue;
        GLuint result=0;effectGetQueryObjectuiv(q->name,GL_QUERY_RESULT,&result);
        const gstate *s=&q->state;
        fprintf(stderr,"effect-vis,v=1,event=result,context=%u,frame=%llu,read_frame=%llu,query=%u,samples=%u,vertices=%u,sources=%u,fb_addr=%08X,fb_id=%u,fb_stride=%d,fb_fmt=%d,zb=%08X,zb_stride=%d,tex_addr=%08X,tex_last=%08X,tex_kind=%d,tex_id=%d,tex_gl=%u,tex_source=%08X,upload=%u,tex_size=%d:%d,tex_hash=%016llX,clut_key=%016llX,mode=%d:%d:%d:%d,depth=%d:%X:%d,range=%g:%g,stencil=%d:%X:%d:%d:%d:%X:%X:%X,scissor=%d:%d:%d:%d,blend=%d:%X:%X:%X,blend_constant=%g:%g:%g:%g,source_factor=%g:%g:%g,color_mask=%d:%d:%d:%d,sampler=%X:%X:%X:%X\n",
            g_effect.context,(unsigned long long)q->frame,(unsigned long long)g_effect.frame,q->name,!!result,q->vertices,q->source_draws,
            q->fb_addr,q->fb,q->fb_stride,q->fb_fmt,s->zb_addr,s->zb_stride,q->tex_addr,q->tex_addr_last,s->tex_kind,s->tex_id,q->texture,q->texture_source,q->upload,q->texture_w,q->texture_h,
            (unsigned long long)q->texture_hash,(unsigned long long)q->clut_key,s->mode[0],s->mode[1],s->mode[2],s->mode[3],s->depth_test,s->depth_func,s->depth_mask,
            s->mode[0]&MODE_DEPTHRANGE?s->depth_range[0]:0,s->mode[0]&MODE_DEPTHRANGE?s->depth_range[1]:65535,
            s->stencil,s->stencil_func,s->stencil_ref,s->stencil_mask,s->stencil_write,s->stencil_fail,s->stencil_zfail,s->stencil_zpass,
            s->sc[0],s->sc[1],s->sc[2],s->sc[3],s->blend,s->bsrc,s->bdst,s->beq,s->bcol[0],s->bcol[1],s->bcol[2],s->bcol[3],
            s->source_factor[0],s->source_factor[1],s->source_factor[2],s->cmask[0],s->cmask[1],s->cmask[2],s->cmask[3],s->wrap_s,s->wrap_t,s->min_f,s->mag_f);
        q->pending=0;g_effect.pending--;g_effect.results++;
    }
}
static void effect_diag_init(void) {
    memset(&g_effect,0,sizeof g_effect);
    if(!g_effect_requested)return;
    g_effect.context=++g_effect_context;
    effectGenQueries=(void (GLCALL *)(GLsizei,GLuint *))g_host.get_proc(g_host.user,"glGenQueries");
    effectDeleteQueries=(void (GLCALL *)(GLsizei,const GLuint *))g_host.get_proc(g_host.user,"glDeleteQueries");
    effectBeginQuery=(void (GLCALL *)(GLenum,GLuint))g_host.get_proc(g_host.user,"glBeginQuery");
    effectEndQuery=(void (GLCALL *)(GLenum))g_host.get_proc(g_host.user,"glEndQuery");
    effectGetQueryObjectuiv=(void (GLCALL *)(GLuint,GLenum,GLuint *))g_host.get_proc(g_host.user,"glGetQueryObjectuiv");
    if(!effectGenQueries||!effectDeleteQueries||!effectBeginQuery||!effectEndQuery||!effectGetQueryObjectuiv) {
        fprintf(stderr,"effect-vis,v=1,event=unsupported,context=%u,reason=query_symbols\n",g_effect.context);return;
    }
    effectGenQueries(EFFECT_QUERY_SLOTS,g_effect.names);
    GLenum error=glGetError();int complete=error==0;
    for(unsigned i=0;i<EFFECT_QUERY_SLOTS;i++)if(!g_effect.names[i])complete=0;
    if(!complete) {
        effectDeleteQueries(EFFECT_QUERY_SLOTS,g_effect.names);
        /* setup() already consumed required-driver errors; discard only this
         * optional allocation's errors, never make it a renderer failure. */
        for(unsigned i=0;i<4&&glGetError()!=0;i++){}
        fprintf(stderr,"effect-vis,v=1,event=unsupported,context=%u,reason=query_allocation,error=%X\n",g_effect.context,error);return;
    }
    g_effect.enabled=1;
    fprintf(stderr,"effect-vis,v=1,event=header,context=%u,interval=%u,frame_max=%u,total_max=%u,ring=%u,scale=%d,api=%s\n",g_effect.context,EFFECT_QUERY_INTERVAL,EFFECT_QUERY_FRAME_MAX,EFFECT_QUERY_MAX,EFFECT_QUERY_SLOTS,g_scale,g_host.gles?"gles":"gl");
}
static void effect_diag_frame(void) {
    if(!g_effect.enabled)return;
    g_effect.frame++;g_effect.frame_samples=0;effect_diag_poll();
}
static int effect_diag_begin(const c_draw *c) {
    const gstate *s=&c->s;
    if(!g_effect.enabled||!g_effect.frame||(g_effect.frame-1)%EFFECT_QUERY_INTERVAL||
       g_effect.frame_samples==EFFECT_QUERY_FRAME_MAX||g_effect.issued==EFFECT_QUERY_MAX||
       (s->mode[0]&MODE_CLEAR)||!s->depth_test||s->depth_mask)return -1;
    unsigned slot=g_effect.cursor,tries=0;
    while(tries<EFFECT_QUERY_SLOTS&&g_effect.slots[slot].pending){slot=(slot+1)%EFFECT_QUERY_SLOTS;tries++;}
    if(tries==EFFECT_QUERY_SLOTS){g_effect.ring_skips++;return -1;}
    effect_query *q=&g_effect.slots[slot];memset(q,0,sizeof *q);
    q->name=g_effect.names[slot];q->state=*s;q->frame=g_effect.frame;q->pending=1;
    q->fb=g_rfb[g_cur_fb_id].fbo;q->fb_addr=c->fb_addr;q->fb_stride=c->fb_stride;q->fb_fmt=c->fb_fmt;
    q->tex_addr=c->tex_addr;q->tex_addr_last=c->tex_addr_last;q->vertices=c->count;q->source_draws=c->source_draws;
    if(s->tex_kind==1&&s->tex_id>=0&&s->tex_id<g_nrtex) {
        const rtex *t=&g_rtex[s->tex_id];q->texture=t->name;q->texture_source=t->source_addr;q->texture_hash=t->source_hash;q->clut_key=t->clut_key;q->upload=t->upload;q->texture_w=t->w;q->texture_h=t->h;
    } else if(s->tex_kind>=2&&s->tex_id>=0&&s->tex_id<g_nrfb) {
        const rfb *f=&g_rfb[s->tex_id];q->texture=s->tex_kind==3?f->feedback_tex:s->tex_kind==4?f->clut_tex:f->tex;q->texture_w=f->w;q->texture_h=f->h;
    }
    g_effect.cursor=(slot+1)%EFFECT_QUERY_SLOTS;g_effect.pending++;g_effect.issued++;g_effect.frame_samples++;
    effectBeginQuery(GL_ANY_SAMPLES_PASSED,q->name);return (int)slot;
}
static void effect_diag_end(int slot) {
    if(slot<0)return;
    effectEndQuery(GL_ANY_SAMPLES_PASSED);
    if(g_effect.issued==EFFECT_QUERY_MAX&&!g_effect.limit_logged) {
        g_effect.limit_logged=1;fprintf(stderr,"effect-vis,v=1,event=limit,context=%u,frame=%llu,issued=%u,pending=%u\n",g_effect.context,(unsigned long long)g_effect.frame,g_effect.issued,g_effect.pending);
    }
}
static void effect_diag_stop(void) {
    if(!g_effect.enabled)return;
    effect_diag_poll();
    fprintf(stderr,"effect-vis,v=1,event=end,context=%u,frames=%llu,issued=%u,results=%u,pending=%u,ring_skips=%u\n",g_effect.context,(unsigned long long)g_effect.frame,g_effect.issued,g_effect.results,g_effect.pending,g_effect.ring_skips);
    effectDeleteQueries(EFFECT_QUERY_SLOTS,g_effect.names);memset(&g_effect,0,sizeof g_effect);
}

static GLuint compile(GLenum type, const char *body) {
    const char *head = g_host.gles ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                                   : "#version 330 core\n";
    const char *src[2] = { head, body };
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "render_gl: shader: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint program(const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "render_gl: link: %s\n", log);
        return 0;
    }
    return p;
}

static void vertex_layout(void) {
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(gvert), (const void *)offsetof(gvert, x));
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, 1, sizeof(gvert), (const void *)offsetof(gvert, rgba));
    glVertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(gvert), (const void *)offsetof(gvert, u));
    glVertexAttribPointer(3, 1, GL_FLOAT, 0, sizeof(gvert), (const void *)offsetof(gvert, fog));
    glVertexAttribPointer(4, 2, GL_FLOAT, 0, sizeof(gvert), (const void *)offsetof(gvert, depth));
    for (GLuint i = 0; i < 5; i++) glEnableVertexAttribArray(i);
}

static GLuint new_texture(int w, int h, const void *px, GLenum filter) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static int load_gl(void) {
    int missing = 0;
#define GL_LOAD(ret, name, args) \
    gl##name = (ret (GLCALL *) args)g_host.get_proc(g_host.user, "gl" #name); \
    if (!gl##name) { fprintf(stderr, "render_gl: no gl%s\n", #name); missing++; }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return missing ? -1 : 0;
}

/* The compressed formats this GL takes (PSP_GL_ASTC4x4 | PSP_GL_BC7), for
 * replacements a host transcodes (texture packs). -1 until the GL is up. */
static volatile int g_cformats = -1;
int psp_render_gl_compressed_formats(void) { return g_cformats; }

static void find_formats(void) {
    GLint n = 0;
    int f = 0;
    glGetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &n);
    for (GLint i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(0x1F03 /* GL_EXTENSIONS */, (GLuint)i);
        if (!e) continue;
        if (!strcmp(e, "GL_KHR_texture_compression_astc_ldr")) f |= PSP_GL_ASTC4x4;
        if (!strcmp(e, "GL_ARB_texture_compression_bptc") || !strcmp(e, "GL_EXT_texture_compression_bptc")) f |= PSP_GL_BC7;
    }
    g_cformats = f;
}

static GLuint new_compressed(int w, int h, GLenum fmt, GLsizei bytes, const void *data) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, bytes, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

int psp_render_gl_validate_scale(int scale,char *error,size_t capacity) {
    GLint tex=0,rb=0;glGetIntegerv(0x0D33,&tex);glGetIntegerv(0x84E8,&rb);
    if(scale<1 || scale>6 || tex/512<scale || rb/512<scale) {
        if(error && capacity)snprintf(error,capacity,"Resolution %dx exceeds GPU limits (texture=%d renderbuffer=%d)",scale,tex,rb);
        return -1;
    }
    g_max_texture=tex;g_max_renderbuffer=rb;return 0;
}
static int setup(void) {
    if (g_host.init(g_host.user) != 0) return -1;
    if (load_gl() != 0) return -1;
    char scale_error[160];if(psp_render_gl_validate_scale(g_scale,scale_error,sizeof scale_error)){gpu_fail(scale_error);return -1;}
    find_formats();
    g_prog = program(VS_DRAW, FS_DRAW);
    g_blit = program(VS_BLIT, FS_BLIT);
    g_resolve=program(VS_RESOLVE,FS_RESOLVE);
    if (!g_prog || !g_blit || !g_resolve) return -1;
    g_resolve_alpha=glGetUniformLocation(g_resolve,"u_alpha");
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "u_tex"), 0);
    g_u_mode = glGetUniformLocation(g_prog, "u_mode");
    g_u_env = glGetUniformLocation(g_prog, "u_env");
    g_u_fog = glGetUniformLocation(g_prog, "u_fog");
    g_u_source_factor=glGetUniformLocation(g_prog,"u_source_factor");
    g_u_depth_range=glGetUniformLocation(g_prog,"u_depth_range");
    glUseProgram(g_blit);
    glUniform1i(glGetUniformLocation(g_blit, "u_tex"), 0);
    g_b_mode = glGetUniformLocation(g_blit, "u_mode");

    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    vertex_layout();
    glGenVertexArrays(1, &g_bvao);
    glGenBuffers(1, &g_bvbo);
    glBindVertexArray(g_bvao);
    glBindBuffer(GL_ARRAY_BUFFER, g_bvbo);
    vertex_layout();

    glDisable(GL_CULL_FACE);
    if (!g_host.gles) glDisable(GL_DEPTH_CLAMP); /* per-draw PSP clamp uses raw fragment depth */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glActiveTexture(GL_TEXTURE0);
    return glGetError() == 0 ? 0 : -1;
}

/* The state of the last draw, so only what changed is set. */
static gstate g_applied;
static int    g_applied_valid;
static void resolve_stencil(int id);
static void snapshot_feedback(int id,int alpha,const gstate *s,const gvert *vertices,uint32_t count);
static void convert_framebuffer_clut(const gstate *s,const gvert *vertices,uint32_t count);
static void feedback_copy_bounds(const rfb *f,const gstate *s,const gvert *v,uint32_t count,int out[4]);
static int depth_bind(int id,uint32_t addr,int stride);

static void bind_texture(const gstate *s) {
    GLuint t = 0;
    rsampler *p = NULL;
    if (s->tex_kind == 1 && s->tex_id < g_nrtex) { t = g_rtex[s->tex_id].name; p = &g_rtex[s->tex_id].sampler; }
    if (s->tex_kind == 2 && s->tex_id < g_nrfb) { t = g_rfb[s->tex_id].tex; p = &g_rfb[s->tex_id].sampler; }
    if (s->tex_kind == 3 && s->tex_id < g_nrfb) { t = g_rfb[s->tex_id].feedback_tex; p = &g_rfb[s->tex_id].feedback_sampler; }
    if (s->tex_kind == 4 && s->tex_id < g_nrfb) { t = g_rfb[s->tex_id].clut_tex; p = &g_rfb[s->tex_id].clut_sampler; }
    glBindTexture(GL_TEXTURE_2D, t);
    if (!t) return;
    /* Sampler parameters belong to the texture object and survive binding a
     * different texture. Keep that state with the object, including FBOs and
     * feedback snapshots; deleting/replacing the object discards the cache. */
    if (!p->valid || p->wrap_s != s->wrap_s) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->wrap_s);
    if (!p->valid || p->wrap_t != s->wrap_t) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->wrap_t);
    if (!p->valid || p->min_f != s->min_f) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->min_f);
    if (!p->valid || p->mag_f != s->mag_f) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->mag_f);
    p->wrap_s=s->wrap_s;p->wrap_t=s->wrap_t;p->min_f=s->min_f;p->mag_f=s->mag_f;p->valid=1;
}

/* Private experiment: source alpha must influence an observable result. */
static int sampled_alpha_used(const gstate *s) {
    if(!(s->mode[0]&MODE_TEXALPHA))return 0;
    if(s->mode[1]==1 || (s->mode[0]&MODE_ATEST) || s->cmask[3])return 1;
    return s->blend && (s->bsrc==GL_SRC_ALPHA || s->bsrc==GL_ONE_MINUS_SRC_ALPHA ||
                        s->bdst==GL_SRC_ALPHA || s->bdst==GL_ONE_MINUS_SRC_ALPHA);
}

static void apply_vertices(const gstate *s,const gvert *vertices,uint32_t count);
/* The legacy scene/shadow caller has no playback vertices at this boundary.
 * Retain a full snapshot there rather than deriving bounds from stale poses. */
static void apply(const gstate *s){apply_vertices(s,NULL,0);}
static void apply_vertices(const gstate *s,const gvert *vertices,uint32_t count) {
    /* Retain the previous Z owner across color-only targets. */
    if(s->depth_test && depth_bind(g_cur_fb_id,s->zb_addr,s->zb_stride)) {
        glBindFramebuffer(GL_FRAMEBUFFER,g_rfb[g_cur_fb_id].fbo);
        glEnable(GL_SCISSOR_TEST);
    }
    if(s->needs_dest_alpha)resolve_stencil(g_cur_fb_id);
    if(s->tex_kind==2 && sampled_alpha_used(s))resolve_stencil(s->tex_id);
    if(s->tex_kind==3)snapshot_feedback(s->tex_id,sampled_alpha_used(s),s,vertices,count);
    if(s->tex_kind==4)convert_framebuffer_clut(s,vertices,count);
    const gstate *o = g_applied_valid ? &g_applied : NULL;
    if ((s->mode[0]&MODE_DEPTHRANGE) && (!o || !(o->mode[0]&MODE_DEPTHRANGE) || memcmp(o->depth_range,s->depth_range,sizeof s->depth_range)))
        glUniform2f(g_u_depth_range,s->depth_range[0],s->depth_range[1]);
    if (!o || o->tex_kind != s->tex_kind || o->tex_id != s->tex_id || o->wrap_s != s->wrap_s ||
        o->wrap_t != s->wrap_t || o->min_f != s->min_f || o->mag_f != s->mag_f)
        bind_texture(s);
    if (!o || memcmp(o->mode, s->mode, sizeof s->mode)) glUniform4i(g_u_mode, s->mode[0], s->mode[1], s->mode[2], s->mode[3]);
    if (!o || memcmp(o->env, s->env, sizeof s->env)) glUniform3f(g_u_env, s->env[0], s->env[1], s->env[2]);
    if (!o || memcmp(o->fogc, s->fogc, sizeof s->fogc)) glUniform3f(g_u_fog, s->fogc[0], s->fogc[1], s->fogc[2]);
    if(!o || memcmp(o->source_factor,s->source_factor,sizeof s->source_factor))
        glUniform3f(g_u_source_factor,s->source_factor[0],s->source_factor[1],s->source_factor[2]);
    if (!o || o->blend != s->blend) { if (s->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
    if (s->blend) {
        if (!o || o->bsrc != s->bsrc || o->bdst != s->bdst || !o->blend)
            glBlendFuncSeparate((GLenum)s->bsrc, (GLenum)s->bdst, GL_ONE, GL_ZERO);   /* alpha: the source's */
        if (!o || o->beq != s->beq || !o->blend) glBlendEquationSeparate((GLenum)s->beq, GL_FUNC_ADD);
        if (!o || memcmp(o->bcol, s->bcol, sizeof s->bcol) || !o->blend)
            glBlendColor(s->bcol[0], s->bcol[1], s->bcol[2], s->bcol[3]);
    }
    if (!o || o->depth_test != s->depth_test) { if (s->depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); }
    if (s->depth_test && (!o || o->depth_func != s->depth_func || !o->depth_test)) glDepthFunc((GLenum)s->depth_func);
    if (!o || o->depth_mask != s->depth_mask) glDepthMask((GLboolean)s->depth_mask);
    if(!o || o->stencil!=s->stencil){if(s->stencil)glEnable(GL_STENCIL_TEST);else glDisable(GL_STENCIL_TEST);}
    if(s->stencil) {
        if(!o || !o->stencil || o->stencil_func!=s->stencil_func || o->stencil_ref!=s->stencil_ref || o->stencil_mask!=s->stencil_mask)
            glStencilFunc((GLenum)s->stencil_func,s->stencil_ref,(GLuint)s->stencil_mask);
        if(!o || !o->stencil || o->stencil_write!=s->stencil_write)glStencilMask((GLuint)s->stencil_write);
        if(!o || !o->stencil || o->stencil_fail!=s->stencil_fail || o->stencil_zfail!=s->stencil_zfail || o->stencil_zpass!=s->stencil_zpass)
            glStencilOp((GLenum)s->stencil_fail,(GLenum)s->stencil_zfail,(GLenum)s->stencil_zpass);
    }
    if (!o || memcmp(o->cmask, s->cmask, sizeof s->cmask))
        glColorMask((GLboolean)s->cmask[0], (GLboolean)s->cmask[1], (GLboolean)s->cmask[2], (GLboolean)s->cmask[3]);
    if (!o || memcmp(o->sc, s->sc, sizeof s->sc))
        glScissor(s->sc[0] * g_scale, s->sc[1] * g_scale, s->sc[2] * g_scale, s->sc[3] * g_scale);
    g_applied = *s;
    g_applied_valid = 1;
    if(s->depth_test && s->depth_mask) {
        /* A depth-only clear can write this attachment even with no shared
         * Z allocation selected. Never reuse its previous resident tag. */
        g_rfb[g_cur_fb_id].depth_valid=0;
        if(g_depth_id>=0)g_depth_dirty=1;
    }
    /* This apply boundary also serves retained scene playback. Mark the
     * impending original draw after its source conversion: a self draw must
     * invalidate the just-converted snapshot for its next dependent draw. */
    g_rfb[g_cur_fb_id].color_version++;
}

static void fb_new(int id, int w, int h, int fmt) {
    g_rfb = (rfb *)grow(g_rfb, &g_nrfb, id, sizeof *g_rfb);
    rfb *f = &g_rfb[id];
    f->w = w * g_scale;
    f->h = h * g_scale;
    f->fmt=fmt;
    f->tex = new_texture(f->w, f->h, NULL, GL_LINEAR);
    glGenRenderbuffers(1, &f->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, f->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, f->w, f->h);
    GLint actual_w=0,actual_h=0;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER,0x8D42,&actual_w);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER,0x8D43,&actual_h);
    host_mutex_lock(&g_m);g_attachment_width=actual_w;g_attachment_height=actual_h;host_mutex_unlock(&g_m);
    glGenFramebuffers(1, &f->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, f->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || glGetError()!=0 || actual_w!=f->w || actual_h!=f->h) {
        char error[160];snprintf(error,sizeof error,"Cannot allocate %dx%d GPU target at %dx resolution",f->w,f->h,g_scale);
        gpu_fail(error);glDeleteFramebuffers(1,&f->fbo);glDeleteTextures(1,&f->tex);glDeleteRenderbuffers(1,&f->depth);
        memset(f,0,sizeof *f);g_cur_fb_h=0;return;
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glStencilMask(255);
    /* RGB565 texture fetches are opaque, including pixels never drawn. The
     * separate stencil attachment stays zero and is never written for 565. */
    glClearColor(0, 0, 0, fmt==0?1.0f:0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    g_applied_valid = 0;
    g_cur_fb_h = 0;
}

static void depth_blit(GLuint src,GLuint dst,int w,int h) {
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,src);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,dst);
    glBlitFramebuffer(0,0,w,h,0,0,w,h,GL_DEPTH_BUFFER_BIT,GL_NEAREST);
    g_applied_valid=0;
}
static void depth_commit(void) {
    if(g_depth_dirty && g_depth_fb>=0 && g_depth_id>=0) {
        rfb *f=&g_rfb[g_depth_fb];rzbuf *z=&g_rzb[g_depth_id];
        int w=f->w<z->w?f->w:z->w,h=f->h<z->h?f->h:z->h;
        depth_blit(f->fbo,z->fbo,w,h);
        /* Partial-target commits conservatively invalidate all other color
         * attachments. This one already contains the newly committed data. */
        z->version++;
        f->depth_id=g_depth_id;f->depth_version=z->version;f->depth_valid=1;
    }
    g_depth_dirty=0;
}
static int depth_find(uint32_t addr,int stride) {
    for(int i=0;i<g_nrzb;i++)if(g_rzb[i].addr==addr && g_rzb[i].stride==stride)return i;
    if(stride<=0 || stride>g_max_renderbuffer/g_scale){gpu_fail("Depth allocation exceeds GPU dimensions");return -1;}
    int id=g_nrzb++;
    g_rzb=(rzbuf *)grow(g_rzb,&g_rzb_capacity,id,sizeof *g_rzb);rzbuf *z=&g_rzb[id];
    z->addr=addr;z->stride=stride;z->w=stride*g_scale;z->h=512*g_scale;
    glGenRenderbuffers(1,&z->depth);glBindRenderbuffer(GL_RENDERBUFFER,z->depth);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,z->w,z->h);
    glGenFramebuffers(1,&z->fbo);glBindFramebuffer(GL_FRAMEBUFFER,z->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,z->depth);
    const GLenum none=GL_ZERO;glDrawBuffers(1,&none);glReadBuffer(GL_ZERO);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE || glGetError()!=0) {
        gpu_fail("Cannot allocate shared GPU depth target");return -1;
    }
    glDisable(GL_SCISSOR_TEST);glDepthMask(1);glClear(GL_DEPTH_BUFFER_BIT);g_applied_valid=0;
    return id;
}
static int depth_bind(int id,uint32_t addr,int stride) {
    if(g_depth_fb==id && ((stride<=0 && g_depth_id<0) ||
       (g_depth_id>=0 && g_rzb[g_depth_id].addr==addr && g_rzb[g_depth_id].stride==stride)))return 0;
    depth_commit();g_depth_fb=id;g_depth_id=-1;
    if(stride<=0)return 1;
    g_depth_id=depth_find(addr,stride);if(g_depth_id<0)return 1;
    rfb *f=&g_rfb[id];rzbuf *z=&g_rzb[g_depth_id];
    int w=f->w<z->w?f->w:z->w,h=f->h<z->h?f->h:z->h;
    if(!f->depth_valid || f->depth_id!=g_depth_id || f->depth_version!=z->version) {
        depth_blit(z->fbo,f->fbo,w,h);
        f->depth_id=g_depth_id;f->depth_version=z->version;f->depth_valid=1;
    }
    return 1;
}
static void fb_free(int id) {
    if (id >= g_nrfb || !g_rfb[id].fbo) return;
    if(g_depth_fb==id){depth_commit();g_depth_fb=g_depth_id=-1;}
    rfb *f = &g_rfb[id];
    glDeleteFramebuffers(1, &f->fbo);
    glDeleteTextures(1, &f->tex);
    glDeleteRenderbuffers(1, &f->depth);
    if(f->feedback_fbo)glDeleteFramebuffers(1,&f->feedback_fbo);
    if(f->feedback_tex)glDeleteTextures(1,&f->feedback_tex);
    if(f->clut_fbo)glDeleteFramebuffers(1,&f->clut_fbo);
    if(f->clut_tex)glDeleteTextures(1,&f->clut_tex);
    memset(f, 0, sizeof *f);
}

static void fb_bind(int id) {
    if (id >= g_nrfb || !g_rfb[id].fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, g_rfb[id].fbo);
    glViewport(0, 0, g_rfb[id].w, g_rfb[id].h);
    glEnable(GL_SCISSOR_TEST);
    g_cur_fb_h = g_rfb[id].h;
    g_cur_fb_id=id;
}

/* Stencil is authoritative. Materialize its eight bits into the color
 * attachment's alpha only when sampling/blending/capturing needs it. This
 * uses GPU stencil tests and alpha-only additive passes, with no readback. */
static void resolve_stencil(int id) {
    if(id<0 || id>=g_nrfb || !g_rfb[id].fbo || !g_rfb[id].stencil_dirty)return;
    rfb *f=&g_rfb[id];int previous=g_cur_fb_id;
    glBindFramebuffer(GL_FRAMEBUFFER,f->fbo);glViewport(0,0,f->w,f->h);
    glDisable(GL_SCISSOR_TEST);glDisable(GL_DEPTH_TEST);glEnable(GL_STENCIL_TEST);
    glStencilMask(0);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);glColorMask(0,0,0,1);
    glDisable(GL_BLEND);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);glBlendFuncSeparate(GL_ZERO,GL_ONE,GL_ONE,GL_ONE);glBlendEquationSeparate(GL_FUNC_ADD,GL_FUNC_ADD);
    glUseProgram(g_resolve);glBindVertexArray(g_vao);
    for(unsigned bit=1;bit<256;bit<<=1){glStencilFunc(GL_NOTEQUAL,0,bit);glUniform1f(g_resolve_alpha,bit/255.0f);glDrawArrays(GL_TRIANGLES,0,3);}
    f->stencil_dirty=0;
    if(previous>=0 && previous<g_nrfb && g_rfb[previous].fbo){rfb *old=&g_rfb[previous];glBindFramebuffer(GL_FRAMEBUFFER,old->fbo);glViewport(0,0,old->w,old->h);glEnable(GL_SCISSOR_TEST);}
    else glBindFramebuffer(GL_FRAMEBUFFER,0);
    glUseProgram(g_prog);glBindVertexArray(g_vao);glBindBuffer(GL_ARRAY_BUFFER,g_vbo);g_applied_valid=0;
}

static void convert_framebuffer_clut(const gstate *s,const gvert *vertices,uint32_t count) {
    if(s->tex_id<0 || s->tex_id>=g_nrfb || s->clut[0]<=0 || s->clut[0]>=g_nrtex)return;
    rfb *f=&g_rfb[s->tex_id];
    int region[4];feedback_copy_bounds(f,s,vertices,count,region);
    if(f->clut_tex && f->clut_version==f->color_version && !memcmp(f->clut_params,s->clut,sizeof s->clut) &&
       region[0]>=f->clut_region[0] && region[1]>=f->clut_region[1] &&
       region[2]<=f->clut_region[2] && region[3]<=f->clut_region[3])return;
    /* Stencil is the authoritative framebuffer alpha even when the texture
     * function ignores sampled alpha: those bits can select a palette RGB. */
    if(((uint32_t)s->clut[2]<<(unsigned)s->clut[1])&0xff000000u)resolve_stencil(s->tex_id);
    if(!g_depal){
        g_depal=program(VS_RESOLVE,FS_DEPAL);if(!g_depal){gpu_fail("Cannot compile framebuffer CLUT conversion");return;}
        glUseProgram(g_depal);glUniform1i(glGetUniformLocation(g_depal,"u_tex"),0);
        glUniform1i(glGetUniformLocation(g_depal,"u_palette"),1);g_depal_mode=glGetUniformLocation(g_depal,"u_clut");
    }
    if(!f->clut_tex){
        f->clut_tex=new_texture(f->w,f->h,NULL,GL_NEAREST);glGenFramebuffers(1,&f->clut_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER,f->clut_fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,f->clut_tex,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){gpu_fail("Cannot allocate framebuffer CLUT conversion");return;}
    }
    int previous=g_cur_fb_id;
    glBindFramebuffer(GL_FRAMEBUFFER,f->clut_fbo);glViewport(0,0,f->w,f->h);
    /* Event filters alternate palettes while copying narrow strips back to
     * the source. Convert only the sampled texels, retaining the full image's
     * dimensions and filtering coordinates. Uncertain inputs use full bounds. */
    glEnable(GL_SCISSOR_TEST);glScissor(region[0],region[1],region[2]-region[0],region[3]-region[1]);
    glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_BLEND);glColorMask(1,1,1,1);
    glUseProgram(g_depal);glUniform4i(g_depal_mode,s->clut[1],s->clut[2],s->clut[3],0);
    glActiveTexture(GL_TEXTURE0+1);glBindTexture(GL_TEXTURE_2D,g_rtex[s->clut[0]].name);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,f->tex);glBindVertexArray(g_vao);glDrawArrays(GL_TRIANGLES,0,3);
    f->clut_version=f->color_version;memcpy(f->clut_params,s->clut,sizeof s->clut);
    memcpy(f->clut_region,region,sizeof region);
    if(previous>=0 && previous<g_nrfb){rfb *target=&g_rfb[previous];glBindFramebuffer(GL_FRAMEBUFFER,target->fbo);glViewport(0,0,target->w,target->h);glEnable(GL_SCISSOR_TEST);}
    glUseProgram(g_prog);glBindVertexArray(g_vao);glBindBuffer(GL_ARRAY_BUFFER,g_vbo);g_applied_valid=0;
}

/* A feedback texture retains the full framebuffer dimensions/UV mapping.
 * Only the texels potentially read by this actual playback batch are copied.
 * Affine clip carriers have positive barycentric weights after clipping, so
 * their UVs stay within the vertex interval. Perspective or uncertain inputs
 * take the existing full copy. The two-physical-texel halo covers both taps
 * of linear filtering and conservative interpolation/rounding at the edge. */
static void feedback_copy_bounds(const rfb *f,const gstate *s,const gvert *v,
                                 uint32_t count,int out[4]) {
    out[0]=out[1]=0;out[2]=f->w;out[3]=f->h;
    if(!v || !count || s->wrap_s!=GL_CLAMP_TO_EDGE || s->wrap_t!=GL_CLAMP_TO_EDGE ||
       (s->min_f!=GL_NEAREST && s->min_f!=GL_LINEAR) ||
       (s->mag_f!=GL_NEAREST && s->mag_f!=GL_LINEAR) ||
       f->w<=0 || f->h<=0 || f->w>4096 || f->h>4096)return;
    float u0=v[0].u,u1=u0,t0=v[0].v,t1=t0;
    for(uint32_t i=0;i<count;i++) {
        /* Limit the optimized path to through/affine playback. In particular
         * do not infer UV bounds from original vertices after pose changes. */
        if(v[i].w!=1.0f || !isfinite(v[i].x) || !isfinite(v[i].y) ||
           !isfinite(v[i].z) || !isfinite(v[i].u) || !isfinite(v[i].v) ||
           v[i].u< -1.0f || v[i].u>2.0f || v[i].v< -1.0f || v[i].v>2.0f)return;
        if(v[i].u<u0)u0=v[i].u;if(v[i].u>u1)u1=v[i].u;
        if(v[i].v<t0)t0=v[i].v;if(v[i].v>t1)t1=v[i].v;
    }
    /* Coordinates already include framebuffer-address offsets and divide by
     * the logical framebuffer dimensions. Multiply by the physical target
     * dimensions here, so scale is preserved exactly once. */
    double lo_u=fmax(0.0,fmin(1.0,(double)u0)),hi_u=fmax(0.0,fmin(1.0,(double)u1));
    double lo_v=fmax(0.0,fmin(1.0,(double)t0)),hi_v=fmax(0.0,fmin(1.0,(double)t1));
    int x0=(int)floor(lo_u*f->w)-2,x1=(int)ceil(hi_u*f->w)+2;
    int y0=(int)floor(lo_v*f->h)-2,y1=(int)ceil(hi_v*f->h)+2;
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>f->w)x1=f->w;if(y1>f->h)y1=f->h;
    if(x1<=x0 || y1<=y0)return;
    out[0]=x0;out[1]=y0;out[2]=x1;out[3]=y1;
}

static void snapshot_feedback(int id,int alpha,const gstate *s,const gvert *vertices,uint32_t count) {
    if(id<0 || id>=g_nrfb || !g_rfb[id].fbo)return;
    if(alpha)resolve_stencil(id);rfb *f=&g_rfb[id];
    if(!f->feedback_fbo) {
        f->feedback_tex=new_texture(f->w,f->h,NULL,GL_NEAREST);
        glGenFramebuffers(1,&f->feedback_fbo);glBindFramebuffer(GL_FRAMEBUFFER,f->feedback_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,f->feedback_tex,0);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,f->fbo);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,f->feedback_fbo);
    int region[4];feedback_copy_bounds(f,s,vertices,count,region);
    glBlitFramebuffer(region[0],region[1],region[2],region[3],region[0],region[1],region[2],region[3],GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,f->fbo);glEnable(GL_SCISSOR_TEST);g_applied_valid=0;
}

/* A textured rectangle in window pixels (y down), for presenting. */
static void blit_quad(float x0, float y0, float x1, float y1, int ww, int wh,
                      float u0, float v0, float u1, float v1) {
    gvert q[4];
    memset(q, 0, sizeof q);
    const float X0 = x0 / (float)ww * 2 - 1, X1 = x1 / (float)ww * 2 - 1;
    const float Y0 = 1 - y0 / (float)wh * 2, Y1 = 1 - y1 / (float)wh * 2;
    q[0].x = X0; q[0].y = Y0; q[0].u = u0; q[0].v = v0;
    q[1].x = X1; q[1].y = Y0; q[1].u = u1; q[1].v = v0;
    q[2].x = X0; q[2].y = Y1; q[2].u = u0; q[2].v = v1;
    q[3].x = X1; q[3].y = Y1; q[3].u = u1; q[3].v = v1;
    for (int i = 0; i < 4; i++) q[i].w = 1;
    glBindVertexArray(g_bvao);
    glBindBuffer(GL_ARRAY_BUFFER, g_bvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Keep a 480x272 copy of what was shown, for psp_display_capture(). */
static void capture_current(void) {
    host_mutex_lock(&g_m);
    uint64_t request=g_capture_requested;
    host_mutex_unlock(&g_m);
    static uint32_t buf[SW * SH];
    int have=g_last_present_valid;
    if(g_last_present_fb<0) {
        memcpy(buf,g_last_ram,sizeof buf);
    } else {
        rfb *src=&g_rfb[g_last_present_fb];
        resolve_stencil(g_last_present_fb);
        if (!g_cap_fbo) {
            g_cap_tex = new_texture(SW, SH, NULL, GL_NEAREST);
            glGenFramebuffers(1, &g_cap_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, g_cap_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_cap_tex, 0);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_cap_fbo);
        glBlitFramebuffer(0,0,SW*g_scale,SH*g_scale,0,0,SW,SH,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_cap_fbo);
        glReadPixels(0, 0, SW, SH, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        g_applied_valid=0;
    }
    host_mutex_lock(&g_m);
    if(have){memcpy(g_capture,buf,sizeof buf);g_readbacks++;}
    g_have_capture=have;g_capture_done=request;host_cond_broadcast(&g_cv);
    host_mutex_unlock(&g_m);
}

static atomic_int g_stretch;      /* any-thread psp_render_gl_set_stretch */
static atomic_int g_native_fps; /* worker writes; any-thread interpolation setter reads */
static int g_native_scene;
static uint64_t g_native_scene_deadline,g_native_swap_lead;
static int scene_wait(uint64_t deadline);
static int g_scene_mode; /* original30Hz scenes, optional visual60Hz midpoint */
static psp_frame_pacer g_native_pacer; /* render-thread owned */
static host_pacing g_native_wait;
static int g_native_wait_ready;

static void present(const c_present *c) {
    if(g_native_scene){
        uint64_t lead=g_host.swap_interval?g_native_swap_lead:0;
        if(!scene_wait(g_native_scene_deadline>lead?g_native_scene_deadline-lead:0)){free(c->pixels);return;}
    }
    if(g_native_fps&&!g_scene_mode&&!g_native_scene&&!psp_frame_pacer_present(&g_native_pacer,host_pacing_now())){
        free(c->pixels);return; /* Omit only the window swap, never GE work. */
    }
    int ww = 0, wh = 0;
    g_host.size(g_host.user, &ww, &wh);
    if (ww <= 0 || wh <= 0) { free(c->pixels); return; }

    GLuint src_tex = 0, src_fbo = 0;
    float u1 = 1, v1 = 1;
    if (c->fb >= 0 && c->fb < g_nrfb && g_rfb[c->fb].fbo) {
        const rfb *f = &g_rfb[c->fb];
        src_tex = f->tex;
        src_fbo = f->fbo;
        u1 = (float)(SW * g_scale) / (float)f->w;
        v1 = (float)(SH * g_scale) / (float)f->h;
    } else if (c->pixels) {
        if (!g_show_tex) g_show_tex = new_texture(SW, SH, NULL, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, g_show_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SW, SH, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
        src_tex = g_show_tex;
    }
    g_last_present_valid=!!src_tex;g_last_present_fb=src_fbo?c->fb:-1;
    if(c->pixels)memcpy(g_last_ram,c->pixels,sizeof g_last_ram);
    free(c->pixels);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_blit);
    if (src_tex) {
        /* Letterboxed at the PSP's aspect, or stretched over the window. */
        float w = (float)ww, h = w * SH / SW;
        if (g_stretch) h = (float)wh;
        else if (h > (float)wh) { h = (float)wh; w = h * SW / SH; }
        const float x0 = ((float)ww - w) / 2, y0 = ((float)wh - h) / 2;
        glBindTexture(GL_TEXTURE_2D, src_tex);
        glUniform4i(g_b_mode, 0, 0, 0, 0);
        blit_quad(x0, y0, x0 + w, y0 + h, ww, wh, 0, 0, u1, v1);
    }

    host_mutex_lock(&g_m);
    if (g_ovl_dirty) {
        if (g_ovl) {
            if (!g_ovl_tex) g_ovl_tex = new_texture(1, 1, NULL, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, g_ovl_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_ovl_w, g_ovl_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_ovl);
            /* Pixel for pixel stays sharp; scaled up, filtered. */
            const GLint f = g_ovl_dw == g_ovl_w && g_ovl_dh == g_ovl_h ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        }
        g_ovl_dirty = 0;
    }
    const int ow = g_ovl ? g_ovl_dw : 0, oh = g_ovl ? g_ovl_dh : 0;
    host_mutex_unlock(&g_m);
    if (ow && g_ovl_tex) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBindTexture(GL_TEXTURE_2D, g_ovl_tex);
        glUniform4i(g_b_mode, 1, 0, 0, 0);
        blit_quad(0, 0, (float)ow, (float)oh, ww, wh, 0, 0, 1, 1);
        glDisable(GL_BLEND);
    }

    uint64_t swap_start=g_native_scene?host_pacing_now():0;
    g_host.swap(g_host.user);
    if(g_native_scene&&g_host.swap_interval){uint64_t elapsed=host_pacing_now()-swap_start;g_native_swap_lead=(g_native_swap_lead*7+(elapsed>16667?16667:elapsed))/8;}
    host_mutex_lock(&g_m);g_presented++;host_mutex_unlock(&g_m);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    g_applied_valid = 0;
}

/* What play() runs: an ordinary frame runs everything; one shown with an
 * in-between frame first makes its textures and framebuffers, then draws
 * twice (the in-between vertices, then its own), then frees. */
enum { PLAY_MAKE = 1, PLAY_DRAW = 2, PLAY_FREE = 4, PLAY_PRESENT = 8, PLAY_ALL = 15 };

static void upload_vertices(const gbuf *vertices) {
    /* Every recorded primitive is a triangle list. Rebase after all pose
     * writers so constant depth stays exact even at unequal clip W. A ratio
     * of independently rounded depth*W and W otherwise floors integer ties
     * one unit low at scattered pixels. This needs no depth epsilon. */
    gvert *v=(gvert *)vertices->p;
    const size_t count=vertices->n/sizeof *v;
    for(size_t i=0;i+2<count;i+=3)
        v[i].depth_base=v[i+1].depth_base=v[i+2].depth_base=v[i].depth;
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)vertices->n,vertices->p,GL_STREAM_DRAW);
}

static void play(gframe *f, int what, const gbuf *vtx) {
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    if ((what & PLAY_DRAW) && vtx->n) upload_vertices(vtx);
    g_applied_valid = 0;

    for (size_t at = 0; at < f->cmd.n; ) {
        chead *h = (chead *)(f->cmd.p + at);
        at += h->size;
        const int kind = h->op == C_FB_NEW || h->op == C_TEX_NEW ? PLAY_MAKE
                       : h->op == C_FB_FREE || h->op == C_TEX_FREE ? PLAY_FREE
                       : h->op == C_PRESENT ? PLAY_PRESENT : PLAY_DRAW;
        if (!(what & kind)) continue;
        switch (h->op) {
        case C_FB_NEW: {
            const c_fb_new *c = (const c_fb_new *)h;
            fb_new(c->id, c->w, c->hgt, c->fmt);
            break;
        }
        case C_FB_FREE: fb_free(((const c_id *)h)->id); break;
        case C_FB_BIND: fb_bind(((const c_id *)h)->id); break;
        case C_TEX_NEW: {
            c_tex_new *c = (c_tex_new *)h;
            g_rtex = (rtex *)grow(g_rtex, &g_nrtex, c->id, sizeof *g_rtex);
            rtex *t=&g_rtex[c->id];
            if(g_effect.enabled){t->source_addr=c->source_addr;t->source_hash=c->source_hash;t->clut_key=c->clut_key;t->upload++;t->w=c->w;t->h=c->hgt;}
            if (c->gl_fmt) {                 /* a replacement, compressed (ASTC, BC7): a texture of its own */
                if (t->name) glDeleteTextures(1, &t->name);
                t->name = new_compressed(c->w, c->hgt, (GLenum)c->gl_fmt, (GLsizei)c->bytes, c->pixels);
                t->sampler.valid=0;
            } else if (!t->name) { t->name = new_texture(c->w, c->hgt, c->pixels, GL_NEAREST);t->sampler.valid=0; }
            else {
                glBindTexture(GL_TEXTURE_2D, t->name);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c->w, c->hgt, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
            }
            free(c->pixels);
            c->pixels = NULL;                /* made once, even when the frame is played twice */
            g_applied_valid = 0;             /* the texture binding moved */
            break;
        }
        case C_TEX_FREE: {
            const int id = ((const c_id *)h)->id;
            if (id < g_nrtex && g_rtex[id].name) { glDeleteTextures(1, &g_rtex[id].name);memset(&g_rtex[id],0,sizeof g_rtex[id]); }
            g_applied_valid = 0;
            break;
        }
        case C_DRAW: {
            const c_draw *c = (const c_draw *)h;
            if (!g_cur_fb_h) break;
            const size_t available=vtx->n/sizeof(gvert);
            const gvert *played=c->first<=available && c->count<=available-c->first?(const gvert *)vtx->p+c->first:NULL;
            apply_vertices(&c->s,played,c->count);
            int query=-1;
            if(g_effect.enabled)query=effect_diag_begin(c);
            glDrawArrays(GL_TRIANGLES, (GLint)c->first, (GLsizei)c->count);
            if(query>=0)effect_diag_end(query);
            if(c->s.stencil && c->s.stencil_write && (c->s.stencil_fail!=GL_KEEP || c->s.stencil_zfail!=GL_KEEP || c->s.stencil_zpass!=GL_KEEP))g_rfb[g_cur_fb_id].stencil_dirty=1;
            break;
        }
        case C_PRESENT:
            present((const c_present *)h);
            g_cur_fb_h = 0;
            g_cur_fb_id=-1;
            break;
        }
    }
}

/* ---- in-between frames (psp_render_gl_set_interpolation) ---------------------
 *
 * The GE side hands over a frame and records the next meanwhile; the render
 * thread copies the frame (so the GE side need not wait for both showings)
 * and, when the frame before it was a drawn one too, shows first a frame made
 * from the two: each of this frame's draws that the previous frame also had
 * (same framebuffer, same state, as many vertices, found in the same order)
 * gets every vertex halfway between its two positions. Draws with no
 * counterpart, and draws whose vertices jump far (a particle slot reused),
 * keep this frame's vertices. Half a frame later this frame itself is shown.
 * When too little matches (a cut, a new scene), when a frame came late
 * (loading), or when the frame is shown from memory (a movie), it is shown
 * alone, at once. */

static atomic_int g_interp;
int psp_render_gl_set_frame_rate(int fps){
    if(g_started||(fps!=30&&fps!=60))return -1;
    g_native_fps=fps;g_scene_mode=g_native_scene=0;g_interp=0;return 0;
}
int psp_render_gl_set_scene_rate(int fps){if(psp_render_gl_set_frame_rate(fps))return -1;g_scene_mode=1;return 0;}
int psp_render_gl_set_native_scene_rate(int fps){if(psp_render_gl_set_frame_rate(fps))return -1;g_native_scene=1;return 0;}
void psp_render_gl_set_interpolation(int on) { if(g_native_fps&&on)return;g_interp = on ? 1 : 0; }
void psp_render_gl_set_stretch(int on) { g_stretch = on ? 1 : 0; }
int  psp_render_gl_interpolation(void) { return g_interp; }

typedef struct { int32_t fb; gstate s; uint32_t first, count; } idraw;

static gframe   g_cur;                       /* the render thread's copy of the frame */
static gbuf     g_ivtx;                      /* its in-between vertices */
static gbuf     g_prev_vtx;                  /* the previous frame's vertices */
static idraw   *g_prev;                      /* and its draws */
static size_t   g_nprev, g_capprev;
static int      g_have_prev;
static uint64_t g_last_arrival, g_interval_us = 33367;
static uint64_t g_smooth_frames, g_alone_frames, g_moved, g_kept;
static uint64_t g_why_noprev, g_why_nopresent, g_why_memory, g_why_late, g_why_few;   /* frames shown alone */
static host_mutex g_sleep_m = HOST_MUTEX_INIT;
static host_cond  g_sleep_cv = HOST_COND_INIT;

static void gbuf_copy(gbuf *to, const gbuf *from) {
    to->n = 0;
    if (from->n) memcpy(gbuf_add(to, from->n), from->p, from->n);
}

static const c_present *find_present(const gframe *f) {
    const c_present *p = NULL;
    for (size_t at = 0; at < f->cmd.n; ) {
        const chead *h = (const chead *)(f->cmd.p + at);
        at += h->size;
        if (h->op == C_PRESENT) p = (const c_present *)h;
    }
    return p;
}

/* This frame's draws, with the framebuffer each went to, for the next frame. */
static void remember(const gframe *f) {
    gbuf_copy(&g_prev_vtx, &f->vtx);
    g_nprev = 0;
    int32_t fb = -1;
    for (size_t at = 0; at < f->cmd.n; ) {
        const chead *h = (const chead *)(f->cmd.p + at);
        at += h->size;
        if (h->op == C_FB_BIND) fb = ((const c_id *)h)->id;
        if (h->op != C_DRAW) continue;
        if (g_nprev == g_capprev) {
            g_capprev = g_capprev ? g_capprev * 2 : 1024;
            g_prev = (idraw *)realloc(g_prev, g_capprev * sizeof *g_prev);
            if (!g_prev) { fputs("render_gl: out of memory\n", stderr); abort(); }
        }
        const c_draw *d = (const c_draw *)h;
        g_prev[g_nprev].fb = fb;
        g_prev[g_nprev].s = d->s;
        g_prev[g_nprev].first = d->first;
        g_prev[g_nprev].count = d->count;
        g_nprev++;
    }
}

static float ndc(float c, float w) { return w != 0.0f ? c / w : c; }

/* The in-between vertices of `f` into g_ivtx; the share of vertices that moved
 * from a counterpart (0..1). */
static float make_between(const gframe *f) {
    gbuf_copy(&g_ivtx, &f->vtx);
    gvert *out = (gvert *)g_ivtx.p;
    const gvert *old = (const gvert *)g_prev_vtx.p;
    size_t cursor = 0, total = 0, matched = 0;
    int32_t fb = -1;
    for (size_t at = 0; at < f->cmd.n; ) {
        const chead *h = (const chead *)(f->cmd.p + at);
        at += h->size;
        if (h->op == C_FB_BIND) fb = ((const c_id *)h)->id;
        if (h->op != C_DRAW) continue;
        const c_draw *d = (const c_draw *)h;
        total += d->count;
        /* Its counterpart: the next draw of the previous frame like it, not far on. */
        const idraw *e = NULL;
        for (size_t j = cursor, k = 0; j < g_nprev && k < 64; j++, k++)
            if (g_prev[j].fb == fb && g_prev[j].count == d->count && !memcmp(&g_prev[j].s, &d->s, sizeof d->s)) {
                e = &g_prev[j];
                cursor = j + 1;
                break;
            }
        if (!e) continue;
        const gvert *a = old + e->first;
        gvert *b = out + d->first;
        /* A vertex that jumps more than an eighth of the screen is another
         * thing in the same slot: the whole draw keeps this frame's. */
        int jump = 0;
        for (uint32_t i = 0; i < d->count && !jump; i++) {
            if ((a[i].w > 0) != (b[i].w > 0)) jump = 1;
            else if (fabsf(ndc(a[i].x, a[i].w) - ndc(b[i].x, b[i].w)) > 0.25f ||
                     fabsf(ndc(a[i].y, a[i].w) - ndc(b[i].y, b[i].w)) > 0.25f) jump = 1;
        }
        if (jump) continue;
        for (uint32_t i = 0; i < d->count; i++) {
            if(a[i].w+b[i].w>0)b[i].depth=(a[i].depth*a[i].w+b[i].depth*b[i].w)/(a[i].w+b[i].w);
            b[i].x = (a[i].x + b[i].x) * 0.5f;
            b[i].y = (a[i].y + b[i].y) * 0.5f;
            b[i].z = (a[i].z + b[i].z) * 0.5f;
            b[i].w = (a[i].w + b[i].w) * 0.5f;
            b[i].fog = (a[i].fog + b[i].fog) * 0.5f;
            /* Scrolling textures move; a texture that flips frames does not blend. */
            if (fabsf(a[i].u - b[i].u) < 0.1f && fabsf(a[i].v - b[i].v) < 0.1f) {
                b[i].u = (a[i].u + b[i].u) * 0.5f;
                b[i].v = (a[i].v + b[i].v) * 0.5f;
            }
        }
        matched += d->count;
    }
    g_moved += matched;
    g_kept += total - matched;
    return total ? (float)matched / (float)total : 0.0f;
}

static void sleep_until(uint64_t deadline) {
    host_mutex_lock(&g_sleep_m);
    while (host_deadline_us() < deadline && !host_cond_until(&g_sleep_cv, &g_sleep_m, deadline)) { }
    host_mutex_unlock(&g_sleep_m);
}

static void play_smooth(gframe *f) {
    const uint64_t now = host_deadline_us();
    const uint64_t since = g_last_arrival ? now - g_last_arrival : 0;
    g_last_arrival = now;
    if (since >= 20000 && since <= 50000) g_interval_us = (g_interval_us * 7 + since) / 8;

    const c_present *p = find_present(f);
    int between = 0;
    if (!p) g_why_nopresent++;
    else if (p->fb < 0) g_why_memory++;
    else if (!g_have_prev) g_why_noprev++;
    else if (since > 60000) g_why_late++;
    else if (make_between(f) < 0.5f) g_why_few++;
    else between = 1;
    if (between) {
        play(f, PLAY_MAKE, &f->vtx);
        play(f, PLAY_DRAW | PLAY_PRESENT, &g_ivtx);             /* the in-between frame, now */
        sleep_until(now + g_interval_us / 2);
        play(f, PLAY_DRAW | PLAY_FREE | PLAY_PRESENT, &f->vtx);   /* and this frame, half a frame on */
        g_smooth_frames++;
    } else {
        play(f, PLAY_ALL, &f->vtx);
        g_alone_frames++;
    }
    g_have_prev = p && p->fb >= 0;
    if (g_have_prev) remember(f);
    if (((g_smooth_frames + g_alone_frames) % 600) == 0)
        fprintf(stderr, "render_gl: in-between frames for %llu of the last 600 frames, %.0f%% of vertices moved "
                "(alone: %llu no present, %llu from memory, %llu after one, %llu late, %llu too different)\n",
                (unsigned long long)g_smooth_frames, g_moved + g_kept ? 100.0 * (double)g_moved / (double)(g_moved + g_kept) : 0.0,
                (unsigned long long)g_why_nopresent, (unsigned long long)g_why_memory, (unsigned long long)g_why_noprev,
                (unsigned long long)g_why_late, (unsigned long long)g_why_few),
        g_smooth_frames = g_alone_frames = g_moved = g_kept = 0,
        g_why_nopresent = g_why_memory = g_why_noprev = g_why_late = g_why_few = 0;
}

#include "render_smooth.h"

/* Allocate replacements without touching the live image or its lazy caches.
 * The old objects remain usable until every allocation and copy has succeeded.
 * ES3.0.6 section4.3.3 permits scaled single-sample depth/stencil blits with
 * matching formats and NEAREST. All our attachments are DEPTH24_STENCIL8.
 * A driver rejecting that operation rejects the entire transaction. */
static int reconfigure_attachment(GLuint *fbo,GLuint *depth,GLuint *texture,int w,int h) {
    if(texture)*texture=new_texture(w,h,NULL,GL_LINEAR);
    glGenRenderbuffers(1,depth);glBindRenderbuffer(GL_RENDERBUFFER,*depth);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,w,h);
    GLint actual_w=0,actual_h=0;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER,0x8D42,&actual_w);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER,0x8D43,&actual_h);
    glGenFramebuffers(1,fbo);glBindFramebuffer(GL_FRAMEBUFFER,*fbo);
    if(texture)glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,*texture,0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,*depth);
    if(!texture){const GLenum none=GL_ZERO;glDrawBuffers(1,&none);glReadBuffer(GL_ZERO);}
    int complete=glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    GLenum error=glGetError();
    return complete && !error && actual_w==w && actual_h==h ? 0 : -1;
}
static int reconfigure_copy(GLuint src,GLuint dst,int sw,int sh,int dw,int dh,GLbitfield mask) {
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,src);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,dst);
    glBlitFramebuffer(0,0,sw,sh,0,0,dw,dh,mask,GL_NEAREST);
    return glGetError()==0?0:-1;
}
static int reconfigure_size(int old,int scale,int maximum) {
    return old>0 && old%g_scale==0 && old/g_scale<=maximum/scale;
}
static int reconfigure_targets(int scale) {
    rfb *frames=NULL;rzbuf *depths=NULL;int result=-1;
    GLint tex=0,rb=0;glGetIntegerv(0x0D33,&tex);glGetIntegerv(0x84E8,&rb);
    if(glGetError()!=0 || tex/512<scale || rb/512<scale)return -1;
    for(int i=0;i<g_nrfb;i++)if(g_rfb[i].fbo &&
       (!reconfigure_size(g_rfb[i].w,scale,tex<rb?tex:rb)||!reconfigure_size(g_rfb[i].h,scale,tex<rb?tex:rb)))return -1;
    for(int i=0;i<g_nrzb;i++)if(!reconfigure_size(g_rzb[i].w,scale,rb)||!reconfigure_size(g_rzb[i].h,scale,rb))return -1;
    if(scale==g_scale)return 0;
    if(g_nrfb && !(frames=(rfb *)calloc((size_t)g_nrfb,sizeof *frames)))goto done;
    if(g_nrzb && !(depths=(rzbuf *)calloc((size_t)g_nrzb,sizeof *depths)))goto done;
    for(int i=0;i<g_nrfb;i++)if(g_rfb[i].fbo) {
        const rfb *old=&g_rfb[i];rfb *f=&frames[i];
        f->w=old->w/g_scale*scale;f->h=old->h/g_scale*scale;f->fmt=old->fmt;
        f->stencil_dirty=old->stencil_dirty;f->color_version=old->color_version;
        f->depth_id=old->depth_id;f->depth_version=old->depth_version;f->depth_valid=old->depth_valid;
        if(reconfigure_attachment(&f->fbo,&f->depth,&f->tex,f->w,f->h))goto done;
    }
    for(int i=0;i<g_nrzb;i++) {
        depths[i]=g_rzb[i];depths[i].fbo=depths[i].depth=0;
        depths[i].w=g_rzb[i].w/g_scale*scale;depths[i].h=g_rzb[i].h/g_scale*scale;
        if(reconfigure_attachment(&depths[i].fbo,&depths[i].depth,NULL,depths[i].w,depths[i].h))goto done;
    }
    for(int i=0;i<g_nrfb;i++)if(g_rfb[i].fbo &&
        reconfigure_copy(g_rfb[i].fbo,frames[i].fbo,g_rfb[i].w,g_rfb[i].h,frames[i].w,frames[i].h,
                         GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT))goto done;
    for(int i=0;i<g_nrzb;i++)if(reconfigure_copy(g_rzb[i].fbo,depths[i].fbo,g_rzb[i].w,g_rzb[i].h,
                                               depths[i].w,depths[i].h,GL_DEPTH_BUFFER_BIT))goto done;
    /* The latest shared depth can still be dirty in a color attachment. Copy
     * it into the replacement shared allocation, leaving the live versions
     * and dirty flag untouched on every rejection path. */
    if(g_depth_dirty && g_depth_fb>=0 && g_depth_id>=0) {
        rfb *f=&frames[g_depth_fb];rzbuf *z=&depths[g_depth_id];
        int w=f->w<z->w?f->w:z->w,h=f->h<z->h?f->h:z->h;
        if(reconfigure_copy(f->fbo,z->fbo,w,h,w,h,GL_DEPTH_BUFFER_BIT))goto done;
        z->version++;f->depth_id=g_depth_id;f->depth_version=z->version;f->depth_valid=1;
    }
    host_mutex_lock(&g_m);int cancelled=g_quit || g_error[0];host_mutex_unlock(&g_m);
    if(cancelled)goto done;
    for(int i=0;i<g_nrfb;i++)scene_drop_target(&g_rfb[i]);
    for(int i=0;i<g_nrzb;i++){glDeleteFramebuffers(1,&g_rzb[i].fbo);glDeleteRenderbuffers(1,&g_rzb[i].depth);}
    free(g_rfb);g_rfb=frames;frames=NULL;
    /* Keep allocated capacity consistent with the replacement's size. */
    free(g_rzb);g_rzb=depths;depths=NULL;g_rzb_capacity=g_nrzb;
    g_scale=scale;g_depth_fb=g_depth_id=-1;g_depth_dirty=0;
    g_max_texture=tex;g_max_renderbuffer=rb;
    host_mutex_lock(&g_m);
    /* Preserve the diagnostic's last-created target identity. */
    for(int i=g_nrfb-1;i>=0;i--)if(g_rfb[i].fbo){g_attachment_width=g_rfb[i].w;g_attachment_height=g_rfb[i].h;break;}
    host_mutex_unlock(&g_m);
    result=0;
done:
    if(frames){for(int i=0;i<g_nrfb;i++)scene_drop_target(&frames[i]);free(frames);}
    if(depths){for(int i=0;i<g_nrzb;i++){if(depths[i].fbo)glDeleteFramebuffers(1,&depths[i].fbo);if(depths[i].depth)glDeleteRenderbuffers(1,&depths[i].depth);}free(depths);}
    /* Allocation/copy errors are recoverable and have been consumed here. */
    while(glGetError()!=0){}
    glBindFramebuffer(GL_FRAMEBUFFER,0);g_cur_fb_h=0;g_cur_fb_id=-1;g_applied_valid=0;
    return result;
}
static int reconfigure_worker(int scale,int fps,int stretch) {
    if(reconfigure_targets(scale))return -1;
    g_native_fps=fps;g_stretch=stretch;
    psp_frame_pacer_init_work(&g_native_pacer,fps,g_scene_mode?30:g_native_scene?fps:60);
    g_native_scene_deadline=0;g_native_swap_lead=g_host.swap_interval?16667:0;
    g_have_prev=0;g_nprev=0;g_last_arrival=0;g_interval_us=33367;
    g_scene_nprev=0;g_scene_last=0;g_scene_swap_lead=g_host.swap_interval?16667:0;
    /* Shadow images are derived, never authoritative. Release resized copies
     * now; the next legacy scene lazily rebuilds them at the committed scale. */
    for(int i=0;i<g_scene_target_count;i++)scene_drop_target(&g_scene_targets[i]);
    return 0;
}

static void *render_thread(void *arg) {
    (void)arg;
    const int r = setup();
    if(r==0)effect_diag_init();
    host_mutex_lock(&g_m);
    g_init_result = r == 0 ? 1 : -1;
    host_cond_broadcast(&g_cv);
    host_mutex_unlock(&g_m);
    if (r != 0) {if(g_host.shutdown)g_host.shutdown(g_host.user);return NULL;}

    if(g_native_fps)psp_frame_pacer_init_work(&g_native_pacer,g_native_fps,g_scene_mode?30:g_native_scene?g_native_fps:60);
    for (;;) {
        host_mutex_lock(&g_m);
        while (g_pending < 0 && !g_quit && !g_reconfigure.pending && g_capture_done>=g_capture_requested) host_cond_wait(&g_cv, &g_m);
        if(g_pending<0 && g_reconfigure.pending) {
            int scale=g_reconfigure.scale,fps=g_reconfigure.fps,stretch=g_reconfigure.stretch;
            int cancelled=g_quit || g_error[0];
            host_mutex_unlock(&g_m);int result=cancelled?-1:reconfigure_worker(scale,fps,stretch);host_mutex_lock(&g_m);
            g_reconfigure.result=result;g_reconfigure.done=1;g_reconfigure.pending=0;
            host_cond_broadcast(&g_cv);host_mutex_unlock(&g_m);continue;
        }
        if(g_pending<0 && g_capture_done<g_capture_requested){host_mutex_unlock(&g_m);capture_current();continue;}
        if(g_pending<0 && g_quit){host_mutex_unlock(&g_m);break;}
        gframe *f = &g_frames[g_pending];
        host_mutex_unlock(&g_m);

        uint64_t native_work_end=0;
        const c_present *front=find_present(f);
        if(front&&g_effect.enabled)effect_diag_frame();
        if(g_native_fps&&front){
            if(g_native_scene){int rate=front->fb<0?30:g_native_fps;if(g_native_pacer.work_fps!=rate)psp_frame_pacer_init_work(&g_native_pacer,rate,rate);}
            psp_frame_pacer_begin(&g_native_pacer,host_pacing_now());
            native_work_end=g_native_pacer.next_frame;
        }

        if (g_interp) {
            /* A copy, and the GE side goes on: showing two frames takes the
             * time the game spends on its next one. */
            gbuf_copy(&g_cur.cmd, &f->cmd);
            gbuf_copy(&g_cur.vtx, &f->vtx);
            host_mutex_lock(&g_m);
            g_pending = -1;
            host_cond_broadcast(&g_cv);
            host_mutex_unlock(&g_m);
            play_smooth(&g_cur);
            continue;
        }
        g_have_prev = 0;
        g_native_scene_deadline=native_work_end;
        if(g_scene_mode)scene_play(f,native_work_end);else play(f, PLAY_ALL, &f->vtx);
        if(g_native_scene&&front&&front->fb>=0){host_mutex_lock(&g_m);g_scene_native_count++;host_mutex_unlock(&g_m);}
        if(glGetError()!=0)gpu_fail("GL error during frame playback");

        host_mutex_lock(&g_m);
        /* Same release barrier after native work in BOTH output modes. A
         * skipped swap must not release the game earlier and change its
         * frame-dependent AI/counters through faster CPU backpressure. */
        while(native_work_end&&!g_quit&&host_pacing_now()<native_work_end)
            host_pacing_until(&g_native_wait,&g_m,native_work_end);
        g_pending = -1;
        host_cond_broadcast(&g_cv);
        host_mutex_unlock(&g_m);
    }
    effect_diag_stop();
    if(g_host.shutdown)g_host.shutdown(g_host.user);
    return NULL;
}

int psp_render_gl_start(const psp_gl_host *host, int scale) {
    if (g_started) return 0;
    if(g_native_fps&&g_interp)return -1;
    if(scale<1 || scale>6)return -1;
    g_host = *host;
    const char *effect=getenv("ASPHALT_EFFECT_DIAGNOSTICS");
    g_effect_requested=effect && !strcmp(effect,"1");
    g_quit=0;g_init_result=0;g_error[0]=0;
    memset(&g_reconfigure,0,sizeof g_reconfigure);
    g_capture_requested=g_capture_done=0;g_last_present_fb=-1;g_last_present_valid=0;
    g_attachment_width=g_attachment_height=0;
    g_scale = scale > 0 ? scale : 1;
    census_init();
    g_native_scene_deadline=0;g_native_swap_lead=host->swap_interval?16667:0;
    if(host_pacing_init(&g_native_wait))return -1;g_native_wait_ready=1;
    if (host_thread_create(&g_render_thread, 1 << 20, render_thread, NULL) != 0){
        if(g_native_wait_ready){host_pacing_destroy(&g_native_wait);g_native_wait_ready=0;}return -1;
    }
    host_mutex_lock(&g_m);
    while (!g_init_result) host_cond_wait(&g_cv, &g_m);
    const int ok = g_init_result > 0;
    host_mutex_unlock(&g_m);
    if (!ok) {host_thread_join(g_render_thread);if(g_native_wait_ready){host_pacing_destroy(&g_native_wait);g_native_wait_ready=0;}fputs("render_gl: no usable GL context\n", stderr); return -1; }
    g_started = 1;
    return 0;
}

int psp_render_gl_reconfigure(int scale,int fps,int stretch) {
    if(scale<1 || scale>6 || (fps!=30 && fps!=60) || (stretch!=0 && stretch!=1))return -1;
    host_mutex_lock(&g_m);
    if(!g_started || g_quit || g_error[0] || !g_native_fps || g_interp || g_reconfigure.pending ||
       g_frames[g_rec].cmd.n || g_frames[g_rec].vtx.n) {host_mutex_unlock(&g_m);return -1;}
    g_reconfigure.scale=scale;g_reconfigure.fps=fps;g_reconfigure.stretch=stretch;
    g_reconfigure.pending=1;g_reconfigure.done=0;g_reconfigure.result=-1;
    host_cond_broadcast(&g_cv);
    while(!g_reconfigure.done && !g_quit && !g_error[0])host_cond_wait(&g_cv,&g_m);
    int result=g_reconfigure.done && !g_quit && !g_error[0]?g_reconfigure.result:-1;
    host_mutex_unlock(&g_m);return result;
}

void psp_render_gl_stop(void) {
    if(!g_started)return;
    host_mutex_lock(&g_m);g_quit=1;host_cond_broadcast(&g_cv);if(g_native_wait_ready)host_pacing_wake(&g_native_wait);host_mutex_unlock(&g_m);
    host_thread_join(g_render_thread);g_started=0;
    if(g_native_wait_ready){host_pacing_destroy(&g_native_wait);g_native_wait_ready=0;}
    /* Context destruction releases all GL objects. Discard only unsubmitted
     * payloads: played commands already released their pixel allocations. */
    gframe *f=&g_frames[g_rec];
    for(size_t at=0;at<f->cmd.n;){chead *h=(chead *)(f->cmd.p+at);at+=h->size;
        if(h->op==C_TEX_NEW)free(((c_tex_new *)h)->pixels);
        if(h->op==C_PRESENT)free(((c_present *)h)->pixels);
    }
    for(int i=0;i<2;i++){free(g_frames[i].cmd.p);free(g_frames[i].vtx.p);memset(&g_frames[i],0,sizeof g_frames[i]);}
    free(g_rtex);g_rtex=NULL;g_nrtex=0;free(g_rfb);g_rfb=NULL;g_nrfb=0;
    free(g_rzb);g_rzb=NULL;g_nrzb=g_rzb_capacity=0;g_depth_fb=g_depth_id=-1;g_depth_dirty=0;
    scene_discard();
    free(g_cur.cmd.p);free(g_cur.vtx.p);memset(&g_cur,0,sizeof g_cur);
    free(g_ivtx.p);memset(&g_ivtx,0,sizeof g_ivtx);free(g_prev_vtx.p);memset(&g_prev_vtx,0,sizeof g_prev_vtx);
    free(g_prev);g_prev=NULL;g_nprev=g_capprev=0;g_have_prev=0;
    memset(g_fbs,0,sizeof g_fbs);g_nfb=0;g_next_fb=1;memset(g_tex,0,sizeof g_tex);g_ntex=0;g_next_tex=1;
    memset(g_fb_clut,0,sizeof g_fb_clut);g_nfb_clut=0;
    g_rec=0;g_pending=-1;g_bound=-1;g_last_tex=-1;g_last_draw=(size_t)-1;g_flip=0;
    g_prog=g_blit=g_vao=g_vbo=g_bvao=g_bvbo=g_show_tex=g_ovl_tex=g_cap_fbo=g_cap_tex=0;
    g_cur_fb_h=0;g_cur_fb_id=-1;g_resolve=g_depal=0;g_applied_valid=0;g_have_capture=0;
}

void psp_render_gl_stats(uint64_t *draws,uint64_t *batches,uint64_t *bytes,uint64_t *presented,uint64_t *readbacks) {
    host_mutex_lock(&g_m);
    if(draws)*draws=g_draws;if(batches)*batches=g_batches;if(bytes)*bytes=g_upload_bytes;
    if(presented)*presented=g_presented;if(readbacks)*readbacks=g_readbacks;
    host_mutex_unlock(&g_m);
}
