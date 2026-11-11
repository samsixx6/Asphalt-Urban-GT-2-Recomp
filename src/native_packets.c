#include "native_packets.h"
#include <math.h>
#include <string.h>
void asphalt_native_packets_reset(asphalt_native_packets *p){if(p)memset(p,0,sizeof *p);}
static int vector_valid(const float *v){for(unsigned k=0;k<3;k++)if(!isfinite(v[k])||v[k]<0)return 0;return 1;}
int asphalt_native_packets_begin(asphalt_native_packets *p,const float delta[3]){
 if(!p||(p->kind!=ASPHALT_PACKET_WAIT&&!p->executed))return -1;
 asphalt_native_packets next=*p;
 if(delta){
  if(!vector_valid(delta)||next.active)return -1;
  memcpy(next.raw,delta,12);
 }
 next.executed=0;next.stages++;
 if(next.active){if(!next.pending)return -1;next.kind=ASPHALT_PACKET_FINISH;}
 else if(delta)next.kind=ASPHALT_PACKET_START;
 else next.kind=ASPHALT_PACKET_WAIT;
 *p=next;return (int)p->kind;
}
static int send(asphalt_native_packets *p,asphalt_native_packet_emit emit,void *user,int kind,const float dt[3]){
 if(emit(user,kind,dt)){asphalt_native_packets_reset(p);return -1;}return 0;
}
int asphalt_native_packets_run(asphalt_native_packets *p,const float effective[3],float rate,int split,asphalt_native_packet_emit emit,void *user){
 if(!p)return -1;
 if(p->kind==ASPHALT_PACKET_WAIT||p->executed)return 0;
 if(!emit)return -1;
 unsigned count;
 if(p->kind==ASPHALT_PACKET_START){
  if(!effective||!vector_valid(effective)||!isfinite(rate)||rate<=0)return -1;
  float product=effective[0]*rate;if(!isfinite(product)||product>=64)return -1;
  count=(unsigned)product+1;
  memcpy(p->sim,effective,12);memcpy(p->step,effective,12);p->step[0]/=count;
  unsigned first=split?(count+1)/2:count;
  p->total=count;p->pending=count-first;p->active=!!p->pending;p->executed=1;p->packets++;
  if(send(p,emit,user,ASPHALT_PACKET_PRE,p->sim))return -1;
  count=first;
 }else if(p->kind==ASPHALT_PACKET_FINISH&&p->active&&p->pending){
  count=p->pending;p->pending=0;p->active=0;p->executed=1;
 }else return -1;
 for(unsigned i=0;i<count;i++){if(send(p,emit,user,ASPHALT_PACKET_STEP,p->step))return -1;p->steps++;}
 if(!p->pending&&send(p,emit,user,ASPHALT_PACKET_POST,p->sim))return -1;
 return 0;
}
