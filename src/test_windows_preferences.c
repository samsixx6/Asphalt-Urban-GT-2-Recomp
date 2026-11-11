#include "display_settings.h"
#include "host_input.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void factory(asphalt_display_config c){
#ifdef ASPHALT_WINDOWS_RELEASE
 assert(c.scale==4&&c.aspect==ASPHALT_ASPECT_DEVICE&&c.fps==60&&!c.auto_accelerate&&!c.show_fps&&c.ui_layout==ASPHALT_UI_ORIGINAL);
#else
 assert(c.scale==2&&c.aspect==ASPHALT_ASPECT_DEVICE&&c.fps==60&&c.auto_accelerate&&!c.show_fps&&c.ui_layout==ASPHALT_UI_NEW);
#endif
}
static void same(asphalt_display_config a,asphalt_display_config b){assert(a.scale==b.scale&&a.aspect==b.aspect&&a.fps==b.fps&&a.auto_accelerate==b.auto_accelerate&&a.show_fps==b.show_fps&&a.ui_layout==b.ui_layout);}
int main(int argc,char**argv){
 assert(argc==2);asphalt_display_config c;int recovered=-1;
 assert(!asphalt_display_load_recovery(argv[1],&c,&recovered)&&!recovered);factory(c);
 asphalt_display_config user={6,ASPHALT_ASPECT_PSP,30,1,1,ASPHALT_UI_NEW},candidate={1,ASPHALT_ASPECT_DEVICE,60,0,0,ASPHALT_UI_ORIGINAL};
 assert(!asphalt_display_save_pending(argv[1],&user));assert(!asphalt_display_begin_trial(argv[1]));assert(!asphalt_display_confirm_trial(argv[1]));
 assert(!asphalt_display_load(argv[1],&c));same(c,user);
 assert(!asphalt_display_begin_live_trial(argv[1],&user,&candidate));assert(!asphalt_display_load_recovery(argv[1],&c,&recovered)&&recovered);same(c,user);
 char path[1024];snprintf(path,sizeof path,"%s/display-settings.bin",argv[1]);FILE*f=fopen(path,"wb");assert(f);fwrite("bad",1,3,f);fclose(f);assert(asphalt_display_load_recovery(argv[1],&c,&recovered)&&recovered);factory(c);
#ifdef TEST_WINDOWS_KEYMAP
 asphalt_windows_keymap m={0};uint8_t k[256]={0};assert(!asphalt_windows_keyboard(&m,k,0));
 k[38]=1;assert(asphalt_windows_keyboard(&m,k,0)==0x10);k[38]=0;k[40]=1;assert(asphalt_windows_keyboard(&m,k,0)==0x40);k[40]=0;k[32]=1;assert(asphalt_windows_keyboard(&m,k,0)==0x4000);
 memset(k,0,sizeof k);assert(!asphalt_windows_keyboard(&m,k,1));
 k[38]=1;assert(asphalt_windows_keyboard(&m,k,1)==0x4000);k[40]=1;assert(asphalt_windows_keyboard(&m,k,1)==0xc000);k[32]=1;assert(asphalt_windows_keyboard(&m,k,1)==0xe000);
 k[37]=1;assert(asphalt_windows_keyboard(&m,k,1)==0xe080);
 /* Held driving keys must not become pause-menu confirmation/navigation. */
 assert(asphalt_windows_keyboard(&m,k,0)==0x80);assert(asphalt_windows_keyboard(&m,k,0)==0x80);
 memset(k,0,sizeof k);assert(!asphalt_windows_keyboard(&m,k,0));k[32]=1;assert(asphalt_windows_keyboard(&m,k,0)==0x4000);
 /* A held menu confirmation cannot activate NOS when a race opens. */
 assert(!asphalt_windows_keyboard(&m,k,1));k[32]=0;assert(!asphalt_windows_keyboard(&m,k,1));k[32]=1;assert(asphalt_windows_keyboard(&m,k,1)==0x2000);
 memset(k,0,sizeof k);k['W']=1;assert(asphalt_windows_keyboard(&m,k,1)==0x4000);k['S']=1;k['A']=1;k['D']=1;assert(asphalt_windows_keyboard(&m,k,1)==0xc0a0);
 memset(k,0,sizeof k);assert(!asphalt_windows_keyboard(&m,k,1));
 /* A newly pressed key at the first race sample is not a held menu key. */
 memset(&m,0,sizeof m);memset(k,0,sizeof k);assert(!asphalt_windows_keyboard(&m,k,0));k['W']=1;assert(asphalt_windows_keyboard(&m,k,1)==0x4000);
 memset(k,0,sizeof k);assert(!asphalt_windows_keyboard(&m,k,0));k[32]=1;assert(asphalt_windows_keyboard(&m,k,1)==0x2000);
 memset(k,0,sizeof k);k['Q']=k['E']=k['X']=k['Z']=k['C']=k['V']=k[13]=k[8]=1;assert(asphalt_windows_keyboard(&m,k,1)==0xf309);
#endif
 puts("Platform defaults, saved preferences, rollback, malformed recovery and context keyboard mapping passed");return 0;
}
