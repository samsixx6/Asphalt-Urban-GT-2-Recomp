/* Original monitor artwork. A private, address-checked NOP marker places the
 * port's extra sprite in the same ordered GE stream as the original menu. */
#include "display_icon.h"
#include "display_game_menu.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
static uint32_t pixels[2],component,options_object;
typedef struct { uint32_t at;float x0,y0,x1,y1;int selected; } icon_command;
static icon_command commands[256];static unsigned next_command;
static void clear_binding(void){component=options_object=0;memset(commands,0,sizeof commands);next_command=0;}
void asphalt_display_icon_released(uint32_t address){if(asphalt_display_options_released(address))clear_binding();}
static int art(int x,int y){
 int dx=2*x-63,dy=2*y-63,r=dx*dx+dy*dy;
 if(r>61*61)return 0;
 if(r>55*55)return 2;
 if(x>=16&&x<=47&&y>=19&&y<=42&&(x<19||x>44||y<22||y>39))return 3;
 if((x>=29&&x<=34&&y>=42&&y<=47)||(x>=23&&x<=40&&y>=47&&y<=49))return 3;
 return 1;
}
static void execute(uint32_t address,uint32_t argument) {
 if((argument&0xffff00u)!=0xd15000u)return;
 icon_command *c=&commands[argument&255];if(c->at!=(address&0x1fffffffu))return;
 if(!component||asphalt_display_options_carousel(options_object)!=component){c->at=0;return;}
 psp_vertex v[2]={{0}};v[0].fx=c->x0;v[0].fy=c->y0;v[1].fx=c->x1;v[1].fy=c->y1;
 v[0].x=(int)c->x0;v[0].y=(int)c->y0;v[1].x=(int)c->x1;v[1].y=(int)c->y1;
 v[0].rgba=v[1].rgba=0xffffffff;v[0].fog=v[1].fog=1;v[1].u=v[1].v=64;
 psp_draw_state st={0};st.sc_x1=479;st.sc_y1=271;st.tex_enable=st.tex_alpha=1;
 st.tex_addr=pixels[c->selected];st.tex_fmt=3;st.tex_w=st.tex_h=st.tex_bw=64;st.tex_func=3;
 st.tex_clamp_u=st.tex_clamp_v=1;st.tex_min_linear=st.tex_mag_linear=1;
 st.blend=1;st.blend_src=2;st.blend_dst=3;
 psp_ge_adjust_screen_vertices_role(PSP_SCREEN_CENTER,6,v,2,&st,1,0,512);
 psp_render_current()->draw(6,v,2,&st);c->at=0;
}
int asphalt_display_install_icon(uint32_t object,uint32_t (*allocate)(unsigned),void (*release)(uint32_t)) {
 clear_binding();
 if(!allocate||!release)return -1;
 uint32_t owner=asphalt_display_options_carousel(object);if(!owner)return -1;
 uint32_t next[2]={0};
 if(!pixels[0]||!pixels[1])for(int mode=0;mode<2;mode++) {
  next[mode]=allocate(64*64*4);
  uint32_t *p=next[mode]?psp_mem_ptr(next[mode],64*64*4):NULL;
  if(!p){for(int i=0;i<2;i++)if(next[i])release(next[i]);return -1;}
  uint32_t pal[4]={0,mode?0xe0191610u:0xd0101010u,mode?0xff18b8fau:0xff858585u,mode?0xff62d7ffu:0xffb5b5b5u};
  for(int y=0;y<64;y++)for(int x=0;x<64;x++)p[y*64+x]=pal[art(x,y)];
 }
 if(next[0]){pixels[0]=next[0];pixels[1]=next[1];}
 component=owner;options_object=object;
 psp_ge_set_nop_handler(execute);return 0;
}
int asphalt_display_icon_enqueue(uint32_t owner,uint32_t offset,uint32_t quad) {
 if(!component||owner!=component||asphalt_display_options_carousel(options_object)!=component||
    offset!=ASPHALT_OPTIONS_RECOMP_INDEX*140||!psp_mem_ptr(quad,32))return 0;
 float q[8];memcpy(q,psp_mem_ptr(quad,32),32);
 uint32_t context=psp_read32(0x08B3DD50u);
 if(!psp_mem_ptr(context,12))return -1;
 uint32_t at=psp_read32(context+8);if(!psp_mem_ptr(at,4))return -1;
 unsigned slot=next_command++&255;if(commands[slot].at)return -1;
 icon_command c={at&0x1fffffffu,q[0],q[4],q[2],q[5],psp_read32(component+0x10c)==ASPHALT_OPTIONS_RECOMP_INDEX};
 if(!isfinite(c.x0)||!isfinite(c.x1)||!isfinite(c.y0)||!isfinite(c.y1)||c.x1<=c.x0||c.y1<=c.y0)return -1;
 static int logged;if(!logged++){fprintf(stderr,"display-icon: rect=%g,%g..%g,%g GE=%08X\n",c.x0,c.y0,c.x1,c.y1,at);}
 commands[slot]=c;psp_write32(at,0x00D15000u|slot);psp_write32(context+8,at+4);return 1;
}
