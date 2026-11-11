#include "display_game_menu.h"
#include "psprecomp/mem.h"
#include <string.h>
static struct {uint32_t object,carousel,widget,icons,captions,keys,label,key;} owned;
static int owns_options(uint32_t object) {
 if(!object||object!=owned.object||!psp_mem_ptr(object,0x350)||psp_read32(object)!=0x08B3D41Cu)return 0;
 uint32_t c=psp_read32(object+0x214),w=psp_read32(object+0x218);
 if(c!=owned.carousel||w!=owned.widget||!psp_mem_ptr(c,0x150)||!psp_mem_ptr(w,0x158))return 0;
 if(psp_read32(c+0x104)!=10||psp_read32(c+0x108)!=10||psp_read32(w+0x144)!=10||
    psp_read32(c+0xb8)!=owned.icons||psp_read32(w+0x150)!=owned.captions||psp_read32(w+0x154)!=owned.keys)return 0;
 if(!psp_mem_ptr(owned.icons-16,1416)||!psp_mem_ptr(owned.captions,160)||!psp_mem_ptr(owned.keys,40)||
    !psp_mem_ptr(owned.label,16)||!psp_mem_ptr(owned.key,13))return 0;
 return psp_read32(owned.icons-16)==140&&psp_read32(owned.icons-12)==10&&
  psp_read32(owned.captions+ASPHALT_OPTIONS_RECOMP_INDEX*16+4)==owned.label&&
  psp_read32(owned.keys+ASPHALT_OPTIONS_RECOMP_INDEX*4)==owned.key&&
  !memcmp(psp_mem_ptr(owned.label,16),"RECOMP SETTINGS",16)&&
  !memcmp(psp_mem_ptr(owned.key,13),"PORT_Display",13);
}
uint32_t asphalt_display_options_carousel(uint32_t object) {
 return owns_options(object)?owned.carousel:0;
}
int asphalt_display_options_released(uint32_t address) {
 if(!address||!owned.object)return 0;
 uint32_t allocations[]={owned.object,owned.carousel,owned.widget,owned.icons-16,owned.icons,
  owned.captions,owned.keys,owned.label,owned.key};
 for(unsigned i=0;i<sizeof allocations/sizeof allocations[0];i++)
  if((address&0x1fffffffu)==(allocations[i]&0x1fffffffu)){
   memset(&owned,0,sizeof owned);return 1;
  }
 return 0;
}
int asphalt_display_options_is_recomp(uint32_t object) {
 return owns_options(object)&&psp_read32(object+0x2a8)==0&&
  psp_read32(owned.widget+0x148)==ASPHALT_OPTIONS_RECOMP_INDEX;
}
uint32_t asphalt_display_options_original_id(uint32_t object,uint32_t selected) {
 if(!owns_options(object)||psp_read32(object+0x2a8)!=0||selected>=10)return selected;
 /* Only the original activation dispatch sees these IDs. The widgets retain
  * their visual indices throughout callbacks, navigation and destruction. */
 return selected==ASPHALT_OPTIONS_RECOMP_INDEX?9:selected>ASPHALT_OPTIONS_RECOMP_INDEX?selected-1:selected;
}
int asphalt_display_extend_options(uint32_t object,uint32_t (*allocate)(unsigned),void (*release)(uint32_t)) {
 if(!allocate||!release||!psp_mem_ptr(object,0x350)||psp_read32(object)!=0x08B3D41Cu)return -1;
 uint32_t c=psp_read32(object+0x214),w=psp_read32(object+0x218);
 if(!psp_mem_ptr(c,0x150)||!psp_mem_ptr(w,0x158))return -1;
 unsigned n=psp_read32(c+0x104);
 if(n==10 && psp_read32(w+0x144)==10)return owns_options(object)?1:-1;
 if(n!=9||psp_read32(c+0x108)!=9||psp_read32(w+0x144)!=9)return -1;
 uint32_t old[3]={psp_read32(c+0xb8),psp_read32(w+0x150),psp_read32(w+0x154)};
 unsigned sizes[5]={1416,160,40,16,16};uint32_t added[5]={0};
 if(!psp_mem_ptr(old[0]-16,1276)||psp_read32(old[0]-16)!=140||psp_read32(old[0]-12)!=9||!psp_mem_ptr(old[1],144)||!psp_mem_ptr(old[2],36))return -1;
 for(int i=0;i<5;i++)if(!(added[i]=allocate(sizes[i]))||!psp_mem_ptr(added[i],sizes[i])) {
  for(int j=0;j<=i;j++)if(added[j])release(added[j]);return -1;
 }
 /* This game's C++ array ABI places a16-byte cookie before icon records.
  * Raw free must receive the allocation base; destructors expect that cookie. */
 memset(psp_mem_ptr(added[0],16),0,16);psp_write32(added[0],140);psp_write32(added[0]+4,10);
 uint32_t icon_data=added[0]+16;
 /* Back stays at zero; index one is immediately above it in the original
  * vertical layout. Move every original option with its caption and key. */
 memcpy(psp_mem_ptr(icon_data,140),psp_mem_ptr(old[0],140),140);
 memcpy(psp_mem_ptr(icon_data+140,140),psp_mem_ptr(old[0],140),140);
 memcpy(psp_mem_ptr(icon_data+280,1120),psp_mem_ptr(old[0]+140,1120),1120);
 memcpy(psp_mem_ptr(added[1],16),psp_mem_ptr(old[1],16),16);
 memcpy(psp_mem_ptr(added[1]+16,16),psp_mem_ptr(old[1],16),16);
 memcpy(psp_mem_ptr(added[1]+32,128),psp_mem_ptr(old[1]+16,128),128);
 memcpy(psp_mem_ptr(added[2],4),psp_mem_ptr(old[2],4),4);
 memcpy(psp_mem_ptr(added[2]+8,32),psp_mem_ptr(old[2]+4,32),32);
 psp_mem_write_block(added[3],"RECOMP SETTINGS",16);psp_mem_write_block(added[4],"PORT_Display",13);
 psp_write32(added[1]+ASPHALT_OPTIONS_RECOMP_INDEX*16+4,added[3]);psp_write32(added[2]+ASPHALT_OPTIONS_RECOMP_INDEX*4,added[4]);
 psp_write32(c+0xb8,icon_data);psp_write32(w+0x150,added[1]);psp_write32(w+0x154,added[2]);
 psp_write32(c+0x104,10);psp_write32(c+0x108,10);psp_write32(w+0x144,10);
 owned.object=object;owned.carousel=c;owned.widget=w;owned.icons=icon_data;
 owned.captions=added[1];owned.keys=added[2];owned.label=added[3];owned.key=added[4];
 release(old[0]-16);release(old[1]);release(old[2]);
 return 0;
}
