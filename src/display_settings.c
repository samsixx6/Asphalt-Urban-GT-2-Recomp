#include "display_settings.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif
typedef struct { asphalt_display_config confirmed,pending;int trial; } settings;
static asphalt_display_persist before_write,after_write;static void *persist_user;
void asphalt_display_settings_set_persistence(asphalt_display_persist before,asphalt_display_persist after,void *user){before_write=before;after_write=after;persist_user=user;}
static settings defaults(void){
#ifdef ASPHALT_WINDOWS_RELEASE
 settings s={{4,ASPHALT_ASPECT_DEVICE,60,0,0,ASPHALT_UI_ORIGINAL},{4,ASPHALT_ASPECT_DEVICE,60,0,0,ASPHALT_UI_ORIGINAL},0};
#else
 settings s={{2,ASPHALT_ASPECT_DEVICE,60,1,0,ASPHALT_UI_NEW},{2,ASPHALT_ASPECT_DEVICE,60,1,0,ASPHALT_UI_NEW},0};
#endif
 return s;
}
int asphalt_display_validate(const asphalt_display_config *c){return !c||c->scale<1||c->scale>6||(c->aspect!=ASPHALT_ASPECT_PSP&&c->aspect!=ASPHALT_ASPECT_DEVICE)||(c->fps!=30&&c->fps!=60)||(c->auto_accelerate!=0&&c->auto_accelerate!=1)||(c->show_fps!=0&&c->show_fps!=1)||(c->ui_layout!=ASPHALT_UI_ORIGINAL&&c->ui_layout!=ASPHALT_UI_NEW)?-1:0;}
static uint32_t get32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(unsigned char *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t checksum(const unsigned char *p,int bytes){uint32_t h=2166136261u;for(int i=0;i<bytes;i++)h=(h^p[i])*16777619u;return h;}
static int filename(const char *root,char *out,size_t n,const char *suffix){if(!root||!*root)return -1;int size=snprintf(out,n,"%s/display-settings.bin%s",root,suffix);return size<0||(size_t)size>=n?-1:0;}
static int read_settings(const char *root,settings *s){
 *s=defaults();char path[1024];if(filename(root,path,sizeof path,""))return -1;
 FILE *f=fopen(path,"rb");if(!f)return errno==ENOENT?0:-1;
 unsigned char b[68];size_t bytes=fread(b,1,sizeof b,f);int ok=fgetc(f)==EOF&&!ferror(f);fclose(f);
 if(!ok)return -1;
 settings c=defaults();
 if(bytes==36&&!memcmp(b,"AS2DSP1",8)&&get32(b+8)==1&&checksum(b,32)==get32(b+32)){
  c.confirmed=(asphalt_display_config){(int)get32(b+12),(asphalt_aspect_mode)get32(b+16),60,1,0,ASPHALT_UI_ORIGINAL};
  c.pending=(asphalt_display_config){(int)get32(b+20),(asphalt_aspect_mode)get32(b+24),60,1,0,ASPHALT_UI_ORIGINAL};c.trial=(int)get32(b+28);
 }else if(bytes==44&&!memcmp(b,"AS2DSP2",8)&&get32(b+8)==2&&checksum(b,40)==get32(b+40)){
  c.confirmed=(asphalt_display_config){(int)get32(b+12),(asphalt_aspect_mode)get32(b+16),(int)get32(b+20),1,0,ASPHALT_UI_ORIGINAL};
  c.pending=(asphalt_display_config){(int)get32(b+24),(asphalt_aspect_mode)get32(b+28),(int)get32(b+32),1,0,ASPHALT_UI_ORIGINAL};c.trial=(int)get32(b+36);
 }else if(bytes==52&&!memcmp(b,"AS2DSP3",8)&&get32(b+8)==3&&checksum(b,48)==get32(b+48)){
  c.confirmed=(asphalt_display_config){(int)get32(b+12),(asphalt_aspect_mode)get32(b+16),(int)get32(b+20),(int)get32(b+24),0,ASPHALT_UI_ORIGINAL};
  c.pending=(asphalt_display_config){(int)get32(b+28),(asphalt_aspect_mode)get32(b+32),(int)get32(b+36),(int)get32(b+40),0,ASPHALT_UI_ORIGINAL};c.trial=(int)get32(b+44);
 }else if(bytes==60&&!memcmp(b,"AS2DSP4",8)&&get32(b+8)==4&&checksum(b,56)==get32(b+56)){
  c.confirmed=(asphalt_display_config){(int)get32(b+12),(asphalt_aspect_mode)get32(b+16),(int)get32(b+20),(int)get32(b+24),(int)get32(b+28),ASPHALT_UI_ORIGINAL};
  c.pending=(asphalt_display_config){(int)get32(b+32),(asphalt_aspect_mode)get32(b+36),(int)get32(b+40),(int)get32(b+44),(int)get32(b+48),ASPHALT_UI_ORIGINAL};c.trial=(int)get32(b+52);
 }else if(bytes==68&&!memcmp(b,"AS2DSP5",8)&&get32(b+8)==5&&checksum(b,64)==get32(b+64)){
  c.confirmed=(asphalt_display_config){(int)get32(b+12),(asphalt_aspect_mode)get32(b+16),(int)get32(b+20),(int)get32(b+24),(int)get32(b+28),(asphalt_ui_layout)get32(b+32)};
  c.pending=(asphalt_display_config){(int)get32(b+36),(asphalt_aspect_mode)get32(b+40),(int)get32(b+44),(int)get32(b+48),(int)get32(b+52),(asphalt_ui_layout)get32(b+56)};c.trial=(int)get32(b+60);
 }else return -1;
 if(asphalt_display_validate(&c.confirmed)||asphalt_display_validate(&c.pending)||(c.trial!=0&&c.trial!=1))return -1;
 *s=c;return 0;
}
static int write_settings(const char *root,const settings *s){
 char path[1024],tmp[1024];if(filename(root,path,sizeof path,"")||filename(root,tmp,sizeof tmp,".part"))return -1;
 unsigned char b[68]={0};memcpy(b,"AS2DSP5",8);put32(b+8,5);put32(b+12,(uint32_t)s->confirmed.scale);put32(b+16,(uint32_t)s->confirmed.aspect);put32(b+20,(uint32_t)s->confirmed.fps);put32(b+24,(uint32_t)s->confirmed.auto_accelerate);put32(b+28,(uint32_t)s->confirmed.show_fps);put32(b+32,(uint32_t)s->confirmed.ui_layout);
 put32(b+36,(uint32_t)s->pending.scale);put32(b+40,(uint32_t)s->pending.aspect);put32(b+44,(uint32_t)s->pending.fps);put32(b+48,(uint32_t)s->pending.auto_accelerate);put32(b+52,(uint32_t)s->pending.show_fps);put32(b+56,(uint32_t)s->pending.ui_layout);put32(b+60,(uint32_t)s->trial);put32(b+64,checksum(b,64));
 if(before_write&&before_write(persist_user))return -1;
 FILE *f=fopen(tmp,"wb");if(!f)return -1;
 int ok=fwrite(b,1,sizeof b,f)==sizeof b && !fflush(f);
#ifdef _WIN32
 if(ok)ok=!_commit(_fileno(f));
#else
 if(ok)ok=!fsync(fileno(f));
#endif
 if(fclose(f))ok=0;
 if(ok){
#ifdef _WIN32
  ok=MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
  ok=rename(tmp,path)==0;
#endif
 }
 if(!ok)remove(tmp);
 if(ok&&after_write&&after_write(persist_user))return -1;
 return ok?0:-1;
}
int asphalt_display_load_recovery(const char *root,asphalt_display_config *out,int *recovered){
 if(!out)return -1;
 settings s;int result=read_settings(root,&s);
 if(recovered)*recovered=s.trial||result!=0;
 if(s.trial){s.pending=s.confirmed;s.trial=0;if(write_settings(root,&s))result=-1;}
 *out=s.pending;return result;
}
int asphalt_display_load(const char *root,asphalt_display_config *out){return asphalt_display_load_recovery(root,out,NULL);}
int asphalt_display_save_pending(const char *root,const asphalt_display_config *config){
 if(asphalt_display_validate(config))return -1;
 settings s;read_settings(root,&s);s.pending=*config;s.trial=0;return write_settings(root,&s);
}
int asphalt_display_begin_trial(const char *root){
 settings s;if(read_settings(root,&s))return -1;
 if(s.trial)s.pending=s.confirmed;
 s.trial=1;return write_settings(root,&s);
}
int asphalt_display_confirm_trial(const char *root){
 settings s;if(read_settings(root,&s))return -1;
 if(!s.trial)return 0;
 s.confirmed=s.pending;s.trial=0;return write_settings(root,&s);
}
int asphalt_display_begin_live_trial(const char *root,const asphalt_display_config *current,const asphalt_display_config *candidate){
 if(asphalt_display_validate(current)||asphalt_display_validate(candidate))return -1;
 settings s={*current,*candidate,1};return write_settings(root,&s);
}
