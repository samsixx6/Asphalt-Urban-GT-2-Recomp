#include "psprecomp/dispatch.h"
#include "psprecomp/cpu.h"
#include <assert.h>
#include <stdio.h>
static unsigned calls;
static void progress(void) { calls++; assert(psp_cpu.r[4]==0x12345678); }
int main(void) {
    psp_cpu_reset();psp_cpu.r[4]=0x12345678;
    psp_trace_set_progress_hook(2,progress);
    psp_trace_enter(0x08800000);assert(calls==0);
    psp_trace_loop(0x08800004);assert(calls==1);
    psp_trace_enter(0x08800008);psp_trace_enter(0x0880000C);assert(calls==2);
    psp_trace_set_progress_hook(0,NULL);
    for(int i=0;i<10;i++)psp_trace_loop(0x08800004);
    assert(calls==2 && psp_cpu.r[4]==0x12345678);
    puts("guest progress hook checks passed");return 0;
}
