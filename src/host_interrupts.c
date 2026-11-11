#include "host_interrupts.h"
#include "psprecomp/hle.h"
#include <stdint.h>
#include <time.h>
/* Called only by the current CPU owner. Polling movie threads do not issue
 * waits, so trace checkpoints also provide a small cooperative timeslice.
 * Interrupt/callback frames and guest critical sections may not be parked. */
void asphalt_interrupt_checkpoint(void) {
    static uint64_t previous_ns;
    struct timespec now;
#ifdef __ANDROID__
    if (clock_gettime(CLOCK_MONOTONIC,&now)) return;
#else
    if (timespec_get(&now,TIME_UTC)!=TIME_UTC) return;
#endif
    uint64_t ns=(uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
    if (ns-previous_ns<1000000ull) return; /* coalesce checkpoints to at most 1 kHz */
    previous_ns=ns;
    psp_intr_tick();
    if(!psp_threadman_intr_disabled() && !psp_intr_active() && !psp_ge_callback_active())
        psp_threadman_yield();
}
