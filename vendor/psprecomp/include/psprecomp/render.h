/* psprecomp -- render backend interface. See docs/RENDERER.md.
 *
 * Display-list *interpretation* has one implementation (src/hle/ge.c);
 * *presentation* is pluggable. The split matters because the software
 * rasterizer is the oracle every other backend is checked against, and it has
 * to keep working on a machine with no GPU.
 *
 * Primitives arrive assembled -- vertex format decoding happens once, in the
 * interpreter, rather than being duplicated (and mis-duplicated) per backend.
 * Texel decoding is shared the same way: psp_tex_fetch() is the one place that
 * knows the texture formats, swizzling and CLUTs.
 */
#ifndef PSPRECOMP_RENDER_H
#define PSPRECOMP_RENDER_H

#include <stdint.h>
#include <stddef.h>
#include "render_transform.h"

/* A vertex after format decoding: screen space, colour resolved, texture
 * coordinates in texels (through mode addresses texels directly). Colours
 * here and below are 0xAABBGGRR, the GE's own byte order. */
typedef struct {
    int      x, y;
    uint32_t rgba;
    float    u, v;
    /* Depth after the viewport (0-65535), and 1/w for perspective-correct
     * texturing. A w of 0 means "no perspective": through-mode vertices and
     * anything built without one interpolate affinely. */
    float    z, w;
    /* Fog factor, 1 = none, 0 = all fog colour (FOG1/FOG2 on view-space z). */
    float    fog;
    /* x and y before rounding to pixels, for backends that rasterise with
     * subpixel precision. */
    float    fx, fy;
    float    local[3];       /* skinned model coordinates, visual provenance */
    int      local_valid;
} psp_vertex;

/* GE primitive types, from the PRIM argument's type field. */
enum {
    PSP_PRIM_POINTS = 0,
    PSP_PRIM_LINES,
    PSP_PRIM_LINE_STRIP,
    PSP_PRIM_TRIANGLES,
    PSP_PRIM_TRIANGLE_STRIP,
    PSP_PRIM_TRIANGLE_FAN,
    PSP_PRIM_SPRITES
};

/* GE texture / CLUT formats (TPSM, CMODE). 0-3 are also the framebuffer and
 * display formats. */
enum {
    PSP_TPSM_565 = 0, PSP_TPSM_5551, PSP_TPSM_4444, PSP_TPSM_8888,
    PSP_TPSM_T4, PSP_TPSM_T8, PSP_TPSM_T16, PSP_TPSM_T32,
    PSP_TPSM_DXT1, PSP_TPSM_DXT3, PSP_TPSM_DXT5
};

/* Everything about the GE's state that changes what a primitive's pixels
 * become. Filled by the interpreter from the state commands; read by the
 * backend. Field comments name the command each comes from. */
typedef struct {
    int      clear;          /* CLEAR bit 0: write vertex colour, nothing else */
    int      clear_color;    /* CLEAR bit 8: ...to RGB */
    int      clear_alpha;    /* CLEAR bit 9: ...to alpha (stencil) */
    int      gouraud;        /* SHADE */

    int      tex_enable;     /* TME */
    uint32_t tex_addr;       /* TBP0 + TBW0 high byte */
    int      tex_bw;         /* TBW0: buffer width, texels */
    int      tex_w, tex_h;   /* TSIZE0, as sizes not logs */
    int      tex_fmt;        /* TPSM */
    int      tex_swizzle;    /* TMODE bit 0 */
    int      tex_clamp_u, tex_clamp_v;   /* TWRAP */
    int      tex_func;       /* TFUNC bits 0-2 */
    int      tex_alpha;      /* TFUNC bit 8: texture alpha is used */
    int      tex_double;     /* TFUNC bit 16: colour doubling */
    uint32_t tex_env;        /* TEC, for the blend function */
    int      tex_min_linear; /* TFLT bit 0 (the software backend always samples nearest) */
    int      tex_mag_linear; /* TFLT bit 8 */

    const uint8_t *clut;     /* bytes captured at the last CLOAD */
    int      clut_fmt;       /* CMODE bits 0-1 */
    int      clut_shift;     /* CMODE bits 2-6 */
    int      clut_mask;      /* CMODE bits 8-15 */
    int      clut_start;     /* CMODE bits 16-20, in units of 16 entries */

    int      blend;          /* ABE */
    int      blend_src, blend_dst, blend_op;   /* ALPHA */
    uint32_t blend_sfix, blend_dfix;           /* SFIX, DFIX */

    int      atest;          /* ATE */
    int      atest_func, atest_ref, atest_mask; /* ATST */

    int      sc_x0, sc_y0, sc_x1, sc_y1;        /* SCISSOR1/2, inclusive */

    int      clear_depth;    /* CLEAR bit 10: ...to the vertex depth */
    int      ztest;          /* ZTE */
    int      ztest_func;     /* ZTST */
    int      zwrite;         /* !ZMSK */
    uint32_t zb_addr;        /* ZBP + ZBW high byte, in eDRAM */
    int      zb_stride;      /* ZBW */

    int      fog;            /* FGE: mix with fog_color by the vertex fog factor */
    uint32_t fog_color;      /* FCOL */

    uint32_t write_mask;     /* MASKRGB + MASKALPHA: 1 preserves a stored bit */
    int      stencil;        /* STENCILTESTENABLE; framebuffer alpha is stencil */
    int      stencil_func, stencil_ref, stencil_mask;
    int      stencil_fail, stencil_zfail, stencil_zpass;
    psp_render_transform transform; /* valid only for eligible3D primitives */
    int      depth_clamp;    /* GE1C: clamp transformed vertex depth before interpolation */
    /* GE MINZ/MAXZ reject integer fragment depths outside these inclusive
     * bounds. The interpreter disables the test for original through-mode
     * draws. A zero-initialized direct-backend state remains unrestricted. */
    int      depth_range_enable, depth_range_min, depth_range_max;
} psp_draw_state;

/* Semantic provenance is supplied by the CPU owner for original source
 * positions/commands. Unclassified geometry always preserves its coordinates. */
enum {
 PSP_SCREEN_PRESERVE=0, PSP_SCREEN_CENTER,
 PSP_SCREEN_HUD_WANTED, PSP_SCREEN_HUD_POSITION, PSP_SCREEN_HUD_MINIMAP,
 PSP_SCREEN_HUD_NITRO, PSP_SCREEN_HUD_MONEY, PSP_SCREEN_HUD_LAP,
 PSP_SCREEN_HUD_SPEED,
 /* Verified frontend font commands use the original perspective camera;
  * their decoded x has already received the port's camera aspect change. */
 PSP_SCREEN_FRONTEND_TEXT, PSP_SCREEN_HUD_EVENT,
 /* Source-proven members of the shared speed/NOS owner. Original maps them
  * exactly as SPEED; New gives the NOS label and x1/x2/x3 their own slots. */
 PSP_SCREEN_HUD_NITRO_LABEL, PSP_SCREEN_HUD_NITRO_MULTIPLIER,
 /* Original alternate SPEED/RPM/GEAR cluster, with a different author-space
  * origin from the analog panel. */
 PSP_SCREEN_HUD_SPEED_DIGITAL,
 /* Exact original gray GIM layers. New halves their alpha while retaining
  * opaque blue overlays; Original aliases them to their existing groups. */
 PSP_SCREEN_HUD_WANTED_BACKGROUND, PSP_SCREEN_HUD_NITRO_BACKGROUND,
 /* Original NOS pickup animation is an atlas sprite, not a GIM image. */
 PSP_SCREEN_HUD_NITRO_GROWTH,
 PSP_SCREEN_ROLE_COUNT
};
void psp_ge_set_screen_role_provider(int (*provider)(uint32_t prim_command,uint32_t position_address,unsigned position_bytes));
void psp_ge_set_screen_role_bounds(int role,float authored_left,float authored_right);
void psp_ge_set_screen_transform(float sx,float sy,float tx,float ty);
/* CPU owner only. Unknown layouts select Original; racing gates New so menus,
 * cinematic overlays and world draws retain their existing transforms. */
void psp_ge_set_hud_layout(int layout,int racing);
#if ASPHALT_HUD_DIAGNOSTICS
/* Observation begins with an actual mixed HUD producer, after loading. */
void psp_ge_hud_diagnostic_arm(int armed);
#endif
/* Maximum New-layout cash edge in the logical 480px presented viewport. */
void psp_ge_set_hud_money_limit(float right);
/* Native race HUD uses full viewport edges. Insets remain accepted for API
 * compatibility; the common menu transform continues to respect them. */
void psp_ge_set_screen_anchors(int enabled,float safe_left,float safe_right);
int psp_ge_adjust_screen_vertices(int prim,psp_vertex *v,int count,psp_draw_state *state,int through,int orthographic,int stride);
void psp_ge_draw_screen(int prim,psp_vertex *v,int count,const psp_draw_state *state,int through,int orthographic,int stride);
int psp_ge_adjust_screen_vertices_role(int role,int prim,psp_vertex *v,int count,psp_draw_state *state,int through,int orthographic,int stride);
void psp_ge_draw_screen_roles(int prim,psp_vertex *v,int count,const psp_draw_state *state,int through,int orthographic,int stride,const uint8_t *roles);
/* Optional native-port extension; unrecognized NOPs retain normal behavior. */
void psp_ge_set_nop_handler(void (*handler)(uint32_t address,uint32_t argument));
void psp_ge_set_pose_owner_provider(uint64_t (*provider)(uint32_t world_matrix_command));
/* CPU-owner callback after SetFrameBuf selects the front buffer, before present. */
void psp_display_set_present_hook(void (*handler)(void));
/* CPU-owner callback after present submits the completed frame. */
void psp_display_set_post_present_hook(void (*handler)(void));
/* Optional title integration: a repeated sync1 SetFrameBuf from this exact
 * caller only enables an unchanged front buffer, rather than ending a scene.
 * Zero disables the filter. Reset clears it. */
void psp_display_set_repeat_caller(uint32_t caller);

typedef struct {
    const char *name;

    int  (*init)(int width, int height);
    void (*shutdown)(void);

    /* GE_FBP / GE_FBW: the framebuffer being drawn into. */
    void (*set_target)(uint32_t addr, uint32_t stride, int fmt);

    /* One assembled primitive. `count` vertices, already in screen space,
     * drawn with state `st`. */
    void (*draw)(int prim, const psp_vertex *v, int count, const psp_draw_state *st);

    /* End of a display list -- a natural point to flush batched work. */
    void (*finish)(void);

    /* sceDisplaySetFrameBuf -- show what has accumulated. */
    void (*present)(void);

    /* The last frame shown, 480x272 as 0xAABBGGRR, for backends whose pixels
     * are not in guest memory. 0 on success; NULL or -1 means "read memory". */
    int  (*capture)(uint32_t *rgba);
} psp_render_backend;

/* Select a backend by name ("software", "null", ...). Returns 0 on success,
 * -1 if the name is unknown, leaving the current backend in place. */
int psp_render_select(const char *name);

/* The active backend. Never NULL -- defaults to software. */
const psp_render_backend *psp_render_current(void);

/* Backends provided by the runtime. */
extern const psp_render_backend psp_render_software;
extern const psp_render_backend psp_render_null;
extern const psp_render_backend psp_render_gl;

/* The OpenGL backend (src/render_gl.c): GL 3.3 core or GLES 3.0, on a render
 * thread of its own. The host owns the window and the context; these are
 * called on the render thread. */
typedef struct {
    void *user;
    int   gles;                                     /* GLES 3.0 shaders */
    int   (*init)(void *user);                      /* create the context, make it current; 0 = ok */
    void *(*get_proc)(void *user, const char *name);
    void  (*swap)(void *user);
    void  (*size)(void *user, int *w, int *h);      /* the drawable, in pixels */
    void  (*shutdown)(void *user);                 /* destroy context on its owning thread */
    int   swap_interval;                           /*1 when swap waits for display refresh*/
} psp_gl_host;

/* Start the render thread; `scale` is the internal resolution (1 = 480x272).
 * 0 when the context works, after which psp_render_select("gl") uses it. */
int  psp_render_gl_start(const psp_gl_host *host, int scale);
/* CPU owner only, immediately after present, with no unfinished commands.
 * Requires a session already started with a 30/60 FPS frame/scene rate setter
 * and interpolation disabled; legacy unpaced/interpolating sessions reject.
 * Waits for previous playback and atomically applies on the existing worker.
 * 0 = applied; -1 = rejected, retaining all previous settings and images.
 * A recoverable rejection does not set psp_render_gl_error(). */
int psp_render_gl_reconfigure(int scale,int fps,int stretch);
/* Startup-only native output target30/60. Work cadence remains60 in both;
 * all draw/resource commands run. No interpolation or guest timing changes. */
int psp_render_gl_set_frame_rate(int fps);
/* Original30Hz native scene cadence, optionally with a separately rasterized
 * visual transform midpoint for60Hz output. Startup only. */
int psp_render_gl_set_scene_rate(int fps);
/* One original scene per output slot. GPU scenes use fps; RAM/movie frames
 * retain30Hz work/presentation. No transform interpolation/shadow playback. */
int psp_render_gl_set_native_scene_rate(int fps);
void psp_render_gl_scene_stats(uint64_t *native_scenes,uint64_t *midpoints,uint64_t *matched_vertices,uint64_t *changed_vertices);
#ifdef ASPHALT_GPU_TESTS
int psp_render_gl_test_scene_waiting(void);
#endif
/* validate_scale is called only with a current context on the render thread. */
int psp_render_gl_validate_scale(int scale,char *error,size_t capacity);
/* Last target dimensions queried from its actual GPU depth/stencil attachment. */
void psp_render_gl_attachment_size(int *width,int *height);
void psp_render_gl_stop(void);
int  psp_render_gl_wait_idle(void); /* wait for submitted frame playback, diagnostics only */
const char *psp_render_gl_error(void); /* NULL until an unsupported state/device error */
void psp_render_gl_stats(uint64_t *draws,uint64_t *batches,uint64_t *upload_bytes,uint64_t *presented,uint64_t *readbacks);
/* An image drawn over the top-left corner of every frame (the host's FPS
 * line), 0xAABBGGRR; NULL removes it. Any thread. */
void psp_render_gl_overlay(const uint32_t *rgba, int w, int h);
/* The same, drawn at dst_w x dst_h window pixels (linearly filtered): a
 * phone's touch controls cover the screen, and are drawn at half its size. */
void psp_render_gl_overlay_scaled(const uint32_t *rgba, int w, int h, int dst_w, int dst_h);

/* In-between frames: a game that draws at 30 frames a second is shown at 60.
 * Each frame is shown after one made from it and the frame before (every
 * vertex of a draw both frames share halfway between them), half a frame
 * apart. The game's own pace and logic are unchanged; what is seen is half a
 * game frame later. Any time, from any thread. */
void psp_render_gl_set_interpolation(int on);
int  psp_render_gl_interpolation(void);

/* The picture stretched over the whole window instead of letterboxed at the
 * PSP's aspect (for a game told to draw wider: a widescreen hack). Any time,
 * from any thread. */
void psp_render_gl_set_stretch(int on);

/* Texture replacement (a host's texture pack, as PPSSPP's): asked on the GE's
 * thread each time a texture is decoded from memory, with its address, buffer
 * width (pixels), size, format (PSP_TPSM_*) and whether it is swizzled.
 * Returns PSP_REPLACE_NONE (decode it as usual), PSP_REPLACE_PENDING (decode
 * it as usual for now; ask again next frame, the replacement is being loaded)
 * or PSP_REPLACE_READY, with *w, *h and *data (malloc'd, the backend frees
 * it) set: RGBA8 when *gl_fmt is 0, else blocks of that compressed GL format
 * (one psp_render_gl_compressed_formats() offered), *bytes long. NULL turns
 * it off. */
enum { PSP_REPLACE_NONE = 0, PSP_REPLACE_PENDING = 1, PSP_REPLACE_READY = 2 };
typedef int (*psp_texture_replacer)(uint32_t addr, int bufw, int w, int h, int fmt, int swizzled,
                                    int *out_w, int *out_h, uint32_t **data, uint32_t *gl_fmt, uint32_t *bytes);
void psp_render_gl_set_replacer(psp_texture_replacer fn);

/* The compressed texture formats the GL takes, once it is up (-1 before):
 * PSP_GL_ASTC4x4 (GL_COMPRESSED_RGBA_ASTC_4x4_KHR, phones), PSP_GL_BC7
 * (GL_COMPRESSED_RGBA_BPTC_UNORM, PC graphics cards). */
enum { PSP_GL_ASTC4x4 = 1, PSP_GL_BC7 = 2 };
#define PSP_GL_FMT_ASTC4x4 0x93B0u
#define PSP_GL_FMT_BC7     0x8E8Cu
int psp_render_gl_compressed_formats(void);

/* Pixels written by the active backend -- the proof of life during bring-up. */
uint64_t psp_render_pixels(void);
/* Fragments depth-tested, rejected by the test, and depth pixels cleared.
 * PSPRECOMP_NO_DEPTH=1 disables the depth buffer. */
void psp_render_depth_stats(uint64_t *tested, uint64_t *failed, uint64_t *cleared);
void     psp_render_reset_pixels(void);

/* One pixel in format 0-3 (565, 5551, 4444, 8888) as 0xAABBGGRR. The 16-bit
 * formats replicate their high bits into the low ones. */
uint32_t psp_color_expand(uint32_t px, int fmt);

/* The texel at integer coordinates (x, y), wrapped or clamped, as 0xAABBGGRR.
 * DXT formats are not decoded: they return opaque magenta and are counted. */
uint32_t psp_tex_fetch(const psp_draw_state *st, int x, int y);
uint64_t psp_render_unsupported_texels(void);

/* Pixel tracing (PSPRECOMP_TRACE_PIXEL=x,y), on while psp_render_trace is set. */
extern int psp_render_trace;
void psp_render_trace_init(void);

#endif
