#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_perf.h"
#include "psprecomp/hle.h"
#include "psprecomp/render.h"
static uint64_t filetime(FILETIME f) {
    return (uint64_t)f.dwLowDateTime|((uint64_t)f.dwHighDateTime<<32);
}
void asphalt_perf_tick(uint64_t elapsed,uint64_t capture_us) {
    static int enabled=-1;
    static int cpu_baselined;
    static uint64_t previous_ms,previous_flips,previous_draw_us,previous_draw_calls,previous_cpu,previous_capture;
    static uint64_t previous_gpu_presented,previous_gpu_batches;
    if(enabled<0){const char *e=getenv("ASPHALT_PERF");enabled=e && !strcmp(e,"1");}
    if(!enabled)return;
    FILETIME created,exited,kernel,user;
    if(!cpu_baselined) {
        if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return;
        previous_cpu=filetime(kernel)+filetime(user);cpu_baselined=1;
    }
    if(elapsed-previous_ms<5000)return;
    uint64_t flips,gap_us,draw_us,draw_calls;
    psp_display_frame_stats(&flips,&gap_us);psp_render_profile_stats(&draw_us,&draw_calls);
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return;
    uint64_t cpu=filetime(kernel)+filetime(user),window=elapsed-previous_ms;
    uint64_t gpu_presented=0,gpu_batches=0;psp_render_gl_stats(NULL,&gpu_batches,NULL,&gpu_presented,NULL);
    fprintf(stderr,"perf: {\"elapsed_ms\":%llu,\"window_ms\":%llu,\"display_fps\":%.3f,"
        "\"max_frame_ms\":%.3f,\"software_draw_ms\":%.3f,\"software_draw_calls\":%llu,"
        "\"cpu_core_equivalents\":%.3f,\"capture_ms\":%.3f,\"gpu_present_fps\":%.3f,\"gpu_batches\":%llu}\n",
        (unsigned long long)elapsed,(unsigned long long)window,(flips-previous_flips)*1000.0/window,
        gap_us/1000.0,(draw_us-previous_draw_us)/1000.0,(unsigned long long)(draw_calls-previous_draw_calls),
        (cpu-previous_cpu)/(10000.0*window),(capture_us-previous_capture)/1000.0,
        (gpu_presented-previous_gpu_presented)*1000.0/window,(unsigned long long)(gpu_batches-previous_gpu_batches));
    previous_ms=elapsed;previous_flips=flips;previous_draw_us=draw_us;previous_draw_calls=draw_calls;
    previous_cpu=cpu;previous_capture=capture_us;
    previous_gpu_presented=gpu_presented;previous_gpu_batches=gpu_batches;
}
