#include "auto_accelerate.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

static int automatic;
static int memory(uint32_t p,unsigned size){return p>=0x08800000u&&size<=0x1800000u&&p<=0x0a000000u-size&&psp_mem_ptr(p,size)!=0;}
void asphalt_auto_accelerate_configure(int enabled){automatic=enabled==1;}
void asphalt_auto_accelerate_query_complete(uint32_t input_map){
 /* Owned ULES00719 digital accelerator query in089011B4. Other callers,
  * including brake08901448 and all menu actions, keep their original result.
  * The original query has already updated its action-state bookkeeping. */
 if(!automatic||psp_cpu.r[31]!=0x08901300u)return;
 uint32_t controller=psp_cpu.r[16];
 if(!memory(controller,160)||psp_read32(controller)!=0x08b385c8u||
    psp_read32(controller+64)!=input_map||!memory(input_map,40)||psp_read32(input_map)!=0x08b3b65cu||
    !psp_read8(controller+48)||psp_read32(0x08b3eb14u)!=52)return;
 uint32_t state=psp_read32(controller+76),actor=psp_read32(controller+4),status=psp_read32(controller+60);
 if(!memory(state,20)||psp_read32(state)!=0x08b385b8u||!psp_read8(state+4)||psp_read32(controller+156)!=state||
    !memory(actor,1256)||psp_read32(actor+1248)!=controller||psp_read32(actor+1252)!=state||!memory(status,24))return;
 uint32_t mode=psp_read32(status+20);
 if(mode!=psp_read32(0x08b3ee9cu)&&mode!=psp_read32(0x08b3eea0u))return;
 /* No guest RAM/button latch, brake result, physics value or clock changes. */
 psp_cpu.r[2]=1;
}
