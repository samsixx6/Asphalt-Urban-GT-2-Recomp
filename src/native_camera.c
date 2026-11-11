#include "native_camera.h"
#include "native_preview.h"
#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/mem.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static int valid(uint32_t p,unsigned size){return p>=0x08800000u&&size<=0x1800000u&&p<=0x0a000000u-size&&psp_mem_ptr(p,size)!=NULL;}
typedef struct {uint32_t child,controller,node,original_id,pointers[4],rear_calculator,rear_source;float delta[3],pass_near,pass_far;asphalt_camera_frame *frame;asphalt_preview_capture extra_prepare,extra_capture;void *extra_user;} camera_capture;
static int source_animation(void *user){
 camera_capture *c=user;if(c->extra_prepare&&!c->extra_prepare(c->extra_user))return 0;uint32_t calculator=psp_read32(c->controller+52);
 /* This original calculator reads a graphics camera's cached animated pose.
  * Its controller delta cannot advance that source. Sample only the verified
  * single forward clip; all other camera/animation contracts stay coarse. */
 if(!valid(calculator,56)||psp_read32(calculator)!=0x08b3afacu||psp_read32(calculator+4)!=4)return 1;
 uint32_t handle=psp_read32(calculator+52);if(!valid(handle,24)||psp_read8(handle+20)||psp_read8(handle+12)!=1)return 1;
 uint32_t graphics=psp_read32(handle+16);if(!valid(graphics,1712))return 1;
 uint32_t vt=psp_read32(graphics);if(!valid(vt,20)||psp_read32(vt+16)!=0x0885d02cu)return 1;
 if(psp_read32(graphics+1668)!=2||psp_read32(graphics+1708)!=2)return 1;
 uint32_t names=psp_read32(graphics+1704),components=psp_read32(graphics+1664),owner=psp_read32(calculator+48);
 if(!valid(names,8)||!valid(components,8))return 1;
 unsigned index=2;for(unsigned i=0;i<2;i++)if(psp_read32(names+4*i)==owner)index=i;if(index==2)return 1;
 uint32_t component=psp_read32(components+4*index);if(!valid(component,164)||psp_read32(component)!=0x08b36b3cu)return 1;
 uint32_t animation=psp_read32(component+160);if(!valid(animation,196)||psp_read32(animation+180)!=1||psp_read32(animation+172)||psp_read32(animation+124)!=1||psp_read32(animation+120)||psp_read32(animation+184))return 1;
 uint32_t phase=psp_read32(animation+192);if(!valid(phase,8))return 1;
 uint32_t state=psp_read32(phase),description=psp_read32(phase+4);if(!valid(state,48)||!valid(description,64)||psp_read32(description+4)!=1)return 1;
 uint32_t curve=psp_read32(description+12),parameters=psp_read32(animation+16),weights=psp_read32(animation+160),poses=psp_read32(animation+176);
 if(!valid(curve,20)||!valid(parameters,24)||!valid(weights,16)||!valid(poses,4))return 1;
 uint32_t pose=psp_read32(poses);if(!valid(pose,816))return 1;
 /* This sampler also supports pose-local transitions and nested poses.
  * Those are separate contracts; keep their cached camera view unchanged. */
 if(psp_read_f32(pose+720)!=0||psp_read32(pose+768)||psp_read32(pose+776)||psp_read32(pose+812))return 1;
 /* The captured single-clip source has no additional blend controls. */
 for(unsigned i=0;i<4;i++)if(psp_read_f32(weights+4*i)!=0)return 1;
 float rate=psp_read_f32(animation+140),speed=psp_read_f32(parameters+16),duration=psp_read_f32(curve+16),current=psp_read_f32(state);
 if(!isfinite(rate)||rate<=0||!isfinite(speed)||speed<=0||!isfinite(duration)||duration<=0||!isfinite(current)||current<0)return 1;
 float step=(c->delta[0]*speed)*rate,next=current+step;
 if(!isfinite(next)||next>=duration)return 1;
 float times[4];for(unsigned i=0;i<4;i++){times[i]=psp_read_f32(state+12*i);if(!isfinite(times[i])||times[i]<0)return 1;}times[0]=next;
 uint32_t stack=psp_cpu.r[29]-64;if(!valid(stack,64))return 0;
 for(unsigned i=0;i<4;i++)psp_write_f32(stack+32+4*i,times[i]);
 /* Original0883C4C8 evaluates clip keyframes from explicit channel times.
  * Neither the original animation/event update nor its phase/loop counters
  * run here. This temporary pose and every sampler cache write are discarded
  * with the controller calculation; only the descriptor is exported. */
 psp_cpu.r[29]=stack;psp_cpu.r[4]=pose;psp_cpu.r[5]=0;psp_cpu.r[6]=stack+32;psp_cpu.r[7]=weights;psp_cpu.f[12]=c->delta[0];
 psp_call_from_host(0x0883c4c8u);return psp_cpu.r[29]==stack;
}
static int capture(void *user){
 camera_capture *c=user;asphalt_camera_frame *out=c->frame;
 if(psp_read32(c->child+8)!=c->controller||psp_read32(c->controller+36)!=c->node)return 0;
 /* The original renderer partitions its depth range by changing the camera
  * near plane. Export the fractional pose/FOV in that same render pass.
  * This original pure builder runs inside the existing rollback journal. */
 if(psp_read_f32(c->node+16)!=c->pass_near||psp_read_f32(c->node+20)!=c->pass_far){
  uint32_t stack=psp_cpu.r[29];
  psp_cpu.r[4]=c->node;psp_cpu.f[12]=psp_read_f32(c->node+24);psp_cpu.f[13]=psp_read_f32(c->node+28);
  psp_cpu.f[14]=c->pass_near;psp_cpu.f[15]=c->pass_far;psp_call_from_host(0x088a52c0u);
  if(psp_cpu.r[29]!=stack)return 0;
 }
 out->descriptor=c->node;out->ranges[0]=(asphalt_camera_range){.address=c->node,.size=64};
 memcpy(out->ranges[0].bytes,psp_mem_ptr(c->node,64),64);
 /* The renderer load hook forces a refresh of this temporary camera. Keep
  * the original ID, so the next committed update cannot collide with it. */
 memcpy(out->ranges[0].bytes+48,&c->original_id,4);
 for(unsigned i=0;i<4;i++){
  unsigned size=i?64:32;uint32_t p=psp_read32(c->node+4*i);
  if(p!=c->pointers[i]||!valid(p,size))return 0;
  out->ranges[i+1]=(asphalt_camera_range){.address=p,.size=size};memcpy(out->ranges[i+1].bytes,psp_mem_ptr(p,size),size);
 }
 out->rear_pose=(asphalt_camera_range){0};
 /* Verified normal-camera calculator: the radar reads this source pose,
  * rather than the renderer's separately built world matrix. */
 uint32_t calc=c->rear_calculator,source=c->rear_source;
 if(calc&&source&&psp_read32(c->controller+52)==calc&&valid(calc,28)&&psp_read32(calc)==0x08b3b004u&&psp_read32(calc+24)==source&&valid(source,96)&&psp_read32(source)==0x08b3aed8u&&psp_read32(source+12)==1){
  int finite=1;for(unsigned i=0;i<16;i++)if(!isfinite(psp_read_f32(source+32+4*i)))finite=0;
  if(finite){out->rear_pose=(asphalt_camera_range){.address=source+32,.size=64};memcpy(out->rear_pose.bytes,psp_mem_ptr(source+32,64),64);}
 }
 return !c->extra_capture||c->extra_capture(c->extra_user);
}
int asphalt_native_camera_calculate_prepared(uint32_t child,uint32_t environment,const float delta[3],asphalt_preview_capture prepare,asphalt_preview_capture extra_capture,void *extra_user,asphalt_camera_frame *out){
 if(!out||!delta||!valid(child,104))return 0;
 uint32_t vt=psp_read32(child);if(!valid(vt,16)||psp_read32(vt+12)!=0x08a352e0u)return 0;
 uint32_t controller=psp_read32(child+8);if(!valid(controller,56))return 0;
 vt=psp_read32(controller);if(!valid(vt,20)||psp_read32(vt+16)!=0x08a1fce4u)return 0;
 uint32_t node=psp_read32(controller+36);if(!valid(node,64))return 0;
 camera_capture c={.child=child,.controller=controller,.node=node,.original_id=psp_read32(node+48),.frame=out,.extra_prepare=prepare,.extra_capture=extra_capture,.extra_user=extra_user};
 uint32_t calc=psp_read32(controller+52);
 if(valid(calc,28)&&psp_read32(calc)==0x08b3b004u){uint32_t source=psp_read32(calc+24);if(valid(source,96)&&psp_read32(source)==0x08b3aed8u&&psp_read32(source+12)==1){c.rear_calculator=calc;c.rear_source=source;}}
 c.pass_near=psp_read_f32(node+16);c.pass_far=psp_read_f32(node+20);
 if(!isfinite(c.pass_near)||!isfinite(c.pass_far)||c.pass_near<=0||c.pass_far<=c.pass_near)return 0;
 memcpy(c.delta,delta,sizeof c.delta);
 for(unsigned i=0;i<4;i++){c.pointers[i]=psp_read32(node+4*i);if(!valid(c.pointers[i],i?64:32))return 0;}
 int ok=asphalt_preview_call_prepared(0x089e8684u,child,environment,delta,source_animation,capture,&c);
 if(getenv("ASPHALT_NATIVE_TRACE")){static unsigned printed;if(printed++<4)fprintf(stderr,"camera-calculation: child=%08X controller=%08X node=%08X delta=%g ok=%d\n",child,controller,node,delta[0],ok);}return ok;
}

int asphalt_native_camera_calculate(uint32_t child,uint32_t environment,const float delta[3],asphalt_camera_frame *out){return asphalt_native_camera_calculate_prepared(child,environment,delta,NULL,NULL,NULL,out);}

/* Pure projection rebuild from one copied sampled view; no second physical
 * or camera-controller calculation. The existing rollback budgets apply. */
typedef struct {const asphalt_camera_frame *source;camera_capture camera;} projection_capture;
static int prepare_projection(void *user){
 projection_capture *p=user;
 for(unsigned i=0;i<5;i++){const asphalt_camera_range *r=&p->source->ranges[i];if(!valid(r->address,r->size)||psp_mem_write_block(r->address,r->bytes,r->size))return 0;}
 return 1;
}
static int capture_projection_frame(void *user){projection_capture *p=user;return capture(&p->camera);}
int asphalt_native_camera_project(const asphalt_camera_frame *source,uint32_t child,float near_plane,float far_plane,asphalt_camera_frame *out){
 if(!source||!out||!valid(child,12)||!isfinite(near_plane)||!isfinite(far_plane)||near_plane<=0||far_plane<=near_plane)return 0;
 uint32_t controller=psp_read32(child+8),node=source->descriptor;if(!valid(controller,56)||psp_read32(controller+36)!=node||!valid(node,64))return 0;
 projection_capture p={.source=source,.camera={.child=child,.controller=controller,.node=node,.original_id=psp_read32(node+48),.pass_near=near_plane,.pass_far=far_plane,.frame=out}};
 for(unsigned i=0;i<4;i++){p.camera.pointers[i]=source->ranges[i+1].address;if(psp_read32(node+i*4)!=p.camera.pointers[i])return 0;}
 psp_cpu_state saved=psp_cpu;memcpy(&psp_cpu.f[12],source->ranges[0].bytes+24,4);memcpy(&psp_cpu.f[13],source->ranges[0].bytes+28,4);psp_cpu.f[14]=near_plane;psp_cpu.f[15]=far_plane;
 float zero[3]={0};int ok=asphalt_preview_call_prepared(0x088a52c0u,node,0,zero,prepare_projection,capture_projection_frame,&p);psp_cpu=saved;
 if(ok)out->rear_pose=source->rear_pose;return ok;
}

