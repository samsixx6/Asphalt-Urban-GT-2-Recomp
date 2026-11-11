#ifndef ASPHALT_PERFORMANCE_REPORT_H
#define ASPHALT_PERFORMANCE_REPORT_H
#include <stdint.h>
#include <stdio.h>
enum { ASPHALT_PERF_MAX_BYTES = 256*1024 };
typedef struct {
    uint64_t wall_ms,cpu_ns,frames,draws,batches,upload_bytes,readbacks,swap_ns;
    int cpu_valid;
} asphalt_perf_point;
typedef struct {
    uint64_t wall_ms,frames,draws,batches,upload_bytes,readbacks,swap_ns;
    double fps,cpu_cores;
    int cpu_valid;
} asphalt_perf_window;
typedef struct {
    FILE *stream;
    unsigned bytes;
    asphalt_perf_point previous;
} asphalt_perf_report;
int asphalt_perf_delta(const asphalt_perf_point *before,const asphalt_perf_point *after,asphalt_perf_window *out);
int asphalt_perf_report_open(asphalt_perf_report *report,const char *path,const asphalt_perf_point *baseline);
int asphalt_perf_report_sample(asphalt_perf_report *report,const asphalt_perf_point *point,int scale,int fps,int aspect);
void asphalt_perf_report_close(asphalt_perf_report *report);
#endif
