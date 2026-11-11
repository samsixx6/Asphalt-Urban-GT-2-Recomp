#include "display_ui.h"
#include "psprecomp/render.h"
#include "psprecomp/hle.h"
#include <string.h>
#define WHITE 0xffeeeeeEu
#define GOLD 0xff48bcffu
#define GRAY 0xff939393u
static psp_draw_state paint;
static void rect_role(int role,float x,float y,float w,float h,uint32_t color){
 psp_vertex v[2]={{0}};v[0].fx=x;v[0].fy=y;v[1].fx=x+w;v[1].fy=y+h;
 v[0].x=(int)x;v[0].y=(int)y;v[1].x=(int)(x+w);v[1].y=(int)(y+h);v[0].rgba=v[1].rgba=color;v[0].fog=v[1].fog=1;
 psp_draw_state s=paint;psp_ge_adjust_screen_vertices_role(role,6,v,2,&s,1,0,512);psp_render_current()->draw(6,v,2,&s);
}
static void rect(float x,float y,float w,float h,uint32_t color){rect_role(PSP_SCREEN_CENTER,x,y,w,h,color);}
/* Each row is five bits. Original bitmap glyph definitions, no font asset. */
static const unsigned char glyphs[][7]={
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
 {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14},
 {0,0,0,31,0,0,0},{4,4,4,4,4,0,4},{2,4,8,16,8,4,2},{8,4,2,1,2,4,8}
};
static void text(int x,int y,const char *s,int scale,uint32_t color){
 for(;*s;s++,x+=6*scale){int i=*s>='A'&&*s<='Z'?*s-'A':*s>='0'&&*s<='9'?26+*s-'0':*s=='-'?36:*s=='!'?37:*s=='<'?38:*s=='>'?39:-1;
  if(i<0)continue;
  for(int row=0;row<7;row++)for(int col=0;col<5;col++)if(glyphs[i][row]&(16>>col))rect((float)(x+col*scale),(float)(y+row*scale),(float)scale,(float)scale,color);
 }
}
static void icon(int cx,int cy,int kind,int selected){
 uint32_t c=selected?GOLD:GRAY;
 for(int y=-12;y<=12;y++)for(int x=-12;x<=12;x++){int r=x*x+y*y;if(r<=144&&r>=100)rect((float)(cx+x),(float)(cy+y),1,1,c);}
 if(kind==4){for(int i=0;i<8;i++)rect((float)(cx-8+i),(float)(cy-i),2,(float)(2*i+1),c);rect((float)cx,(float)(cy-3),10,7,c);return;}
 if(kind==2){rect((float)(cx-1),(float)(cy-8),2,9,c);rect((float)cx,(float)(cy-1),7,2,c);return;}
 if(kind==3){for(int i=0;i<5;i++)rect((float)(cx-8+i),(float)(cy-1+i),2,2,c);for(int i=0;i<10;i++)rect((float)(cx-4+i),(float)(cy+3-i),2,2,c);return;}
 if(kind==5){rect((float)(cx-7),(float)(cy-8),6,16,c);rect((float)(cx+2),(float)(cy-5),6,13,c);for(int y=-5;y<=4;y+=3){rect((float)(cx-5),(float)(cy+y),2,1,0xff070707u);rect((float)(cx+4),(float)(cy+y+2),2,1,0xff070707u);}return;}
 rect((float)(cx-9),(float)(cy-7),19,2,c);rect((float)(cx-9),(float)(cy+5),19,2,c);rect((float)(cx-9),(float)(cy-6),2,12,c);rect((float)(cx+8),(float)(cy-6),2,12,c);
 if(kind==0){rect((float)(cx-2),(float)(cy+7),4,4,c);rect((float)(cx-7),(float)(cy+11),14,2,c);}
 if(kind==1){rect((float)(cx-4),(float)(cy-4),9,2,c);rect((float)(cx-4),(float)(cy+2),9,2,c);}
}
void asphalt_display_ui_draw(asphalt_display_menu_state state){
 if(!state.active)return;
 uint32_t address,stride,format;psp_display_get(&address,&stride,&format);if(!address)return;
 const psp_render_backend *b=psp_render_current();b->set_target(address,stride,(int)format);
 memset(&paint,0,sizeof paint);paint.sc_x1=479;paint.sc_y1=271;
 rect_role(PSP_SCREEN_PRESERVE,0,0,480,272,0xff070707u);
 for(int y=1;y<272;y+=2)rect_role(PSP_SCREEN_PRESERVE,0,(float)y,480,1,0xff0c0c0cu);
 rect(35,0,3,272,0xff393939u);
 text(65,12,"RECOMP SETTINGS",2,GOLD);
 text(66,32,"APPLY WITHOUT RESTARTING",1,GRAY);
 for(int row=0;row<ASPHALT_MENU_ROWS;row++){
  int cy=50+row*24,selected=row==state.focus;
  if(selected){for(int y=cy-10;y<=cy+10;y++){int d=y>cy?y-cy:cy-y;rect(38,(float)y,432,1,0xff000000u|((unsigned)(40-d)<<16)|((unsigned)(24+2*d)<<8)|(unsigned)(170+2*d));}rect(38,(float)(cy-11),432,1,GOLD);rect(38,(float)(cy+11),432,1,GOLD);}
  icon(36,cy,row==ASPHALT_MENU_AUTOMATIC?5:row==ASPHALT_MENU_COUNTER?6:row==ASPHALT_MENU_LAYOUT?1:row==ASPHALT_MENU_APPLY?3:row==ASPHALT_MENU_BACK?4:row,selected);const char *label=row==0?"RESOLUTION":row==1?"ASPECT RATIO":row==2?"FRAME RATE":row==3?"AUTO ACCELERATE":row==4?"FPS COUNTER":row==ASPHALT_MENU_LAYOUT?"UI LAYOUT":row==ASPHALT_MENU_APPLY?"APPLY":"BACK";
  text(65,cy-6,label,2,WHITE);
  if(row==0){char value[]="< 1X >";value[2]=(char)('0'+state.draft.scale);text(352,cy-6,value,2,GOLD);}
  if(row==1){const char *value=state.draft.aspect==ASPHALT_ASPECT_PSP?"< PSP >":"< DEVICE NATIVE >";int scale=state.draft.aspect==ASPHALT_ASPECT_PSP?2:1;int x=470-(int)strlen(value)*6*scale-10;text(x,cy-(scale==2?6:3),value,scale,GOLD);}
  if(row==2)text(340,cy-6,state.draft.fps==30?"< 30 FPS >":"< 60 FPS >",2,GOLD);
  if(row==3)text(376,cy-6,state.draft.auto_accelerate?"< YES >":"< NO >",2,GOLD);
  if(row==4)text(376,cy-6,state.draft.show_fps?"< ON >":"< OFF >",2,GOLD);
  if(row==ASPHALT_MENU_LAYOUT){const char *value=state.draft.ui_layout==ASPHALT_UI_NEW?"< NEW >":"< ORIGINAL >";text(470-(int)strlen(value)*12-10,cy-6,value,2,GOLD);}
 }
 text(64,234,state.draft.fps==60?"60 FPS NATIVE RENDERING":"30 FPS ORIGINAL SCENE RATE",1,GRAY);
 if(state.error)text(64,246,"APPLY FAILED - PREVIOUS SETTINGS KEPT",1,GOLD);
 else if(state.applying)text(64,246,"APPLYING SETTINGS",1,GOLD);
 else if(state.applied)text(64,246,"SETTINGS APPLIED",1,GOLD);
 else text(64,246,"APPLY SAVES WITHOUT RESTARTING",1,GRAY);
 text(64,260,"X SELECT    O BACK    < > CHANGE",1,WHITE);
}
