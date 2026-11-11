#include "display_session.h"
#include "display_menu.h"
#include "display_view.h"
#include "display_ui.h"
#include "display_hooks.h"
#include "display_pose.h"
#include "display_semantic_hooks.h"
#include "native_engine.h"
#include "auto_accelerate.h"
#include "race_controls.h"
#include "startup_logos.h"
#include "psprecomp/render.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
static char settings_root[1024];
static asphalt_display_config active={1,ASPHALT_ASPECT_PSP,60,1,0,ASPHALT_UI_ORIGINAL};
static atomic_int recovered_settings,fps_counter,controls_layout;
static int started,menu_seen,menu_frames,confirmed;
static asphalt_display_config queued;
static int has_queued,geometry[6];
static int race_view,observed_race,options_active;
static float safe_left,safe_right;
static char previous_screen[40];
static void anchors(void){int racing=(race_view||observed_race)&&!options_active;psp_ge_set_screen_anchors(active.aspect==ASPHALT_ASPECT_DEVICE&&racing,safe_left,safe_right);psp_ge_set_hud_layout(active.ui_layout,racing);
#if ASPHALT_HUD_DIAGNOSTICS
 asphalt_semantic_diagnostics_layout(active.ui_layout,racing);
#endif
}
static int semantic_enabled(void){return active.aspect==ASPHALT_ASPECT_DEVICE||active.ui_layout==ASPHALT_UI_NEW;}
static void publish_controls_layout(void){atomic_store_explicit(&controls_layout,(active.ui_layout==ASPHALT_UI_NEW?1:0)|(active.aspect==ASPHALT_ASPECT_PSP?2:0),memory_order_release);}
static int save(const asphalt_display_config *c){
 if(!started||has_queued||asphalt_display_validate(c))return -1;
 queued=*c;has_queued=1;return 0;
}
int asphalt_display_session_start(const char *root){
 started=menu_seen=menu_frames=confirmed=race_view=observed_race=options_active=has_queued=0;previous_screen[0]=0;memset(geometry,0,sizeof geometry);atomic_store(&recovered_settings,0);atomic_store(&fps_counter,0);
 asphalt_startup_logos_reset();
 if(!root||!root[0]||strlen(root)>=sizeof settings_root)return -1;
 strcpy(settings_root,root);int recovered=0;
 if(asphalt_display_load_recovery(root,&active,&recovered)){
#ifdef ASPHALT_WINDOWS_RELEASE
  active=(asphalt_display_config){4,ASPHALT_ASPECT_DEVICE,60,0,0,ASPHALT_UI_ORIGINAL};
#else
  active=(asphalt_display_config){1,ASPHALT_ASPECT_PSP,60,1,0,ASPHALT_UI_ORIGINAL};
#endif
  if(asphalt_display_save_pending(root,&active))return -1;
 }
 if(asphalt_display_begin_trial(root))return -1;
 if(psp_render_gl_set_native_scene_rate(active.fps))return -1;
 asphalt_native_engine_configure(active.fps);
 asphalt_auto_accelerate_configure(active.auto_accelerate);
 asphalt_race_controls_configure(active.auto_accelerate);
 asphalt_pose_begin_session();
 asphalt_semantic_hooks_begin_session(semantic_enabled());
 atomic_store(&recovered_settings,recovered);started=1;
 atomic_store_explicit(&fps_counter,active.show_fps,memory_order_release);
 publish_controls_layout();anchors();
 /* Startup publication precedes worker creation. Live publication uses the
  * worker rendezvous after a complete frame has been submitted. */
 psp_render_gl_set_stretch(active.aspect==ASPHALT_ASPECT_DEVICE);
 asphalt_display_menu_configure(&active,save);
 psp_display_set_present_hook(asphalt_display_session_frame);
 psp_display_set_post_present_hook(asphalt_display_session_post_present);
 psp_display_set_repeat_caller(0x08807FE8u);
 fprintf(stderr,"display-start: scale=%d aspect=%s fps=%d recovered=%d auto_accelerate=%d ui_layout=%d\n",active.scale,active.aspect==ASPHALT_ASPECT_DEVICE?"device":"psp",active.fps,recovered,active.auto_accelerate,active.ui_layout);
 return 0;
}
asphalt_display_config asphalt_display_session_config(void){return active;}
int asphalt_display_session_geometry(int width,int height,const int insets[4]){
 asphalt_display_view v;if(asphalt_display_view_make(&active,width,height,insets,&v))return -1;
 geometry[0]=width;geometry[1]=height;for(int i=0;i<4;i++)geometry[i+2]=insets?insets[i]:0;
 int native=active.aspect==ASPHALT_ASPECT_DEVICE;
 asphalt_display_hooks_configure(native,(float)width/(float)height);
 psp_ge_set_screen_transform(v.ui_x_scale,v.ui_y_scale,v.ui_x_offset,v.ui_y_offset);
 /* Match the New Camera strip's safe-area geometry, then convert its left
  * edge to the actual presented viewport (also for PSP letterboxing). */
 float safe_width=(float)(width-geometry[2]-geometry[4]);
 float safe_height=(float)(height-geometry[3]-geometry[5]);
 float touch_scale=fminf(safe_width/1284,safe_height/597);
 float money_limit=((float)width-geometry[4]-119*touch_scale-v.x)*480/v.width;
 psp_ge_set_hud_money_limit(fmaxf(0,fminf(480*.902f,money_limit)));
 safe_left=insets?(float)insets[0]*480/width:0;
 safe_right=insets?(float)insets[2]*480/width:0;anchors();
 return 0;
}
void asphalt_display_session_menu_seen(void){if(!menu_seen){menu_seen=1;menu_frames=0;}}
void asphalt_display_session_screen(const char *name){
 if(!name)return;
 /* Verified original Arcade flow: ASSelectTrack -> LaunchVideo -> race.
  * A movie launched elsewhere must not turn on race-specific HUD layout. */
 if(!strcmp(name,"LaunchVideo")&&!strcmp(previous_screen,"ASSelectTrack"))race_view=1;
 else if(!strncmp(name,"AS",2)&&strcmp(name,"ASOption"))race_view=observed_race=0;
 snprintf(previous_screen,sizeof previous_screen,"%s",name);anchors();
}
void asphalt_display_session_options(int enabled){options_active=enabled;anchors();}
void asphalt_display_session_race_observed(int present){observed_race=present!=0;anchors();}
void asphalt_display_session_frame(void){
 if(started&&menu_seen&&!confirmed&&!has_queued&&++menu_frames>=10){
  if(!asphalt_display_confirm_trial(settings_root)){confirmed=1;fprintf(stderr,"display-trial: confirmed after usable menu frames\n");}
 }
 asphalt_display_ui_draw(asphalt_display_menu_snapshot());
}
void asphalt_display_session_post_present(void){
 if(!started||!has_queued)return;
 asphalt_display_config candidate=queued;has_queued=0;
 int same=candidate.scale==active.scale&&candidate.aspect==active.aspect&&candidate.fps==active.fps&&candidate.auto_accelerate==active.auto_accelerate&&candidate.show_fps==active.show_fps&&candidate.ui_layout==active.ui_layout;
 if(same){asphalt_display_menu_apply_finished(&active,1);return;}
 if(geometry[0]<=0||geometry[1]<=0){asphalt_display_menu_apply_finished(&active,0);return;}
 /* One durable record protects the old usable state before any GPU mutation. */
 if(asphalt_display_begin_live_trial(settings_root,&active,&candidate)){
  confirmed=1;menu_frames=0; /* Never confirm a rejected candidate if rollback I/O fails. */
  asphalt_display_config ignored;(void)asphalt_display_load_recovery(settings_root,&ignored,NULL);
  asphalt_display_menu_apply_finished(&active,0);return;
 }
 if(psp_render_gl_reconfigure(candidate.scale,candidate.fps,candidate.aspect==ASPHALT_ASPECT_DEVICE)){
  confirmed=1;menu_frames=0;
  asphalt_display_config ignored;(void)asphalt_display_load_recovery(settings_root,&ignored,NULL);
  asphalt_display_menu_apply_finished(&active,0);return;
 }
 int semantic_was_enabled=semantic_enabled();
 if(candidate.fps!=active.fps)(void)asphalt_native_engine_request_rate(candidate.fps);
 if(candidate.auto_accelerate!=active.auto_accelerate){asphalt_auto_accelerate_configure(candidate.auto_accelerate);asphalt_race_controls_set_automatic(candidate.auto_accelerate);}
 active=candidate;
 /* Provenance names original producers, independently of their destination
  * layout. Preserve queued NOS layers and other verified draws across live
  * geometry changes. Only an observation enable/disable transition starts a
  * new generation and invalidates suspended scopes. */
 if(semantic_was_enabled!=semantic_enabled())asphalt_semantic_hooks_begin_session(semantic_enabled());
 (void)asphalt_display_session_geometry(geometry[0],geometry[1],geometry+2);
 atomic_store_explicit(&fps_counter,active.show_fps,memory_order_release);
 publish_controls_layout();
 confirmed=menu_frames=0;
 asphalt_display_menu_apply_finished(&active,1);
 fprintf(stderr,"display-live: scale=%d aspect=%s fps=%d auto_accelerate=%d show_fps=%d ui_layout=%d\n",active.scale,active.aspect==ASPHALT_ASPECT_DEVICE?"device":"psp",active.fps,active.auto_accelerate,active.show_fps,active.ui_layout);
}
int asphalt_display_session_restart_requested(void){return 0;} /* compatibility getter */
int asphalt_display_session_recovered(void){return atomic_load(&recovered_settings);}
int asphalt_display_session_fps_counter(void){return atomic_load_explicit(&fps_counter,memory_order_acquire);}
int asphalt_display_session_controls_layout(void){return atomic_load_explicit(&controls_layout,memory_order_acquire);}
