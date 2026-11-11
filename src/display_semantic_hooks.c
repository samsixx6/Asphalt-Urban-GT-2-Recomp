/* Observes the verified original UI producers; it never rewrites their data. */
#include "display_semantic_hooks.h"
#include "display_semantics.h"
#include "display_semantic_rules.h"
#include "native_preview.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include <limits.h>
#include <string.h>
#if ASPHALT_HUD_DIAGNOSTICS
#include <stdio.h>
static int diagnostic_layout,diagnostic_racing,diagnostic_ready;
static unsigned diagnostic_states,diagnostic_hits,diagnostic_misses,diagnostic_sources;
static uint32_t diagnostic_seen[128][6];
void asphalt_semantic_diagnostics_layout(int layout,int racing){
 if(diagnostic_layout!=layout||diagnostic_racing!=racing){
  if(diagnostic_states++<16)fprintf(stderr,"hud-state: layout=%d racing=%d\n",layout,racing);
  diagnostic_layout=layout;diagnostic_racing=racing;
 }
}
static int diagnostic_provider(uint32_t prim,uint32_t pos,unsigned bytes){
 int role=asphalt_semantics_role(prim,pos,bytes);
 /* Coordinates filter observations only. They never classify or move a draw. */
 uint32_t p=pos&0x1fffffffu;
 if(diagnostic_ready&&(role?diagnostic_hits<24:diagnostic_misses<72)&&diagnostic_layout==1&&diagnostic_racing&&bytes==6&&p>=0x08800000u&&p<=0x09fffffau&&psp_mem_ptr(p,6)){
  int x=(int16_t)psp_read16(p),y=(int16_t)psp_read16(p+2);
  if(x>=-6&&x<=130&&((y>=-4&&y<=32)||(y>=230&&y<=280))){
   unsigned *count=role?&diagnostic_hits:&diagnostic_misses;
   if(*count<(role?24u:72u)){(*count)++;asphalt_semantics_debug_query(prim,pos,bytes,role);}
  }
 }
 return role;
}
static void diagnostic_component(uint32_t component,int role){
 if(diagnostic_sources>=128)return;
 uint32_t leaf=component&0x1fffffffu,p=leaf,child=0;
 for(unsigned depth=0;depth<8&&p>=0x08800000u&&p<=0x09ffffbcu&&psp_mem_ptr(p,68);depth++){
  uint32_t type=psp_read32(p);
  if(type==0x08b3a844u||type==0x08b3a8f4u){
   diagnostic_ready=1;psp_ge_hud_diagnostic_arm(1);
   uint32_t slot=~0u,n=psp_read32(p+64),array=psp_read32(p+60)&0x1fffffffu;
   if(child&&n<=128&&array>=0x08800000u&&array<=0x0a000000u-n*4&&psp_mem_ptr(array,n*4))
    for(uint32_t i=0;i<n;i++)if((psp_read32(array+4*i)&0x1fffffffu)==child){slot=i;break;}
   uint32_t key[6]={type,slot,psp_read32(leaf),psp_read32(leaf+4),psp_read32(leaf+8),(uint32_t)role};
   for(unsigned i=0;i<diagnostic_sources;i++)if(!memcmp(diagnostic_seen[i],key,sizeof key))return;
   memcpy(diagnostic_seen[diagnostic_sources++],key,sizeof key);
   fprintf(stderr,"hud-source: component=%08X owner=%08X owner_type=%08X slot=%u leaf_type=%08X field4=%08X field8=%08X role=%d depth=%u\n",leaf,p,type,slot,key[2],key[3],key[4],role,depth);
   return;
  }
  child=p;p=psp_read32(p+16)&0x1fffffffu;
 }
}
#endif
static int enabled;
#if defined(_MSC_VER)
#define SEMANTIC_TLS __declspec(thread)
#else
#define SEMANTIC_TLS _Thread_local
#endif
static SEMANTIC_TLS int components[64];
static SEMANTIC_TLS unsigned component_depth;
static SEMANTIC_TLS uint64_t component_generation;
static void sync_components(void){
 uint64_t generation=asphalt_semantics_generation();
 if(component_generation==generation)return;
 component_depth=0;memset(components,0,sizeof components);component_generation=generation;
}
static int valid(uint32_t p,unsigned n){return p>=0x08800000u&&n&&n<=0x01800000u&&p<=0x0a000000u-n&&psp_mem_ptr(p,n)!=NULL;}
static int current_role(void){
 sync_components();
 if(component_depth>64)return PSP_SCREEN_PRESERVE;
 uint32_t frames[24];unsigned n=0;
 for(psp_frame *f=psp_cpu.frame;f&&n<24;f=f->prev)frames[n++]=f->ret;
 int command=asphalt_semantic_command_role(frames,n);
 if(command==PSP_SCREEN_FRONTEND_TEXT)return command;
 if(component_depth&&components[component_depth-1])return components[component_depth-1];
 return command;
}
void asphalt_semantic_hooks_begin_session(int on){
 enabled=on!=0;
 asphalt_semantics_begin_session(on);sync_components();
#if ASPHALT_HUD_DIAGNOSTICS
 diagnostic_states=diagnostic_hits=diagnostic_misses=diagnostic_sources=0;
 diagnostic_ready=0;psp_ge_hud_diagnostic_arm(0);
 memset(diagnostic_seen,0,sizeof diagnostic_seen);
 fprintf(stderr,"hud-diagnostic: build=0.30-motion-fixes enabled=%d generation=%llu\n",enabled,(unsigned long long)asphalt_semantics_generation());
 psp_ge_set_screen_role_provider(diagnostic_provider);
#else
 psp_ge_set_screen_role_provider(asphalt_semantics_role);
#endif
 asphalt_semantic_configure_bounds();
}
void asphalt_semantic_component_begin(uint32_t component){
 if(!enabled||asphalt_preview_active())return;
 sync_components();
 if(component_depth<64){
  components[component_depth]=asphalt_semantic_component_role(component);
#if ASPHALT_HUD_DIAGNOSTICS
  diagnostic_component(component,components[component_depth]);
#endif
 }
 if(component_depth<UINT_MAX)component_depth++;else enabled=0;
}
void asphalt_semantic_component_end(void){if(enabled&&!asphalt_preview_active()){sync_components();if(component_depth)component_depth--;}}
void asphalt_semantic_hook_enter(uint32_t pc,uint32_t object){
 if(enabled&&!asphalt_preview_active()&&pc==0x088aa264u)asphalt_semantics_batch_enter(object);
}
void asphalt_semantic_hook_leave(uint32_t pc,uint32_t object){
 if(!enabled||asphalt_preview_active())return;
 if(pc==0x088aa264u){asphalt_semantics_batch_leave(object);return;}
 if(pc!=0x088aa364u||psp_cpu.r[2]!=1||!valid(object,84))return;
 uint32_t consumed=psp_read32(object+72),queued=psp_read32(object+76),meta=psp_read32(object+80);
 if(queued<6||consumed>8192||queued>8192-consumed||!valid(meta,108))return;
 /* Original088A6D94 writes attribute0 through metadata+48, stride byte5.
  * Capture proves both the s16 atlas and float UV/position layout. Only the
  * typed event owner may use the latter here; projected frontend fonts have
  * stronger exact-command provenance at their dedicated flush. */
 int role=current_role();unsigned stride=psp_read8(meta+5),bytes=0;
 if(psp_read32(meta+88)!=0)return;
 if(psp_read32(meta)==0x303u&&psp_read32(meta+104)==0&&stride==16)bytes=6;
 else if(role==PSP_SCREEN_HUD_EVENT&&psp_read32(meta)==0x301u&&psp_read32(meta+92)==7&&psp_read32(meta+104)==7&&stride==24)bytes=12;
 if(!bytes)return;
 uint32_t position=psp_read32(meta+48);uint64_t first=(uint64_t)position+(consumed+queued-6)*stride;
 if(first>UINT32_MAX||!valid((uint32_t)first,5*stride+bytes))return;
 asphalt_semantics_record_vertices(object,(uint32_t)first,stride,6,bytes,role);
}
void asphalt_semantic_prim(uint32_t at,uint32_t word){
 if(enabled&&!asphalt_preview_active())asphalt_semantics_record_command(at,word,current_role());
}
