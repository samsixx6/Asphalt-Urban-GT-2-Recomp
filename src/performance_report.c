#include "performance_report.h"
#include <string.h>

int asphalt_perf_delta(const asphalt_perf_point *a,const asphalt_perf_point *b,asphalt_perf_window *out) {
    if(!a||!b||!out)return -1;
    if(b->wall_ms<a->wall_ms||b->frames<a->frames||b->draws<a->draws||b->batches<a->batches||
       b->upload_bytes<a->upload_bytes||b->readbacks<a->readbacks||b->swap_ns<a->swap_ns)return -1;
    uint64_t elapsed=b->wall_ms-a->wall_ms;
    if(!elapsed)return 0;
    asphalt_perf_window w={0};w.wall_ms=elapsed;w.frames=b->frames-a->frames;
    w.draws=b->draws-a->draws;w.batches=b->batches-a->batches;
    w.upload_bytes=b->upload_bytes-a->upload_bytes;w.readbacks=b->readbacks-a->readbacks;
    w.swap_ns=b->swap_ns-a->swap_ns;
    w.fps=w.frames*1000.0/elapsed;
    w.cpu_valid=a->cpu_valid&&b->cpu_valid&&b->cpu_ns>=a->cpu_ns;
    if(w.cpu_valid)w.cpu_cores=(b->cpu_ns-a->cpu_ns)/(elapsed*1000000.0);
    *out=w;return 1;
}
void asphalt_perf_report_close(asphalt_perf_report *r) {
    if(r&&r->stream){fclose(r->stream);r->stream=NULL;}
}
static int append(asphalt_perf_report *r,const char *text,size_t n) {
    if(!r->stream)return 0;
    if(n>ASPHALT_PERF_MAX_BYTES-r->bytes){asphalt_perf_report_close(r);return 0;}
    if(fwrite(text,1,n,r->stream)!=n||fflush(r->stream)){asphalt_perf_report_close(r);return -1;}
    r->bytes+=(unsigned)n;return 1;
}
int asphalt_perf_report_open(asphalt_perf_report *r,const char *path,const asphalt_perf_point *baseline) {
    if(!r||!path||!baseline)return -1;
    asphalt_perf_report_close(r);memset(r,0,sizeof *r);r->previous=*baseline;
    r->stream=fopen(path,"wb");if(!r->stream)return -1;
    const char header[]="{\"schema\":1,\"kind\":\"session\",\"cpu_metric\":\"process_core_equivalents\",\"gpu_timing_available\":false}\n";
    return append(r,header,sizeof header-1)<0?-1:0;
}
int asphalt_perf_report_sample(asphalt_perf_report *r,const asphalt_perf_point *p,int scale,int fps,int aspect) {
    if(!r||!p)return -1;
    asphalt_perf_window w;int result=asphalt_perf_delta(&r->previous,p,&w);
    if(result<0){r->previous=*p;return 0;}if(!result)return 0;
    r->previous=*p;
    char line[640];int n=snprintf(line,sizeof line,
        "{\"kind\":\"sample\",\"wall_ms\":%llu,\"window_ms\":%llu,\"fps\":%.3f,\"frames\":%llu,"
        "\"cpu_valid\":%s,\"cpu_cores\":%.3f,\"draws\":%llu,\"batches\":%llu,\"upload_bytes\":%llu,"
        "\"readbacks\":%llu,\"egl_swap_ms\":%.3f,\"scale\":%d,\"target_fps\":%d,\"aspect\":%d}\n",
        (unsigned long long)p->wall_ms,(unsigned long long)w.wall_ms,w.fps,(unsigned long long)w.frames,
        w.cpu_valid?"true":"false",w.cpu_cores,(unsigned long long)w.draws,(unsigned long long)w.batches,
        (unsigned long long)w.upload_bytes,(unsigned long long)w.readbacks,w.swap_ns/1000000.0,scale,fps,aspect);
    if(n<0||(size_t)n>=sizeof line)return -1;
    return append(r,line,(size_t)n);
}
