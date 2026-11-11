#ifndef ASPHALT_HOST_PERF_H
#define ASPHALT_HOST_PERF_H
#include <stdint.h>
void psp_render_profile_stats(uint64_t *draw_us,uint64_t *draw_calls);
void asphalt_perf_tick(uint64_t elapsed_ms,uint64_t capture_us);
#endif
