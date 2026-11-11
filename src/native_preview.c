#include "native_preview.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include <setjmp.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { RAM_BASE=0x08800000u,RAM_BYTES=0x01800000u,PAGE_BYTES=4096,PAGE_LIMIT=128,EVENT_LIMIT=100000 };
/* Only the emulated CPU owner can enter this scope. Guards themselves are
 * thread-local; no checkpoint or firmware call can transfer ownership in it. */
static struct {
 jmp_buf escape;
 psp_cpu_state cpu,call_cpu;
 float arguments[3];
 psp_mem_access_guard memory_guard;
 psp_hle_call_guard firmware_guard;
 psp_trace_guard_fn trace_guard;
 unsigned active,ok,used,events,last_pages;uint32_t reason,target;
 uint16_t page_slot[RAM_BYTES/PAGE_BYTES];
 uint32_t page_address[PAGE_LIMIT];
 unsigned char pages[PAGE_LIMIT][PAGE_BYTES];
} scope;

int asphalt_preview_active(void){return scope.active!=0;}
unsigned asphalt_preview_last_pages(void){return scope.last_pages;}
void asphalt_preview_forbid(void){if(scope.active)longjmp(scope.escape,1);}
static void guard_firmware(uint32_t nid){scope.reason=1;scope.target=nid;asphalt_preview_forbid();}
static void guard_event(uint32_t target){if((target&&!psp_lookup(target))||(!target&&++scope.events>EVENT_LIMIT)){scope.reason=2;scope.target=target;asphalt_preview_forbid();}}
static void guard_memory(uint32_t address,uint32_t size,int write){
 uint32_t at=address&PSP_ADDR_MASK;
 if(size>RAM_BYTES||at<RAM_BASE||at>RAM_BASE+RAM_BYTES-size){scope.reason=3;scope.target=address;asphalt_preview_forbid();}
 if(!size||!write)return;
 unsigned first=(at-RAM_BASE)/PAGE_BYTES,last=(at-RAM_BASE+size-1)/PAGE_BYTES;
 for(unsigned page=first;page<=last;page++){
  if(scope.page_slot[page])continue;
  if(scope.used==PAGE_LIMIT){scope.reason=4;asphalt_preview_forbid();}
  unsigned slot=scope.used++;uint32_t base=RAM_BASE+page*PAGE_BYTES;
  scope.page_slot[page]=(uint16_t)(slot+1);scope.page_address[slot]=base;
  memcpy(scope.pages[slot],psp_mem_ptr(base,PAGE_BYTES),PAGE_BYTES);
 }
}

int asphalt_preview_call_prepared(uint32_t pc,uint32_t object,uint32_t mode,const float delta[3],asphalt_preview_capture prepare,asphalt_preview_capture capture,void *user){
 if(scope.active||!delta||!capture||!psp_lookup(pc))return 0;
 for(unsigned i=0;i<3;i++)if(!isfinite(delta[i])||delta[i]<0)return 0;
 uint32_t stack=psp_cpu.r[29]-48;
 if(stack<RAM_BASE||stack>RAM_BASE+RAM_BYTES-48||!psp_mem_ptr(stack,48))return 0;
 scope.cpu=psp_cpu;memcpy(scope.arguments,delta,sizeof scope.arguments);scope.used=scope.events=scope.ok=scope.reason=scope.target=0;memset(scope.page_slot,0,sizeof scope.page_slot);
 /* No automatic local is relied upon after a longjmp. Scope state and saved
  * guards live in this CPU-owned static record. */
 if(setjmp(scope.escape)==0){
  scope.memory_guard=psp_mem_set_access_guard(guard_memory);
  scope.firmware_guard=psp_hle_set_call_guard(guard_firmware);
  scope.trace_guard=psp_trace_set_event_guard(guard_event);scope.active=1;
  for(unsigned i=0;i<3;i++)psp_write_f32(stack+32+4*i,scope.arguments[i]);
  psp_cpu.r[29]=stack;psp_cpu.r[4]=object;psp_cpu.r[5]=stack+32;psp_cpu.r[6]=mode;
  scope.call_cpu=psp_cpu;
  if(!prepare||prepare(user)){
   psp_cpu=scope.call_cpu;
   for(unsigned i=0;i<3;i++)psp_write_f32(stack+32+4*i,scope.arguments[i]);
   psp_call_from_host(pc);
   if(psp_cpu.r[29]==stack&&capture(user))scope.ok=1;
  }
 }
 scope.active=0;
 psp_mem_set_access_guard(scope.memory_guard);psp_hle_set_call_guard(scope.firmware_guard);psp_trace_set_event_guard(scope.trace_guard);
 for(unsigned i=0;i<scope.used;i++)memcpy(psp_mem_ptr(scope.page_address[i],PAGE_BYTES),scope.pages[i],PAGE_BYTES);
 scope.last_pages=scope.used;psp_cpu=scope.cpu;
 if(!scope.ok&&getenv("ASPHALT_NATIVE_TRACE")){static unsigned printed;if(printed++<8)fprintf(stderr,"camera-isolation-refused: reason=%u target=%08X pages=%u events=%u\n",scope.reason,scope.target,scope.used,scope.events);}
 return scope.ok!=0;
}
int asphalt_preview_call(uint32_t pc,uint32_t object,uint32_t mode,const float delta[3],asphalt_preview_capture capture,void *user){
 return asphalt_preview_call_prepared(pc,object,mode,delta,NULL,capture,user);
}
