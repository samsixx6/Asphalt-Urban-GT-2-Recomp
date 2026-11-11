#include "startup_logos.h"
#include "psprecomp/mem.h"
#include <math.h>
#include <string.h>

/* Owned ULES00719 original08AD17F8 resolves these named children, and
 * 08AD1CB8 displays them over its local0..8s timer at+340. They belong to
 * this frontend controller, not to the shared movie/input/clock services. */
typedef struct {
 uint32_t object,list,count,children[4],names[4];
} logo_owner;
static logo_owner bound;
static enum { UNSEEN,BOUND,CLOSED } lifetime;
static int cross_down;

static int memory(uint32_t p,unsigned size){
 return p>=0x08800000u&&size&&size<=0x01800000u&&p<=0x0a000000u-size&&psp_mem_ptr(p,size)!=NULL;
}
static int inspect(uint32_t object,logo_owner *out){
 static const unsigned offsets[]={316,304,308,312};
 static const char *const names[]={"stwUbiSoft","stwGameloft","stwGameSource","stwVirtuos"};
 if((object&3)||!memory(object,0x160)||psp_read32(object)!=0x08b3dae8u)return 0;
 memset(out,0,sizeof *out);out->object=object;
 out->count=psp_read32(object+52);out->list=psp_read32(object+88);
 if(out->count<4||out->count>64||(out->list&3)||!memory(out->list,out->count*4))return 0;
 for(unsigned i=0;i<4;i++){
  uint32_t child=psp_read32(object+offsets[i]);out->children[i]=child;
  if((child&3)||!memory(child,0x150)||psp_read32(child)!=0x08b38da8u||psp_read32(child+36)!=object)return 0;
  for(unsigned j=0;j<i;j++)if(child==out->children[j])return 0;
  uint32_t name=psp_read32(child+56);out->names[i]=name;
  unsigned bytes=(unsigned)strlen(names[i])+1;
  if(!memory(name,bytes)||memcmp(psp_mem_ptr(name,bytes),names[i],bytes))return 0;
  unsigned matches=0;
  for(unsigned j=0;j<out->count;j++)if(psp_read32(out->list+4*j)==child)matches++;
  if(matches!=1)return 0;
 }
 return 1;
}
void asphalt_startup_logos_reset(void){
 memset(&bound,0,sizeof bound);lifetime=UNSEEN;cross_down=0;
}
void asphalt_startup_logos_tick(uint32_t object,uint32_t buttons){
 if(lifetime==CLOSED||(lifetime==BOUND&&object!=bound.object))return;
 logo_owner current;
 if(!inspect(object,&current)){
  if(lifetime==BOUND)lifetime=CLOSED;
  return;
 }
 if(lifetime==BOUND&&memcmp(&bound,&current,sizeof bound)){lifetime=CLOSED;return;}
 float elapsed=psp_read_f32(object+340);int down=(buttons&0x4000u)!=0;
 if(lifetime==UNSEEN){
  bound=current;lifetime=BOUND;cross_down=down;
  /* A Profile/autosave confirmation already held at the first title tick
   * must be released before it can skip anything. Later title instances can
   * never acquire this one-shot, even after the same address is reused. */
  if(isfinite(elapsed)&&elapsed>=8)lifetime=CLOSED;
  return;
 }
 int pressed=down&&!cross_down;cross_down=down;
 if(!isfinite(elapsed)||elapsed<0)return;
 if(elapsed>=8){lifetime=CLOSED;return;}
 if(pressed){
  float next=elapsed<2?2:elapsed<4?4:elapsed<6?6:8;
  psp_write_f32(object+340,next);
  if(next==8)lifetime=CLOSED;
 }
 /* The wrapper still executes the complete original update with its exact
  * delta/registers. At8s the existing copyright initializer/cleanup runs. */
}
void asphalt_startup_logos_released(uint32_t address){
 if(lifetime!=BOUND)return;
 address&=0x1fffffffu;
 if(address==bound.object||address==bound.list){lifetime=CLOSED;return;}
 for(unsigned i=0;i<4;i++)if(address==bound.children[i]||address==bound.names[i]){lifetime=CLOSED;return;}
}
