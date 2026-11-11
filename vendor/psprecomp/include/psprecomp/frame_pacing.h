/* Host presentation scheduling only. No guest memory, clocks or update code. */
#ifndef PSPRECOMP_FRAME_PACING_H
#define PSPRECOMP_FRAME_PACING_H
#include <stdint.h>
#include <string.h>
typedef struct {
 uint64_t next_frame,next_present,last;
 unsigned frame_remainder,present_remainder;
 int fps,started,work_fps;
} psp_frame_pacer;
static inline int psp_frame_pacer_init_work(psp_frame_pacer *p,int fps,int work_fps){
 if(!p||(fps!=30&&fps!=60)||(work_fps!=30&&work_fps!=60))return -1;
 memset(p,0,sizeof *p);p->fps=fps;p->work_fps=work_fps;return 0;
}
static inline int psp_frame_pacer_init(psp_frame_pacer *p,int fps){return psp_frame_pacer_init_work(p,fps,60);}
static inline uint64_t psp_frame_pacer_advance(uint64_t at,unsigned *remainder,unsigned fps){
 unsigned r=*remainder+1000000u%fps;at+=1000000u/fps;
 if(r>=fps){at++;r-=fps;}*remainder=r;return at;
}
static inline uint64_t psp_frame_pacer_begin(psp_frame_pacer *p,uint64_t now){
 if(!p->started||now<p->last){
  p->next_frame=p->next_present=now;p->frame_remainder=p->present_remainder=0;p->started=1;
 }
 p->last=now;
 /* Same native-work ceiling in BOTH modes. Late frames rebase; loading never
  * produces a burst of catch-up submissions or a changed simulation clock. */
 if(now>p->next_frame&&(p->work_fps==30||now-p->next_frame>1000000u/(unsigned)p->work_fps+1u)){p->next_frame=now;p->frame_remainder=0;}
 uint64_t ready=now>p->next_frame?now:p->next_frame;
 p->next_frame=psp_frame_pacer_advance(p->next_frame,&p->frame_remainder,(unsigned)p->work_fps);
 return ready;
}
static inline int psp_frame_pacer_present(psp_frame_pacer *p,uint64_t now){
 if(now<p->next_present)return 0;
 p->next_present=psp_frame_pacer_advance(p->next_present,&p->present_remainder,(unsigned)p->fps);
 if(p->next_present<=now){p->present_remainder=0;p->next_present=psp_frame_pacer_advance(now,&p->present_remainder,(unsigned)p->fps);}
 return 1;
}
#endif
