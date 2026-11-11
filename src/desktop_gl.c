#include "desktop_gl.h"
#include "psprecomp/render.h"
#include <GL/gl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static struct {HWND window;HDC dc;HGLRC context;HMODULE library;int running,vsync,gles;char vendor[128],renderer[160],version[128];} gpu;
#ifdef ASPHALT_GPU_TESTS
static void (*test_swap_hook)(void);
void asphalt_gl_test_set_swap_hook(void (*hook)(void)){test_swap_hook=hook;}
/* Fixture-only driver fault. Product builds never contain this switch. */
static void (APIENTRY *real_storage)(GLenum,GLenum,GLsizei,GLsizei);
static void APIENTRY failed_storage(GLenum target,GLenum format,GLsizei w,GLsizei h){
    (void)w;real_storage(target,format,-1,h); /* GL_INVALID_VALUE, no large allocation */
}
#endif
static void context_stop(void *unused) {
    (void)unused;wglMakeCurrent(NULL,NULL);
    if(gpu.context){wglDeleteContext(gpu.context);gpu.context=NULL;}
    if(gpu.dc){ReleaseDC(gpu.window,gpu.dc);gpu.dc=NULL;}
}
static void *proc(void *unused,const char *name) {
    (void)unused;PROC p=wglGetProcAddress(name);
    if(!p || (uintptr_t)p<=3 || (intptr_t)p==-1)p=GetProcAddress(gpu.library,name);
#ifdef ASPHALT_GPU_TESTS
    const char *fail=getenv("ASPHALT_GPU_FAIL_STORAGE");
    if(!strcmp(name,"glRenderbufferStorage")&&fail&&*fail=='1'){
        real_storage=(void (APIENTRY *)(GLenum,GLenum,GLsizei,GLsizei))p;
        return (void *)failed_storage;
    }
#endif
    return (void *)p;
}
static int context_start(void *unused) {
    (void)unused;gpu.dc=GetDC(gpu.window);if(!gpu.dc)return -1;
    PIXELFORMATDESCRIPTOR pfd={0};pfd.nSize=sizeof pfd;pfd.nVersion=1;
    pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;pfd.iPixelType=PFD_TYPE_RGBA;
    pfd.cColorBits=32;pfd.cAlphaBits=8;pfd.cDepthBits=24;pfd.cStencilBits=8;
    int format=ChoosePixelFormat(gpu.dc,&pfd);
    if(!format || (!GetPixelFormat(gpu.dc) && !SetPixelFormat(gpu.dc,format,&pfd)))return -1;
    HGLRC legacy=wglCreateContext(gpu.dc);gpu.context=legacy;
    if(!legacy || !wglMakeCurrent(gpu.dc,legacy))return -1;
    typedef HGLRC (WINAPI *create_fn)(HDC,HGLRC,const int *);
    create_fn create=(create_fn)proc(NULL,"wglCreateContextAttribsARB");
    if(!create)return -1;
    /* WGL_EXT_create_context_es_profile lets capable Windows drivers run
     * the actual GLES API/ESSL path, rather than only desktop GLSL. */
    const int attributes[]={0x2091,3,0x2092,gpu.gles?0:3,0x9126,gpu.gles?4:1,0};
    HGLRC modern=create(gpu.dc,NULL,attributes);
    if(!modern){fprintf(stderr,"GPU: cannot create %s context (Windows error %lu)\n",gpu.gles?"GLES 3":"OpenGL 3.3",GetLastError());return -1;}
    wglMakeCurrent(NULL,NULL);wglDeleteContext(legacy);gpu.context=modern;
    if(!wglMakeCurrent(gpu.dc,modern))return -1;
    snprintf(gpu.vendor,sizeof gpu.vendor,"%s",glGetString(GL_VENDOR));
    snprintf(gpu.renderer,sizeof gpu.renderer,"%s",glGetString(GL_RENDERER));
    snprintf(gpu.version,sizeof gpu.version,"%s",glGetString(GL_VERSION));
    fprintf(stderr,"GPU: %s | %s | %s\n",gpu.vendor,gpu.renderer,gpu.version);
    typedef BOOL (WINAPI *swap_fn)(int);swap_fn interval=(swap_fn)proc(NULL,"wglSwapIntervalEXT");
    if(interval)interval(gpu.vsync);
    return 0;
}
static void swap(void *unused){(void)unused;
#ifdef ASPHALT_GPU_TESTS
    if(test_swap_hook)test_swap_hook();
#endif
    SwapBuffers(gpu.dc);
#ifdef ASPHALT_GPU_TESTS
    const char *delay=getenv("ASPHALT_GPU_TEST_SWAP_DELAY");if(delay&&*delay=='1')Sleep(16);
#endif
}
static void size(void *unused,int *w,int *h){(void)unused;RECT r={0};GetClientRect(gpu.window,&r);*w=r.right;*h=r.bottom;}
int asphalt_gl_start(HWND window,int vsync) {
    return asphalt_gl_start_scaled(window,vsync,1);
}
int asphalt_gl_start_scaled(HWND window,int vsync,int scale) {
    if(gpu.running)return -1;
    memset(&gpu,0,sizeof gpu);gpu.window=window;gpu.vsync=vsync;
    const char *api=getenv("ASPHALT_GPU_API");gpu.gles=api && !strcmp(api,"gles");
    if(api && *api && strcmp(api,"gl") && strcmp(api,"gles"))return -1;
    gpu.library=LoadLibrary("opengl32.dll");if(!gpu.library)return -1;
    psp_gl_host host={0};host.gles=gpu.gles;host.init=context_start;host.get_proc=proc;host.swap=swap;host.size=size;host.shutdown=context_stop;host.swap_interval=vsync;
    if(psp_render_gl_start(&host,scale) || psp_render_select("gl")) {psp_render_gl_stop();FreeLibrary(gpu.library);gpu.library=NULL;return -1;}
    gpu.running=1;return 0;
}
void asphalt_gl_stop(void) {
    if(gpu.running){psp_render_gl_stop();psp_render_select("software");gpu.running=0;}
    if(gpu.library){FreeLibrary(gpu.library);gpu.library=NULL;}
}
static void quoted(FILE *out,const char *s){fputc('"',out);for(;*s;s++){if(*s=='"'||*s=='\\')fputc('\\',out);fputc((unsigned char)*s<32?'?':*s,out);}fputc('"',out);}
void asphalt_gl_report(FILE *out) {
    fprintf(out,"{\"api\":\"%s\",\"vendor\":",gpu.gles?"OpenGL ES":"OpenGL");quoted(out,gpu.vendor);fputs(",\"renderer\":",out);quoted(out,gpu.renderer);
    fputs(",\"version\":",out);quoted(out,gpu.version);fprintf(out,",\"vsync\":%d,\"context_alive\":%s}",gpu.vsync,gpu.context?"true":"false");
}
