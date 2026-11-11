/* CPU-owner-only diagnostic map; production menu integration is added after
 * the original widget relationships have been verified in a bounded trace. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "display_game_menu.h"
#include "display_icon.h"
#include "display_hooks.h"
#include "display_menu.h"
#include "display_session.h"
#include "display_pose.h"
#include "display_semantic_hooks.h"
#include "display_semantics.h"
#include "native_engine.h"
#include "native_preview.h"
#include "auto_accelerate.h"
#include "race_controls.h"
#include "startup_logos.h"
static int native_aspect;
static float camera_x=1;
void asphalt_display_hooks_configure(int device_native,float drawable_aspect) {
    native_aspect=device_native && isfinite(drawable_aspect) && drawable_aspect>0;
    camera_x=native_aspect?(480.0f/272)/drawable_aspect:1;
}
static int trace_enabled=-1;
static uint32_t last_pc,last_object,last_state,last_selection;
static int entering_menu;
static int startup_title_seen,startup_copyright_pending;
static uint32_t game_allocate(unsigned n) {
    psp_cpu_state saved=psp_cpu;psp_cpu.r[4]=n;psp_call_from_host(0x08804044u);
    uint32_t out=psp_cpu.r[2];psp_cpu=saved;return out;
}
static void game_release(uint32_t p) {
    psp_cpu_state saved=psp_cpu;psp_cpu.r[4]=p;psp_call_from_host(0x088040C8u);psp_cpu=saved;
}
static void widget(uint32_t at,int depth) {
    if(!at || !psp_mem_ptr(at,0x160))return;
    char name[100];uint32_t np=psp_read32(at+0x38);
    if(!np || !psp_mem_ptr(np,100))name[0]=0;else psp_str(np,name,sizeof name);
    unsigned count=psp_read32(at+0x34);uint32_t list=psp_read32(at+0x58);
    fprintf(stderr,"menu-widget: depth=%d at=%08X vtable=%08X name=%s count=%u list=%08X props=%u,%u,%u,%u\n",
        depth,at,psp_read32(at),name,count,list,psp_read32(at+0x104),psp_read32(at+0x108),psp_read32(at+0x10c),psp_read32(at+0x148));
    if(depth<2 && count<64 && list && psp_mem_ptr(list,count*4))
        for(unsigned i=0;i<count;i++)widget(psp_read32(list+4*i),depth+1);
}
int asphalt_display_hook_enter(uint32_t pc,uint32_t object) {
    /* Isolated camera calls cannot mutate host bookkeeping or allocate/free
     * guest objects. Deny these before any pose/native-engine hook runs. */
    if(asphalt_preview_active()){
        if(pc==0x08804000u||pc==0x08804044u||pc==0x08804088u||pc==0x088040C8u)asphalt_preview_forbid();
        if(pc==0x088A52C0u && native_aspect && fabsf(psp_cpu.f[13]-9.0f/16)<0.001f)psp_cpu.f[13]*=camera_x;
        return 0;
    }
    if(pc==0x08913D40u){
        uint32_t pose=asphalt_native_engine_part_pose(object,psp_cpu.r[31]);
        if(pose){psp_cpu.r[2]=pose;return 1;}
    }
    asphalt_pose_hook_enter(pc,object);
    if(pc==0x08AD1CB8u)asphalt_startup_logos_tick(object,psp_ctrl_buttons());
    if(pc==0x088DF0A4u)asphalt_race_controls_loading_draw(object);
    if(pc==0x088DEB4Cu)asphalt_race_controls_loading_end(object);
    if(pc==0x0899C398u)asphalt_race_controls_script(object,psp_cpu.r[5],psp_cpu.r[31]);
    asphalt_semantic_hook_enter(pc,object);
    if(pc==0x08A3BED4u||pc==0x088BCCC0u)asphalt_native_engine_model_enter(psp_cpu.r[5],psp_cpu.r[31]);
    if(pc==0x08A380FCu)asphalt_native_engine_camera_enter(object,psp_cpu.r[6]);
    if(pc==0x08A14484u)asphalt_native_engine_rear_enter(object);
    if(pc==0x088788FCu){asphalt_race_controls_root_enter(object);asphalt_display_session_race_observed(asphalt_race_controls_racing());asphalt_native_engine_root_enter(object);}
    if(pc==0x08879D38u)asphalt_native_engine_group_enter(object,psp_cpu.r[5]);
    if(pc==0x08804088u||pc==0x088040C8u){asphalt_startup_logos_released(object);asphalt_display_icon_released(object);asphalt_semantics_released(object);asphalt_race_controls_released(object);asphalt_native_engine_released(object);}
    if(pc==0x08941000u){
        char name[40]={0};uint32_t p=psp_cpu.r[5];
        if(p&&psp_mem_ptr(p,sizeof name))psp_str(p,name,sizeof name);
        if(getenv("ASPHALT_MENU_TRACE"))fprintf(stderr,"display-screen: %s\n",name);
        if(!strcmp(name,"ASPressStart")){
            startup_copyright_pending=!startup_title_seen;
            startup_title_seen=1;
        }else if(startup_title_seen)startup_copyright_pending=0;
        asphalt_display_session_screen(name);
        asphalt_race_controls_screen(name);
        if(!strcmp(name,"ASMainMenu")||!strcmp(name,"ASOption"))entering_menu=1;
    }
    if(pc==0x08AC0A24u && asphalt_display_menu_consuming()){
        asphalt_display_menu_input(psp_ctrl_buttons());return 1;
    }
    if(pc==0x08AC1248u && asphalt_display_options_is_recomp(object)){
        asphalt_display_menu_open();fprintf(stderr,"display-menu: opened\n");return 1;
    }
    if(pc==0x08946614u && psp_cpu.r[31]>=0x08A85700u && psp_cpu.r[31]<=0x08A857C0u) {
        int icon=asphalt_display_icon_enqueue(psp_cpu.r[20],psp_cpu.r[16],psp_cpu.r[5]);
        if(icon>0)return 1;
        if(icon<0){static int logged;if(!logged++)fprintf(stderr,"display-icon: invalid draw context\n");}
    }
    /* The original builder consumes inverse aspect in f13. It writes both
     * the projection and horizontal-frustum parameters from that same input.
     * Square effect cameras keep their original inverse aspect1. */
    if(pc==0x088A52C0u && native_aspect && fabsf(psp_cpu.f[13]-9.0f/16)<0.001f)
        psp_cpu.f[13]*=camera_x;
    if(trace_enabled<0){const char *v=getenv("ASPHALT_MENU_TRACE");trace_enabled=v && *v=='1';}
    if(pc==0x08A3C33Cu && getenv("ASPHALT_CAMERA_TRACE")) {
        uint32_t m=psp_cpu.r[8];static uint32_t seen[16];static int n;
        if(psp_mem_ptr(m,64)){
            float p[16];memcpy(p,psp_mem_ptr(m,64),64);int known=0;
            for(int i=0;i<n;i++)if(seen[i]==m)known=1;
            if(!known && n<16){seen[n++]=m;
                fprintf(stderr,"camera-source: renderer=%08X source=%08X P0=%g P5=%g P11=%g ra=%08X frames:",object,m,p[0],p[5],p[11],psp_cpu.r[31]);
                psp_frame *f=psp_cpu.frame;for(int i=0;i<14&&f;i++,f=f->prev)fprintf(stderr," %08X",f->ret);fprintf(stderr,"\n");
                if(fabsf(p[11])>0.5f && fabsf(p[5]/p[0])>1.7f && fabsf(p[5]/p[0])<1.9f){psp_mem_watch_write(m);psp_mem_watch_read(m);}
            }
        }return 0;
    }
    if(pc==0x0880834Cu && object==0 && getenv("ASPHALT_CAMERA_TRACE")) {
        uint32_t m=psp_cpu.r[5];
        if(psp_mem_ptr(m,64)) {
            float p[16];memcpy(p,psp_mem_ptr(m,64),64);
            static uint32_t known[16];static int n;
            if(fabsf(p[11])>0.5f && fabsf(p[15])<0.001f && fabsf(p[0])>0.01f) {
                int found=0;for(int i=0;i<n;i++)if(known[i]==m)found=1;
                if(!found && n<16){known[n++]=m;
                    fprintf(stderr,"camera-projection: at=%08X P0=%g P5=%g P11=%g aspect=%g ra=%08X frames:",m,p[0],p[5],p[11],fabsf(p[5]/p[0]),psp_cpu.r[31]);
                    psp_frame *f=psp_cpu.frame;for(int i=0;i<12 && f;i++,f=f->prev)fprintf(stderr," %08X",f->ret);fprintf(stderr,"\n");
                    /* Source-matrix watch is installed at08A3C33C instead. */
                }
            }
        }
        return 0;
    }
    if(!trace_enabled || !object || !psp_mem_ptr(object,0x350))return 0;
    if(pc==0x08AC1248u || pc==0x08AC0A24u || pc==0x08AC0570u) {
        uint32_t state=psp_read32(object+0x2a8),sel=0xffffffffu,bar=psp_read32(object+0x218);
        if(bar && psp_mem_ptr(bar,0x150))sel=psp_read32(bar+0x148);
        if(pc!=last_pc || object!=last_object || state!=last_state || sel!=last_selection)
            fprintf(stderr,"menu-hook: pc=%08X object=%08X state=%u selected=%u arg1=%08X\n",pc,object,state,sel,psp_cpu.r[5]);
        last_pc=pc;last_object=object;last_state=state;last_selection=sel;
    }
    return 0;
}
void asphalt_display_hook_leave(uint32_t pc,uint32_t object) {
    if(asphalt_preview_active())return;
    if(pc==0x08A14484u)asphalt_native_engine_rear_leave();
    if(pc==0x08A3BED4u||pc==0x088BCCC0u)asphalt_native_engine_model_leave();
    if(pc==0x0887B130u)asphalt_auto_accelerate_query_complete(object);
    asphalt_pose_hook_leave(pc,object);
    asphalt_semantic_hook_leave(pc,object);
    if(pc==0x08804000u||pc==0x08804044u)asphalt_race_controls_allocated(psp_cpu.r[2],object);
    /* Owned ULES00719 title:08AD3174 initializes the two copyright text
     * widgets during the8..20.5s phase of08AD1CB8. Run it intact, then end
     * only this local phase. The remainder of this update fades the text to
     * zero; the next update runs original08AD3294 cleanup and title setup.
     * The one-shot closes on any subsequent screen request, including title
     * reentry. No global clock, delta, logo, profile or movie is changed. */
    if(pc==0x08AD3174u && startup_copyright_pending && object &&
       psp_mem_ptr(object,0x160) && psp_read32(object)==0x08B3DAE8u){
        float elapsed=psp_read_f32(object+340);
        if(isfinite(elapsed) && elapsed>=8.0f && elapsed<20.5f){
            psp_write_f32(object+340,20.5f);startup_copyright_pending=0;
            fprintf(stderr,"startup-copyright: original text initialized; local phase ended\n");
        }
    }
    if(pc==0x088788FCu){asphalt_native_engine_root_leave(object);asphalt_race_controls_root_leave(object);asphalt_display_session_race_observed(asphalt_race_controls_racing());}
    if(pc==0x08941000u && entering_menu){entering_menu=0;asphalt_display_session_menu_seen();}
    if(pc==0x08ABF8B8u){
        int result=asphalt_display_extend_options(object,game_allocate,game_release);
        fprintf(stderr,"menu-extend: result=%d\n",result);
        if(result==0){int icon=asphalt_display_install_icon(object,game_allocate,game_release);fprintf(stderr,"menu-icon: result=%d\n",icon);}
    }
    if(pc==0x08AC0570u){asphalt_display_session_menu_seen();asphalt_display_session_options(1);}
    if(pc==0x08AC09C0u)asphalt_display_session_options(0);
    if(trace_enabled==1 && pc==0x08ABF8B8u) {
        fprintf(stderr,"menu-init: object=%08X\n",object);
        widget(object,0);
        for(unsigned i=0x210;i<=0x2a4;i+=4)fprintf(stderr,"menu-field: %03X=%08X\n",i,psp_read32(object+i));
        const void *ram=psp_mem_ptr(0x08800000u,0x1800000u);
        FILE *snapshot=ram?fopen("menu-ram.bin","wb"):NULL;
        if(snapshot){fwrite(ram,1,0x1800000u,snapshot);fclose(snapshot);}
    }
}
