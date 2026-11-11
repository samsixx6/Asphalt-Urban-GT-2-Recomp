/* Android's resumable, one-session :game adapter. No device/UI thread touches the
 * PSP register file, guest RAM, scheduler or guest-owned PCM pointers. */
#include "android_host.h"
#include "android_lifecycle.h"
#include "audio_mixer.h"
#include "savedata.h"
#include "display_settings.h"
#include "display_session.h"
#include "performance_report.h"
#include "native_engine.h"
#include "race_controls.h"
#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_FRAMES 512
#define ERROR_CAPACITY 512
static struct {
    pthread_mutex_t state_lock, input_lock, audio_lock;
    pthread_cond_t audio_ready, state_changed;
    int claimed, audio_started, audio_ready_state, gpu_started;
    int finished;
    asphalt_android_lifecycle life;
    atomic_int stopping, suspended, audio_quit, audio_error;
    char status[ERROR_CAPACITY], device_error[ERROR_CAPACITY];
    char image_path[PATH_MAX], runtime_root[PATH_MAX];
    int iso_fd;
    ANativeWindow *pending_window; /* state_lock; never borrowed by GLES */
    ANativeWindow *window; /* render-thread-owned acquired reference */
    unsigned window_generation;
    uint32_t buttons;
    int geometry[6],applied_geometry[6]; /* drawable w/h, safe l/t/r/b */
    uint8_t ax, ay;
    pthread_t audio_thread;
    asphalt_audio_mixer mixer;
    uint64_t audio_written;
    asphalt_perf_report performance;
    _Atomic uint64_t swap_wall_ns;
    /* EGL and the GLES library are accessed only on the render thread. */
    EGLDisplay display;
    EGLSurface surface;
    EGLSurface parking;
    EGLConfig config;
    EGLint visual;
    EGLContext context;
    void *gles_library;
    JavaVM *vm;
    jclass activity_class;
    jmethodID storage_before, storage_after;
} host = {
    .state_lock = PTHREAD_MUTEX_INITIALIZER,
    .input_lock = PTHREAD_MUTEX_INITIALIZER,
    .audio_lock = PTHREAD_MUTEX_INITIALIZER,
    .audio_ready = PTHREAD_COND_INITIALIZER,
    .state_changed = PTHREAD_COND_INITIALIZER,
    .suspended = 1,
    .iso_fd = -1, .ax = 128, .ay = 128,
    .display = EGL_NO_DISPLAY, .surface = EGL_NO_SURFACE, .parking = EGL_NO_SURFACE,
    .context = EGL_NO_CONTEXT
};

uint64_t asphalt_android_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static asphalt_perf_point performance_point(void) {
    asphalt_perf_point p={0};struct timespec t;
    p.wall_ms=asphalt_android_now_ms();
    if(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)){
        p.cpu_valid=1;p.cpu_ns=(uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
    }
    psp_render_gl_stats(&p.draws,&p.batches,&p.upload_bytes,&p.frames,&p.readbacks);
    p.swap_ns=atomic_load_explicit(&host.swap_wall_ns,memory_order_relaxed);
    return p;
}
static void device_error(const char *message) {
    pthread_mutex_lock(&host.state_lock);
    if (!host.device_error[0]) {
        snprintf(host.device_error, sizeof host.device_error, "%s", message);
        __android_log_print(ANDROID_LOG_ERROR, "AsphaltNative", "%s", message);
        pthread_cond_broadcast(&host.state_changed);
        fprintf(stderr, "Android device failure: %s\n", message);
    }
    pthread_mutex_unlock(&host.state_lock);
}
static void egl_error(const char *operation) {
    char error[128];
    snprintf(error, sizeof error, "%s failed (EGL 0x%04X)", operation,
             (unsigned)eglGetError());
    device_error(error);
}
/* The UI only publishes a generation and an acquired pending reference. All
 * EGL/GL work and the active window's final release belong to this worker. */
static void window_detach(void) {
    if (host.surface != EGL_NO_SURFACE) {
        if(!eglMakeCurrent(host.display,host.parking,host.parking,host.context))
            egl_error("Retain GLES context on parking surface");
        eglDestroySurface(host.display,host.surface);
        host.surface=EGL_NO_SURFACE;
    }
}
static void window_park(void) {
    window_detach();
    if (host.window) { ANativeWindow_release(host.window);host.window=NULL; }
}
static int window_bind(void) {
    for (;;) {
        pthread_mutex_lock(&host.state_lock);
        int running=asphalt_android_lifecycle_running(&host.life);
        unsigned generation=host.life.generation;
        int stopping=host.life.stopping;
        int failed=host.device_error[0]!=0;
        if(failed){pthread_mutex_unlock(&host.state_lock);return 0;}
        if (running && host.window && host.surface!=EGL_NO_SURFACE && host.window_generation==generation) {
            pthread_mutex_unlock(&host.state_lock);return 1;
        }
        pthread_mutex_unlock(&host.state_lock);
        window_detach();
        if (stopping) return 0;
        pthread_mutex_lock(&host.state_lock);
        while (!asphalt_android_lifecycle_running(&host.life) && !host.life.stopping && !host.device_error[0])
            pthread_cond_wait(&host.state_changed,&host.state_lock);
        if (host.life.stopping || host.device_error[0]) { pthread_mutex_unlock(&host.state_lock);return 0; }
        generation=host.life.generation;
        ANativeWindow *old=NULL;
        if (generation!=host.window_generation || !host.window) {
            old=host.window;host.window=host.pending_window;host.pending_window=NULL;
        }
        host.window_generation=generation;
        pthread_mutex_unlock(&host.state_lock);
        if (old) ANativeWindow_release(old);
        if (!host.window) continue;
        int valid=!ANativeWindow_setBuffersGeometry(host.window,0,0,host.visual);
        if (valid) host.surface=eglCreateWindowSurface(host.display,host.config,host.window,NULL);
        if (valid && host.surface!=EGL_NO_SURFACE &&
            eglMakeCurrent(host.display,host.surface,host.surface,host.context)) {
            eglSwapInterval(host.display,1);
            /* Recheck generation: Android may have destroyed it during binding. */
            continue;
        }
        EGLint failure=eglGetError();
        pthread_mutex_lock(&host.state_lock);
        int stale=generation!=host.life.generation || !asphalt_android_lifecycle_running(&host.life);
        if (!stale) {
            host.life.have_window=0;
            atomic_store_explicit(&host.suspended,1,memory_order_release);
        }
        pthread_mutex_unlock(&host.state_lock);
        if (!stale && failure!=EGL_BAD_SURFACE && failure!=EGL_BAD_NATIVE_WINDOW && failure!=EGL_SUCCESS) {
            device_error("Cannot rebind the Android GLES window");return 0;
        }
    }
}
static void context_stop(void *unused) {
    (void)unused;
    window_park();
    if (host.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(host.display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (host.context != EGL_NO_CONTEXT) eglDestroyContext(host.display,host.context);
        if (host.parking != EGL_NO_SURFACE) eglDestroySurface(host.display,host.parking);
        eglTerminate(host.display);
    }
    host.context=EGL_NO_CONTEXT;host.surface=host.parking=EGL_NO_SURFACE;
    host.display=EGL_NO_DISPLAY;
    if (host.gles_library) dlclose(host.gles_library);
    host.gles_library=NULL;
}
static int context_start(void *unused) {
    (void)unused;
    EGLint count;
    const EGLint attributes[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT|EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
        EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE};
    const EGLint context_attributes[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    const EGLint parking_attributes[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
    host.display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (host.display==EGL_NO_DISPLAY || !eglInitialize(host.display,NULL,NULL)) {
        egl_error("eglInitialize");return -1;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(host.display,attributes,&host.config,1,&count) || count!=1 ||
        !eglGetConfigAttrib(host.display,host.config,EGL_NATIVE_VISUAL_ID,&host.visual)) {
        egl_error("GLES3 window/pbuffer config");return -1;
    }
    host.parking=eglCreatePbufferSurface(host.display,host.config,parking_attributes);
    host.context=eglCreateContext(host.display,host.config,EGL_NO_CONTEXT,context_attributes);
    if (host.parking==EGL_NO_SURFACE || host.context==EGL_NO_CONTEXT ||
        !eglMakeCurrent(host.display,host.parking,host.parking,host.context)) {
        egl_error("GLES3 retained context/pbuffer");return -1;
    }
    host.gles_library=dlopen("libGLESv3.so",RTLD_NOW|RTLD_LOCAL);
    if (!host.gles_library) {device_error("Cannot open libGLESv3.so");return -1;}
    fprintf(stderr,"Android GPU: %s | %s | %s\n",glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION));
    return 0;
}
static void *get_proc(void *unused,const char *name) {
    (void)unused;void *address=(void *)eglGetProcAddress(name);
    return address?address:dlsym(host.gles_library,name);
}
static void swap(void *unused) {
    (void)unused;
    /* A pause after size() must not swap to an abandoned Android surface. The
     * queued frame may finish on its retained FBO, then parks before presenting. */
    if (!window_bind()) return;
    struct timespec before={0},after={0};
    int timed=!clock_gettime(CLOCK_MONOTONIC,&before);
    if (!eglSwapBuffers(host.display,host.surface)) {
        EGLint failure=eglGetError();
        if (failure==EGL_BAD_SURFACE || failure==EGL_BAD_NATIVE_WINDOW) {
            pthread_mutex_lock(&host.state_lock);
            if (host.window_generation==host.life.generation) {
                host.life.have_window=0;
                atomic_store_explicit(&host.suspended,1,memory_order_release);
            }
            pthread_mutex_unlock(&host.state_lock);
            window_park();
        } else device_error("eglSwapBuffers failed; GLES context unavailable");
    }
    if(timed&&!clock_gettime(CLOCK_MONOTONIC,&after)){
        uint64_t a=(uint64_t)before.tv_sec*1000000000ull+before.tv_nsec;
        uint64_t b=(uint64_t)after.tv_sec*1000000000ull+after.tv_nsec;
        if(b>=a)atomic_fetch_add_explicit(&host.swap_wall_ns,b-a,memory_order_relaxed);
    }
}
static void size(void *unused,int *width,int *height) {
    (void)unused;*width=*height=0;
    if (!window_bind()) return;
    EGLint w=0,h=0;
    if (!eglQuerySurface(host.display,host.surface,EGL_WIDTH,&w) ||
        !eglQuerySurface(host.display,host.surface,EGL_HEIGHT,&h) || w<=0 || h<=0) return;
    *width=w;*height=h;
    pthread_mutex_lock(&host.input_lock);
    host.geometry[0]=w;host.geometry[1]=h;
    pthread_mutex_unlock(&host.input_lock);
}
static void audio_sink(int channel, const int16_t *pcm, uint32_t frames,
                        int stereo, int left, int right) {
    /* push copies/scales samples while this thread still owns the guest CPU. */
    pthread_mutex_lock(&host.audio_lock);
    asphalt_audio_push(&host.mixer, channel, pcm, frames, stereo, left, right);
    pthread_mutex_unlock(&host.audio_lock);
}
static void audio_error_callback(AAudioStream *stream, void *user, aaudio_result_t error) {
    (void)stream; (void)user;
    /* Closing/stop/writes are forbidden on the AAudio error callback thread. */
    atomic_store_explicit(&host.audio_error, error, memory_order_release);
}
static void audio_failure(const char *operation, aaudio_result_t result) {
    char error[ERROR_CAPACITY];
    snprintf(error, sizeof error, "%s: %s (%d)", operation,
             AAudio_convertResultToText(result), (int)result);
    device_error(error);
}
static void signal_audio_ready(int success) {
    pthread_mutex_lock(&host.state_lock);
    host.audio_ready_state = success ? 1 : -1;
    pthread_cond_broadcast(&host.audio_ready);
    pthread_mutex_unlock(&host.state_lock);
}
static AAudioStream *audio_open(void) {
    AAudioStreamBuilder *builder=NULL;AAudioStream *stream=NULL;
    aaudio_result_t result=AAudio_createStreamBuilder(&builder);
    if (result!=AAUDIO_OK) {audio_failure("AAudio builder",result);return NULL;}
    AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder,2);
    AAudioStreamBuilder_setSampleRate(builder,44100);
    AAudioStreamBuilder_setErrorCallback(builder,audio_error_callback,NULL);
    result=AAudioStreamBuilder_openStream(builder,&stream);
    AAudioStreamBuilder_delete(builder);
    if(result!=AAUDIO_OK){
        if(result!=AAUDIO_ERROR_DISCONNECTED)audio_failure("AAudio open",result);
        return NULL;
    }
    if(AAudioStream_getSampleRate(stream)!=44100 || AAudioStream_getChannelCount(stream)!=2 ||
       AAudioStream_getFormat(stream)!=AAUDIO_FORMAT_PCM_I16){
        device_error("AAudio did not provide PCM16 stereo 44100 Hz");AAudioStream_close(stream);return NULL;
    }
    atomic_store_explicit(&host.audio_error,AAUDIO_OK,memory_order_release);
    result=AAudioStream_requestStart(stream);
    if(result!=AAUDIO_OK){
        if(result!=AAUDIO_ERROR_DISCONNECTED)audio_failure("AAudio start",result);
        AAudioStream_close(stream);return NULL;
    }
    return stream;
}
static void audio_close(AAudioStream **stream) {
    if(*stream){AAudioStream_requestStop(*stream);AAudioStream_close(*stream);*stream=NULL;}
}
static void *audio_loop(void *unused) {
    (void)unused;AAudioStream *stream=NULL;int reconnects=0;
    /* Ready means the worker can accept pause/stop even before a window exists. */
    signal_audio_ready(1);
    while(!atomic_load_explicit(&host.audio_quit,memory_order_acquire)){
        if(atomic_load_explicit(&host.suspended,memory_order_acquire)){
            /* Closing discards the device queue. Do not drain/mix guest PCM while
             * suspended, and never close from the AAudio callback or UI thread. */
            audio_close(&stream);
            pthread_mutex_lock(&host.state_lock);
            while(atomic_load_explicit(&host.suspended,memory_order_acquire) &&
                  !atomic_load_explicit(&host.audio_quit,memory_order_acquire))
                pthread_cond_wait(&host.state_changed,&host.state_lock);
            pthread_mutex_unlock(&host.state_lock);
            reconnects=0;continue;
        }
        if(!stream){
            stream=audio_open();
            if(!stream){
                if(++reconnects>=10){device_error("AAudio could not reconnect");break;}
                struct timespec retry={0,100000000};nanosleep(&retry,NULL);continue;
            }
            reconnects=0;
        }
        int16_t pcm[AUDIO_FRAMES*2];
        pthread_mutex_lock(&host.audio_lock);
        if(atomic_load_explicit(&host.suspended,memory_order_acquire)){
            pthread_mutex_unlock(&host.audio_lock);continue;
        }
        asphalt_audio_mix(&host.mixer,pcm,AUDIO_FRAMES);
        pthread_mutex_unlock(&host.audio_lock);
        int offset=0,timeouts=0;
        while(offset<AUDIO_FRAMES && !atomic_load_explicit(&host.audio_quit,memory_order_acquire) &&
              !atomic_load_explicit(&host.suspended,memory_order_acquire)){
            aaudio_result_t result=atomic_load_explicit(&host.audio_error,memory_order_acquire);
            if(result==AAUDIO_OK)result=AAudioStream_write(stream,pcm+offset*2,AUDIO_FRAMES-offset,100000000);
            if(result==AAUDIO_ERROR_DISCONNECTED){audio_close(&stream);break;}
            if(result<0){audio_failure("AAudio write",result);goto done;}
            if(!result && ++timeouts>=10){device_error("AAudio output stalled for one second");goto done;}
            offset+=result;
            pthread_mutex_lock(&host.audio_lock);host.audio_written+=(unsigned)result;pthread_mutex_unlock(&host.audio_lock);
        }
    }
done:
    audio_close(&stream);return NULL;
}

int asphalt_android_host_start(void) {
    if(asphalt_display_session_start(host.runtime_root)){
        device_error("Cannot read/write Recomp Settings. Check device storage.");return -1;
    }
    pthread_mutex_lock(&host.input_lock);
    if(host.geometry[0]<=0 || host.geometry[1]<=0){host.geometry[0]=480;host.geometry[1]=272;}
    memcpy(host.applied_geometry,host.geometry,sizeof host.geometry);
    pthread_mutex_unlock(&host.input_lock);
    if(asphalt_display_session_geometry(host.applied_geometry[0],host.applied_geometry[1],host.applied_geometry+2)){
        device_error("Invalid game surface dimensions");return -1;
    }
    asphalt_display_config display_config=asphalt_display_session_config();
    psp_gl_host graphics = { .gles = 1, .init = context_start, .get_proc = get_proc,
        .swap = swap, .size = size, .shutdown = context_stop, .swap_interval=1 };
    if (psp_render_gl_start(&graphics, display_config.scale)) {
        /* render_gl joins a failed initialization in stop(). */
        psp_render_gl_stop();
        device_error("GLES initialization failed. Tap Play to restore the last confirmed Recomp Settings."); return -1;
    }
    host.gpu_started = 1;
    if (psp_render_select("gl")) { device_error("GLES renderer unavailable"); return -1; }
    int result = pthread_create(&host.audio_thread, NULL, audio_loop, NULL);
    if (result) { device_error("Cannot start Android audio worker"); return -1; }
    host.audio_started = 1;
    pthread_mutex_lock(&host.state_lock);
    while (!host.audio_ready_state) pthread_cond_wait(&host.audio_ready, &host.state_lock);
    int ok = host.audio_ready_state > 0;
    pthread_mutex_unlock(&host.state_lock);
    if (!ok) return -1;
    psp_audio_set_sink(audio_sink);
    char perf_path[PATH_MAX];
    int length=snprintf(perf_path,sizeof perf_path,"%s/performance.jsonl",host.runtime_root);
    if(length>0&&(size_t)length<sizeof perf_path){
        asphalt_perf_point baseline=performance_point();
        (void)asphalt_perf_report_open(&host.performance,perf_path,&baseline);
    }
    return 0;
}
void asphalt_android_host_stop(void) {
    asphalt_perf_report_close(&host.performance);
    /* Called only by the CPU owner. Parked guest threads stay parked until
     * Android terminates this one-session process. Never reset/free PSP RAM. */
    psp_audio_set_sink(NULL);
    atomic_store_explicit(&host.audio_quit, 1, memory_order_release);
    pthread_mutex_lock(&host.state_lock);pthread_cond_broadcast(&host.state_changed);pthread_mutex_unlock(&host.state_lock);
    if (host.audio_started) { pthread_join(host.audio_thread, NULL); host.audio_started = 0; }
    if (host.gpu_started) { psp_render_gl_stop(); host.gpu_started = 0; }
}
int asphalt_android_host_tick(uint32_t *buttons, uint8_t *ax, uint8_t *ay,
                              char *error, size_t capacity) {
    if (atomic_load_explicit(&host.stopping, memory_order_acquire)) {
        snprintf(error, capacity, "Android activity stopped"); return 1;
    }
    pthread_mutex_lock(&host.state_lock);
    /* The current guest owner retains ThreadMan's baton here. Its cooperative
     * virtual clock and every other guest pthread stay frozen until resume. */
    while(!asphalt_android_lifecycle_running(&host.life) && !host.life.stopping && !host.device_error[0])
        pthread_cond_wait(&host.state_changed,&host.state_lock);
    int stopped=host.life.stopping;
    pthread_mutex_unlock(&host.state_lock);
    if(stopped){snprintf(error,capacity,"Android activity stopped");return 1;}
    const char *gpu_error = psp_render_gl_error();
    if (gpu_error) device_error(gpu_error);
    pthread_mutex_lock(&host.state_lock);
    int failed = host.device_error[0] != 0;
    if (failed) snprintf(error, capacity, "%s", host.device_error);
    pthread_mutex_unlock(&host.state_lock);
    if (failed) return -1;
    pthread_mutex_lock(&host.input_lock);
    *buttons = host.buttons; *ax = host.ax; *ay = host.ay;
    int geometry[6];memcpy(geometry,host.geometry,sizeof geometry);
    pthread_mutex_unlock(&host.input_lock);
    if(memcmp(geometry,host.applied_geometry,sizeof geometry)){
        if(asphalt_display_session_geometry(geometry[0],geometry[1],geometry+2)){
            snprintf(error,capacity,"Invalid display geometry");return -1;
        }
        memcpy(host.applied_geometry,geometry,sizeof geometry);
    }
    return 0;
}
void asphalt_android_host_report(FILE *out) {
    asphalt_perf_point point=performance_point();
    asphalt_display_config config=asphalt_display_session_config();
    (void)asphalt_perf_report_sample(&host.performance,&point,config.scale,config.fps,config.aspect);
    /* Existing five-second/final report runs on the guest CPU owner. It is
     * independent of the UI-thread swap counter and reads no guest memory. */
    asphalt_native_engine_report(out);
    uint64_t draws = 0, batches = 0, bytes = 0, presented = 0, readbacks = 0;
    psp_render_gl_stats(&draws, &batches, &bytes, &presented, &readbacks);
    pthread_mutex_lock(&host.audio_lock);
    fprintf(out, "android-host: gpu_draws=%llu batches=%llu upload_bytes=%llu presented=%llu "
            "readbacks=%llu audio_written=%llu input=%llu mixed=%llu nonzero=%llu dropped=%llu peak=%u\n",
            (unsigned long long)draws, (unsigned long long)batches, (unsigned long long)bytes,
            (unsigned long long)presented, (unsigned long long)readbacks,
            (unsigned long long)host.audio_written, (unsigned long long)host.mixer.input_frames,
            (unsigned long long)host.mixer.mixed_frames, (unsigned long long)host.mixer.nonzero_frames,
            (unsigned long long)host.mixer.dropped_frames, host.mixer.peak);
    pthread_mutex_unlock(&host.audio_lock);
}
_Noreturn void asphalt_android_finish(const char *reason) {
    atomic_store_explicit(&host.stopping, 1, memory_order_release);
    pthread_mutex_lock(&host.state_lock);asphalt_android_lifecycle_stop(&host.life);
    pthread_cond_broadcast(&host.state_changed);pthread_mutex_unlock(&host.state_lock);
    asphalt_android_host_stop();
    /* The render join has released the active window. Pending references were
     * never borrowed by GLES and are safe to release after that join. */
    pthread_mutex_lock(&host.state_lock);
    if(host.pending_window){ANativeWindow_release(host.pending_window);host.pending_window=NULL;}
    pthread_mutex_unlock(&host.state_lock);
    if (host.iso_fd >= 0) { close(host.iso_fd); host.iso_fd = -1; }
    pthread_mutex_lock(&host.state_lock);
    snprintf(host.status, sizeof host.status, "%s", reason);
    host.finished=1;pthread_cond_broadcast(&host.state_changed);
    pthread_mutex_unlock(&host.state_lock);
    __android_log_print(ANDROID_LOG_ERROR, "AsphaltNative", "Session ended: %s", reason);
    fflush(NULL);
    /* Deliberately retain the runtime's CPU baton. Returning or unwinding via
     * ThreadMan would hand it to another guest and continue after a fatal trap.
     * Its other pthreads and RAM remain intact until Java kills :game. */
    pthread_exit(NULL);
}
static void *game_loop(void *unused) {
    (void)unused;
    char iso_path[64];
    snprintf(iso_path, sizeof iso_path, "/proc/self/fd/%d", host.iso_fd);
    char log_path[PATH_MAX];
    int count = snprintf(log_path, sizeof log_path, "%s/android-native.log", host.runtime_root);
    if (count < 0 || (size_t)count >= sizeof log_path)
        asphalt_android_finish("Runtime log path is too long");
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log_fd < 0) asphalt_android_finish("Cannot create Android runtime log");
    if (dup2(log_fd, STDOUT_FILENO) < 0 || dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd); asphalt_android_finish("Cannot redirect Android runtime log");
    }
    if (log_fd != STDOUT_FILENO && log_fd != STDERR_FILENO) close(log_fd);
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "Android native session started; ISO descriptor=%d\n", host.iso_fd);
    asphalt_android_game_run(host.image_path, iso_path, host.runtime_root);
    asphalt_android_finish("Native game returned unexpectedly");
}
static int copy_string(JNIEnv *env, jstring input, char *out, size_t capacity) {
    if (!input) return -1;
    const char *text = (*env)->GetStringUTFChars(env, input, NULL);
    if (!text) return -1;
    size_t length = strlen(text);
    int valid = length > 0 && length < capacity;
    if (valid) memcpy(out, text, length + 1);
    (*env)->ReleaseStringUTFChars(env, input, text);
    return valid ? 0 : -1;
}
static int persistence_call(jmethodID method) {
    if(!host.vm || !host.activity_class || !method)return -1;
    JNIEnv *env=NULL;int attached=0;
    jint result=(*host.vm)->GetEnv(host.vm,(void **)&env,JNI_VERSION_1_6);
    if(result==JNI_EDETACHED){
        if((*host.vm)->AttachCurrentThread(host.vm,&env,NULL)!=JNI_OK)return -1;
        attached=1;
    } else if(result!=JNI_OK)return -1;
    jint status=(*env)->CallStaticIntMethod(env,host.activity_class,method);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);status=-1;}
    if(attached)(*host.vm)->DetachCurrentThread(host.vm);
    return status;
}
static int persistence_before(void *unused){(void)unused;return persistence_call(host.storage_before);}
static int persistence_after(void *unused){(void)unused;return persistence_call(host.storage_after);}
void asphalt_android_install_persistence(void) {
    asphalt_savedata_set_persistence(persistence_before,persistence_after,NULL);
    asphalt_display_settings_set_persistence(persistence_before,persistence_after,NULL);
}
#define JNI_METHOD(name) Java_com_samsixx_asphalt2recomp_GameActivity_##name
JNIEXPORT jint JNICALL JNI_METHOD(nativeRecoverSaves)(JNIEnv *env,jclass clazz,jstring root){
    (void)clazz;char path[PATH_MAX];
    return copy_string(env,root,path,sizeof path)?-1:asphalt_savedata_recover(path);
}
JNIEXPORT jint JNICALL JNI_METHOD(nativeValidateSaves)(JNIEnv *env,jclass clazz,jstring root){
    (void)clazz;char path[PATH_MAX];
    return copy_string(env,root,path,sizeof path)?-1:asphalt_savedata_validate(path);
}
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeFPSCounterEnabled)(JNIEnv *env,jclass clazz){
    (void)env;(void)clazz;return asphalt_display_session_fps_counter()?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jint JNICALL JNI_METHOD(nativeUILayout)(JNIEnv *env,jclass clazz){
    (void)env;(void)clazz;return asphalt_display_session_controls_layout();
}
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeRaceControlsHidden)(JNIEnv *env,jclass clazz){
    (void)env;(void)clazz;return asphalt_race_controls_hidden()?JNI_TRUE:JNI_FALSE;
}
static void publish_running_locked(void){
    atomic_store_explicit(&host.suspended,!asphalt_android_lifecycle_running(&host.life),memory_order_release);
    pthread_cond_broadcast(&host.state_changed);
}
JNIEXPORT void JNICALL JNI_METHOD(nativeSetResumed)(JNIEnv *env,jclass clazz,jboolean resumed){
    (void)env;(void)clazz;
    pthread_mutex_lock(&host.state_lock);
    asphalt_android_lifecycle_resume(&host.life,resumed==JNI_TRUE);
    publish_running_locked();pthread_mutex_unlock(&host.state_lock);
    if(!resumed){pthread_mutex_lock(&host.input_lock);host.buttons=0;host.ax=host.ay=128;pthread_mutex_unlock(&host.input_lock);}
}
JNIEXPORT void JNICALL JNI_METHOD(nativeSetSurface)(JNIEnv *env,jclass clazz,jobject surface){
    (void)clazz;ANativeWindow *window=surface?ANativeWindow_fromSurface(env,surface):NULL;
    /* Surface validity can change between Java's check and this acquisition. */
    if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
    pthread_mutex_lock(&host.state_lock);
    ANativeWindow *old=host.pending_window;host.pending_window=window;
    asphalt_android_lifecycle_window(&host.life,window!=NULL);publish_running_locked();
    pthread_mutex_unlock(&host.state_lock);
    /* This reference was never transferred to the render thread. Its active
     * reference is independent and only that thread may release it. */
    if(old)ANativeWindow_release(old);
    if(!window){pthread_mutex_lock(&host.input_lock);host.buttons=0;host.ax=host.ay=128;pthread_mutex_unlock(&host.input_lock);}
}
JNIEXPORT jlong JNICALL JNI_METHOD(nativePresentedFrames)(JNIEnv *env,jclass clazz){
    (void)env;(void)clazz;
    uint64_t frames=0;
    /* This getter only reads the renderer's locked presentation counter.
     * It never touches guest memory, CPU registers or the game scheduler. */
    psp_render_gl_stats(NULL,NULL,NULL,&frames,NULL);
    return (jlong)frames;
}
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeDisplayRestartRequested)(JNIEnv *env,jclass clazz){
    (void)env;(void)clazz;return asphalt_display_session_restart_requested()?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeDisplayRecovered)(JNIEnv *env,jclass clazz){
    (void)env;(void)clazz;return asphalt_display_session_recovered()?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT void JNICALL JNI_METHOD(nativeDisplayInsets)(JNIEnv *env,jclass clazz,jint l,jint t,jint r,jint b){
    (void)env;(void)clazz;if(l<0||t<0||r<0||b<0)return;
    pthread_mutex_lock(&host.input_lock);
    host.geometry[2]=l;host.geometry[3]=t;host.geometry[4]=r;host.geometry[5]=b;
    pthread_mutex_unlock(&host.input_lock);
}
JNIEXPORT jstring JNICALL JNI_METHOD(nativeStart)(JNIEnv *env, jclass clazz,
    jobject surface, jint iso_fd, jstring image_path, jstring runtime_root) {
    pthread_mutex_lock(&host.state_lock);
    if (host.claimed) {
        pthread_mutex_unlock(&host.state_lock);
        return (*env)->NewStringUTF(env, "This game process already owns a session; relaunch the game");
    }
    host.claimed = 1;
    pthread_mutex_unlock(&host.state_lock);
    const char *error = NULL;
    if (iso_fd < 0 || copy_string(env, image_path, host.image_path, sizeof host.image_path) ||
        copy_string(env, runtime_root, host.runtime_root, sizeof host.runtime_root)) {
        error = "Invalid Surface, ISO descriptor or native runtime paths"; goto fail;
    }
    host.iso_fd = fcntl(iso_fd, F_DUPFD_CLOEXEC, 3);
    if (host.iso_fd < 0) { error = "Cannot duplicate the selected ISO descriptor"; goto fail; }
    unsigned char signature;
    if (pread(host.iso_fd, &signature, 1, 0) != 1) {
        error = "The selected ISO does not provide a seekable readable descriptor"; goto fail;
    }
    if((*env)->GetJavaVM(env,&host.vm)!=JNI_OK){error="Cannot retain Java VM";goto fail;}
    host.activity_class=(*env)->NewGlobalRef(env,clazz);
    host.storage_before=(*env)->GetStaticMethodID(env,clazz,"storageBeforeWrite","()I");
    host.storage_after=(*env)->GetStaticMethodID(env,clazz,"storageAfterWrite","()I");
    if(!host.activity_class || !host.storage_before || !host.storage_after){error="Cannot connect persistent storage";goto fail;}
    if(surface)JNI_METHOD(nativeSetSurface)(env,clazz,surface);
    pthread_attr_t attr;
    pthread_t boot;
    int result = pthread_attr_init(&attr);
    if (result) { error = "Cannot initialize the native game thread"; goto fail; }
    result = pthread_attr_setstacksize(&attr, 16u * 1024u * 1024u);
    if (!result) result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!result) result = pthread_create(&boot, &attr, game_loop, NULL);
    pthread_attr_destroy(&attr);
    if (result) { error = "Cannot create the native game thread"; goto fail; }
    return NULL;
fail:
    if (host.iso_fd >= 0) { close(host.iso_fd); host.iso_fd = -1; }
    pthread_mutex_lock(&host.state_lock);
    if (host.pending_window) { ANativeWindow_release(host.pending_window); host.pending_window = NULL; }
    snprintf(host.status, sizeof host.status, "%s", error);
    host.finished=1;pthread_cond_broadcast(&host.state_changed);
    pthread_mutex_unlock(&host.state_lock);
    return (*env)->NewStringUTF(env, error);
}
static uint8_t axis_byte(float value) {
    if (!isfinite(value)) return 128;
    if (value <= -1.0f) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(128.0f + value * 127.0f);
}
JNIEXPORT void JNICALL JNI_METHOD(nativeInput)(JNIEnv *env, jclass clazz,
    jint buttons, jfloat ax, jfloat ay) {
    (void)env; (void)clazz;
    pthread_mutex_lock(&host.input_lock);
    int suspended=atomic_load_explicit(&host.suspended,memory_order_acquire);
    host.buttons = suspended?0:((uint32_t)buttons & 0xffffu);
    host.ax = suspended?128:axis_byte(ax); host.ay = suspended?128:axis_byte(ay);
    pthread_mutex_unlock(&host.input_lock);
}
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeStop)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    atomic_store_explicit(&host.stopping, 1, memory_order_release);
    pthread_mutex_lock(&host.state_lock);
    asphalt_android_lifecycle_stop(&host.life);publish_running_locked();
    struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=2;
    while(!host.finished){if(pthread_cond_timedwait(&host.state_changed,&host.state_lock,&deadline)==ETIMEDOUT)break;}
    int finished=host.finished;pthread_mutex_unlock(&host.state_lock);
    return finished?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeRaceControlsActive)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    return !atomic_load_explicit(&host.stopping,memory_order_acquire)&&asphalt_race_controls_active()?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jstring JNICALL JNI_METHOD(nativeStatus)(JNIEnv *env, jclass clazz) {
    (void)clazz;
    char status[ERROR_CAPACITY];
    pthread_mutex_lock(&host.state_lock);
    snprintf(status, sizeof status, "%s", host.status[0] ? host.status : host.device_error);
    pthread_mutex_unlock(&host.state_lock);
    return status[0] ? (*env)->NewStringUTF(env, status) : NULL;
}
