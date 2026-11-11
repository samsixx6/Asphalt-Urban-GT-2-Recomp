#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "display_menu.h"
#include <time.h>
static asphalt_display_menu_state menu;
static asphalt_display_config current;
static int (*save_pending)(const asphalt_display_config *);
static uint32_t previous,repeat_button;
static uint64_t repeat_at;
static int release_gate;
void asphalt_display_menu_configure(const asphalt_display_config *c,int (*save)(const asphalt_display_config *)){
 current=*c;menu=(asphalt_display_menu_state){.draft=*c};save_pending=save;previous=repeat_button=0;release_gate=0;
}
void asphalt_display_menu_open(void){
 menu=(asphalt_display_menu_state){.draft=current,.active=1,.revision=menu.revision+1};
 release_gate=1;previous=repeat_button=0;
}
int asphalt_display_menu_active(void){return menu.active;}
int asphalt_display_menu_consuming(void){return menu.active||release_gate;}
asphalt_display_menu_state asphalt_display_menu_snapshot(void){return menu;}
void asphalt_display_menu_apply_finished(const asphalt_display_config *active,int success){
 if(!menu.applying)return;
 menu.applying=0;menu.applied=success!=0;menu.error=!success;
 if(success&&active){current=*active;menu.draft=*active;}
 menu.revision++;
}
static void close_menu(void){menu.active=0;menu.draft=current;release_gate=1;menu.revision++;}
void asphalt_display_menu_input_at(uint32_t buttons,uint64_t now){
 buttons&=0xffffu;
 if(release_gate){previous=buttons;if(!buttons){release_gate=0;previous=0;}return;}
 if(!menu.active||menu.applying){previous=buttons;return;}
 uint32_t edge=buttons&~previous,nav=buttons&0xf0u,event=edge;
 if(nav&&!(nav&(nav-1))){
  if(nav!=repeat_button){repeat_button=nav;repeat_at=now+400;}
  else if(now>=repeat_at){event|=nav;repeat_at=now+100;}
 }else repeat_button=0;
 previous=buttons;
 if(edge&0x2000u){close_menu();return;}
 if(event&0x10u)menu.focus=(menu.focus+ASPHALT_MENU_ROWS-1)%ASPHALT_MENU_ROWS;
 if(event&0x40u)menu.focus=(menu.focus+1)%ASPHALT_MENU_ROWS;
 if((event&0xa0u)&&menu.focus<ASPHALT_MENU_APPLY){
  if(menu.focus==0)menu.draft.scale=(menu.draft.scale-1+((event&0x20u)?1:5))%6+1;
  else if(menu.focus==1)menu.draft.aspect=menu.draft.aspect==ASPHALT_ASPECT_PSP?ASPHALT_ASPECT_DEVICE:ASPHALT_ASPECT_PSP;
  else if(menu.focus==2)menu.draft.fps=menu.draft.fps==60?30:60;
  else if(menu.focus==ASPHALT_MENU_AUTOMATIC)menu.draft.auto_accelerate=!menu.draft.auto_accelerate;
  else if(menu.focus==ASPHALT_MENU_COUNTER)menu.draft.show_fps=!menu.draft.show_fps;
  else menu.draft.ui_layout=menu.draft.ui_layout==ASPHALT_UI_ORIGINAL?ASPHALT_UI_NEW:ASPHALT_UI_ORIGINAL;
  menu.error=menu.applied=0;
 }
 if(edge&0x4000u){
  if(menu.focus==ASPHALT_MENU_BACK){close_menu();return;}
  if(menu.focus==ASPHALT_MENU_APPLY){menu.applied=0;if(!save_pending||save_pending(&menu.draft))menu.error=1;else{menu.error=0;menu.applying=1;}}
 }
 if(event)menu.revision++;
}
void asphalt_display_menu_input(uint32_t buttons){
 struct timespec t;
#ifdef _WIN32
 timespec_get(&t,TIME_UTC);
#else
 clock_gettime(CLOCK_MONOTONIC,&t);
#endif
 asphalt_display_menu_input_at(buttons,(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000);
}
