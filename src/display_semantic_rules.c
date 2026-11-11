/* Original UI identities, observed in the original callback graph. Positions
 * and texture coordinates never decide whether a draw is HUD or an effect. */
#include "display_semantic_rules.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include <stddef.h>
static uint32_t physical(uint32_t p){return p&0x1fffffffu;}
static int valid(uint32_t p,unsigned n){p=physical(p);return p>=0x08800000u&&n&&p<=0x0a000000u-n&&psp_mem_ptr(p,n)!=NULL;}
static int child_slot(uint32_t parent,uint32_t child){
 uint32_t n=psp_read32(parent+64),a=psp_read32(parent+60);
 if(n>128||!n||!valid(a,n*4))return -1;
 int slot=-1;
 for(uint32_t i=0;i<n;i++)if(physical(psp_read32(a+4*i))==child){if(slot>=0)return -1;slot=(int)i;}
 return slot;
}
/* Original08899E68/088990F4 leave object+4/+8 untouched. They are not
 * stable artwork identities: real phone races contain different/zero bytes.
 * Exact typed owner, unique direct membership and leaf type define the slot.
 * Positions and texture coordinates never grant a role. */
static const unsigned wanted[]={0,1,2,3,4,5};
static const unsigned wanted_background[]={1};
static const unsigned nitro[]={0,2,6,7};
static const unsigned nitro_background[]={0};
/* Original089EF0F8..089EF2E8 moves/enables this independent atlas leaf. */
static const unsigned nitro_growth[]={32};
/* Original SpriteSet0 frames: NOS label30 and multiplier15/20/14. */
static const unsigned nitro_label[]={14};
static const unsigned nitro_multiplier[]={15,16,17};
static const unsigned digital_background[]={9};
static const unsigned digital_text[]={34,35,36,37,38,39};
static int mixed_role(uint32_t owner,uint32_t child,const unsigned *slots,unsigned count,int alternate,int primary,uint32_t leaf_type){
 if(!child)return primary;
 int slot=child_slot(owner,child);if(slot<0)return PSP_SCREEN_PRESERVE;
 for(unsigned i=0;i<count;i++)if((unsigned)slot==slots[i]){
  if(psp_read32(child)!=leaf_type)return PSP_SCREEN_PRESERVE;
  return alternate;
 }
 return primary;
}
int asphalt_semantic_component_role(uint32_t component){
 uint32_t p=physical(component),child=0,seen[8];unsigned depth=0;
 while(depth<8&&valid(p,68)){
  for(unsigned i=0;i<depth;i++)if(seen[i]==p)return PSP_SCREEN_PRESERVE;
  seen[depth++]=p;
  switch(psp_read32(p)){
  case 0x08b3a844:{
   int role=mixed_role(p,child,wanted_background,sizeof wanted_background/sizeof *wanted_background,PSP_SCREEN_HUD_WANTED_BACKGROUND,PSP_SCREEN_HUD_MONEY,0x08b374e4u);
   if(role!=PSP_SCREEN_HUD_MONEY)return role;
   return mixed_role(p,child,wanted,sizeof wanted/sizeof *wanted,PSP_SCREEN_HUD_WANTED,PSP_SCREEN_HUD_MONEY,0x08b374e4u);
  }
  case 0x08b3a8f4:{
   int role=mixed_role(p,child,nitro_background,sizeof nitro_background/sizeof *nitro_background,PSP_SCREEN_HUD_NITRO_BACKGROUND,PSP_SCREEN_HUD_SPEED,0x08b374e4u);
   if(role!=PSP_SCREEN_HUD_SPEED)return role;
   role=mixed_role(p,child,nitro_growth,sizeof nitro_growth/sizeof *nitro_growth,PSP_SCREEN_HUD_NITRO_GROWTH,PSP_SCREEN_HUD_SPEED,0x08b3786cu);
   if(role!=PSP_SCREEN_HUD_SPEED)return role;
   role=mixed_role(p,child,nitro,sizeof nitro/sizeof *nitro,PSP_SCREEN_HUD_NITRO,PSP_SCREEN_HUD_SPEED,0x08b374e4u);
   if(role!=PSP_SCREEN_HUD_SPEED)return role;
   role=mixed_role(p,child,nitro_label,sizeof nitro_label/sizeof *nitro_label,PSP_SCREEN_HUD_NITRO_LABEL,PSP_SCREEN_HUD_SPEED,0x08b3786cu);
   if(role!=PSP_SCREEN_HUD_SPEED)return role;
   role=mixed_role(p,child,nitro_multiplier,sizeof nitro_multiplier/sizeof *nitro_multiplier,PSP_SCREEN_HUD_NITRO_MULTIPLIER,PSP_SCREEN_HUD_SPEED,0x08b3786cu);
   if(role!=PSP_SCREEN_HUD_SPEED)return role;
   role=mixed_role(p,child,digital_background,sizeof digital_background/sizeof *digital_background,PSP_SCREEN_HUD_SPEED_DIGITAL,PSP_SCREEN_HUD_SPEED,0x08b374e4u);
   if(role!=PSP_SCREEN_HUD_SPEED)return role;
   return mixed_role(p,child,digital_text,sizeof digital_text/sizeof *digital_text,PSP_SCREEN_HUD_SPEED_DIGITAL,PSP_SCREEN_HUD_SPEED,0x08b378c0u);
  }
  case 0x08b3a89c:return PSP_SCREEN_HUD_POSITION;
  case 0x08b3abbc:return PSP_SCREEN_HUD_MINIMAP;
  case 0x08b3a9a4:return PSP_SCREEN_HUD_LAP;
  case 0x08b3addc:return PSP_SCREEN_HUD_EVENT; /* typed race event messages */
  case 0x08b376bc:return PSP_SCREEN_CENTER; /* original pause/countdown owner */
  case 0x08b3a94c: /* full-width cinematic bars */
  case 0x08b3ce44: /* full-scene overlay with mixed transient content */
   return PSP_SCREEN_PRESERVE;
  default:break;
  }
  child=p;p=physical(psp_read32(p+16));
 }
 return PSP_SCREEN_PRESERVE;
}
int asphalt_semantic_command_role(const uint32_t *returns,unsigned count){
 if(!returns)return PSP_SCREEN_PRESERVE;
 int frontend=0,font=0;
 for(unsigned i=0;i<count&&i<64;i++){
  uint32_t p=physical(returns[i]);
  if(p==0x08940c20||p==0x08940c50)frontend=1;
  if(p==0x088bda78)font=1; /* original text-widget's atlas flush */
 }
 if(frontend&&font)return PSP_SCREEN_FRONTEND_TEXT;
 for(unsigned i=0;i<count&&i<64;i++)switch(physical(returns[i])){
  case 0x08940c20:case 0x08940c50: /* original menu renderer */
  case 0x088d3c40: /* original confirmation dialog, background and text */
   return PSP_SCREEN_CENTER;
  default:break;
 }
 /* 088A2700 is a shared atlas flush, and 088A2434 also draws the minimap.
  * Neither proves the semantic role of any vertex in those commands. */
 return PSP_SCREEN_PRESERVE;
}
void asphalt_semantic_configure_bounds(void){
 /* Use stable PSP author-space container/clip boundaries. Dynamic rank,
  * currency and lap glyph extents must never move their entire group. */
 for(int role=PSP_SCREEN_HUD_WANTED;role<=PSP_SCREEN_HUD_SPEED;role++)psp_ge_set_screen_role_bounds(role,0,480);
 psp_ge_set_screen_role_bounds(PSP_SCREEN_HUD_EVENT,0,480);
}
