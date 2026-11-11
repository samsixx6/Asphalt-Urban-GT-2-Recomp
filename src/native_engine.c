#include "native_engine.h"
#include "native_packets.h"
#include "native_camera.h"
#include "native_preview.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/vfpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One CPU owner, one race. The schema contains identities, not borrowed host
 * pointers. Released identities invalidate the generation before guest free. */
typedef struct {uint32_t pc,object,mode,record;float rate;unsigned active;} task;
typedef struct {
 uint32_t owner,vehicle,physics,render,world,manager,scene;
 task root[8],car[15],physical[2],draw[8];
} schema;
static schema bound;
static asphalt_native_packets packets;
static asphalt_native_engine_stats counts;
static asphalt_native_task_observer observer;
static int configured,enabled,in_root,manual_depth,stage,split_packet,finished;
static int requested_rate=-1,applying_rate;
static uint32_t coarse_tick,packet_tick,forced_before;
static float coarse_delta,packet_previous,native_dt[3],root_dt[3];
static unsigned slots;
static int in_group;
static uint32_t deferred_owner,deferred_tick;static float deferred_delta;

/* Observations own only copied bytes and guest identity values. At most one
 * selected body/view source is compared per root, never a mesh or whole scene.
 * Neither stream is claimed to be the player's car or the primary camera. */
typedef struct {
 uint32_t id,source,parent,storage,auxiliary;unsigned have,size;
 uint64_t root,samples,comparisons,changes,selections;unsigned char bytes[64];
 const char *phase;
} observed_source;
static struct {
 uint64_t roots,root_schema_attempts,root_rejections,schema_attempts,schema_rejections,camera_attempts,camera_applied,camera_refused;
 uint32_t owner,last_reject_owner,last_reject_line,last_schema_line,last_schema_owner,last_camera_line;
 uint64_t last_reject_root,last_schema_root;
 const char *last_reject_reason,*last_schema_phase,*last_camera_reason;
 observed_source model,view;
} observation;
/* Track coherent render sources until their next original publication. */
typedef struct {uint32_t part,vt,tag,render[2];unsigned char cache[48];} recovery_part;
typedef struct {uint32_t actor,body,parts,count,vt,ground,wheels[4];unsigned first,n,valid;unsigned char before[48],pose[48],spatial[128];} recovery_actor;
static struct {uint32_t owner,world,list,count,members[32];uint64_t ready_root;unsigned ready,pending,actors_n,parts_n;recovery_actor actors[32];recovery_part parts[256];} recovery;
static void recovery_reset(void);
static void recovery_posted(int ready);
static void recovery_begin(task t);
static void recovery_end(int ok);
static void recovery_release(uint32_t object);
static uint32_t recovery_render_part(uint32_t part,uint32_t caller);
static int recovery_model_ready(uint32_t matrix);
static int recovery_anchor_base(uint32_t actor,uint32_t matrix);
static void recovery_pending_validate(void);

/* Copied cinematic render state, owned by the CPU thread. */
typedef struct {uint32_t words[32];unsigned count;} event_signature;
typedef struct {uint32_t actor,parts,body,count,wheels[4],ground,publish_body;unsigned char pose[48],baseline[128],spatial[12];} event_actor;
typedef struct {uint32_t part,actor,parts,body,tag;unsigned char baseline[48],pose[48];} event_part;
static struct {
 event_signature epoch;unsigned epoch_valid;float packet_scale;
 uint64_t root,attempts;unsigned attempted,valid,coarse;
 uint32_t tick,epoch_actor,epoch_body,epoch_parts;float delta[3];asphalt_camera_frame camera;
 event_actor actors[32];event_part parts[256];unsigned actors_n,parts_n;
} event_frame;
static void event_reset(void);
static void event_root(void);
static void event_commit(void);
static void event_release(uint32_t object);
static int event_render_camera(uint32_t descriptor);
static uint32_t event_render_part(uint32_t part,uint32_t caller);
static int event_validate(void);
static int event_visual_ready(void);
static int event_rear_prepare(void);

static unsigned inspect_rejection_line;
static const char *inspect_phase;
static void retire_source(observed_source *s){s->id=s->source=s->parent=s->storage=s->auxiliary=s->have=0;s->root=0;s->phase=NULL;}
static void retire_sources(void){retire_source(&observation.model);retire_source(&observation.view);}
static void rejection(const char *reason,unsigned line){
 observation.last_reject_reason=reason;observation.last_reject_line=line;
 observation.last_reject_owner=observation.owner;observation.last_reject_root=observation.roots;
}

/* Every model load has a marker, including refused and nested loads. Only
 * the accepted scope owns bytes; its matching leave restores them at once. */
enum { MODEL_SCOPE_LIMIT=16 };
typedef struct {uint32_t matrix;unsigned char bytes[64];} model_scope;
static model_scope model_scopes[MODEL_SCOPE_LIMIT];static unsigned model_depth;
static void restore_models(void){
 unsigned n=model_depth<MODEL_SCOPE_LIMIT?model_depth:MODEL_SCOPE_LIMIT;
 while(n){model_scope *s=&model_scopes[--n];if(s->matrix){void *p=psp_mem_ptr(s->matrix,64);if(p)memcpy(p,s->bytes,64);s->matrix=0;}}
 model_depth=0;
}

enum { REAR_LIMIT=12,REAR_SCOPE_LIMIT=16 };
typedef struct {uint32_t quad,id,spatial[10],authored[4],uv[4],colour,alpha,resource;} rear_item;
typedef struct {unsigned n;rear_item items[REAR_LIMIT];} rear_scope;
static rear_scope rear_scopes[REAR_SCOPE_LIMIT];static unsigned rear_depth;
static struct {uint64_t frame;uint32_t radar;unsigned attempted,valid,n;rear_item input[REAR_LIMIT],output[REAR_LIMIT];} rear_cache;
static asphalt_camera_range rear_camera_pose;
static void restore_rears(void){
 unsigned n=rear_depth<REAR_SCOPE_LIMIT?rear_depth:REAR_SCOPE_LIMIT;
 while(n){rear_scope *scope=&rear_scopes[--n];for(unsigned i=0;i<scope->n;i++){rear_item *p=&scope->items[i];void *a=psp_mem_ptr(p->quad+40,8),*b=psp_mem_ptr(p->quad+64,32);if(a&&b){memcpy(a,p->spatial,8);memcpy(b,p->spatial+2,32);}}scope->n=0;}
 rear_depth=0;rear_cache.valid=0;
}
typedef struct {uint32_t address;unsigned size;unsigned char bytes[64];} saved_pose;
static saved_pose pose_backup[320];static unsigned pose_backups;
static uint32_t camera_overlay;
static void restore_poses(void){
 restore_rears();restore_models();
 for(unsigned i=0;i<pose_backups;i++)memcpy(psp_mem_ptr(pose_backup[i].address,pose_backup[i].size),pose_backup[i].bytes,pose_backup[i].size);
 pose_backups=0;camera_overlay=0;rear_camera_pose=(asphalt_camera_range){0};
}
static int save_range(uint32_t address,unsigned size){
 if(!size||size>64||!psp_mem_ptr(address,size))return 0;
 for(unsigned i=0;i<pose_backups;i++)if(pose_backup[i].address==address)return pose_backup[i].size>=size;
 if(pose_backups==320)return 0;
 saved_pose *p=&pose_backup[pose_backups++];p->address=address;p->size=size;memcpy(p->bytes,psp_mem_ptr(address,size),size);return 1;
}
static int save_pose(uint32_t address){return save_range(address,64);}
static int memory(uint32_t p,unsigned n){return p>=0x08800000u&&p<=0x0a000000u-n&&psp_mem_ptr(p,n)!=NULL;}
static void observe_source(observed_source *s,uint32_t id,uint32_t source,unsigned size,uint32_t parent,uint32_t storage,uint32_t auxiliary,const char *phase){
 if(!in_root||!id||!size||size>sizeof s->bytes||!memory(source,size))return;
 if(!s->id){s->id=id;s->parent=parent;s->storage=storage;s->auxiliary=auxiliary;s->size=size;s->selections++;}
 if(s->id!=id||s->size!=size||s->root==observation.roots)return;
 const void *p=psp_mem_ptr(source,size);s->root=observation.roots;s->source=source;s->phase=phase;s->samples++;
 if(s->have){s->comparisons++;if(memcmp(s->bytes,p,size))s->changes++;}
 memcpy(s->bytes,p,size);s->have=1;
}
static void observe_committed_body(uint32_t part){
 /* This exact original getter returns part+88. A selected native-bound body
  * keeps the same part identity when the coarse cache is consumed next. */
 if(observation.model.id==part&&memory(part,184)&&psp_read32(part+136)==37)
  observe_source(&observation.model,part,part+88,48,0,0,0,"committed_cache");
}
static void observe_view(uint32_t renderer,uint32_t descriptor){
 if(!in_root||!memory(descriptor,64))return;
 if(!observation.view.id){
  /* Select the camera owned by the bound controller, rather than whichever
   * auxiliary loader happens to render first. Unsupported contracts stay
   * unsampled and are visible in the refresh outcome counters. */
  uint32_t manager=bound.car[11].object;
  if(!bound.owner||!memory(manager,28)||psp_read32(manager+24)!=1)return;
  uint32_t list=psp_read32(manager+20);if(!memory(list,4))return;
  uint32_t child=psp_read32(list);if(!memory(child,12))return;
  uint32_t controller=psp_read32(child+8);
  if(!memory(controller,40)||psp_read32(controller+36)!=descriptor)return;
  observe_source(&observation.view,descriptor,psp_read32(descriptor+12),64,controller,child,renderer,camera_overlay==descriptor?"temporary_camera":"committed_camera");
 }else if(observation.view.id==descriptor)
  observe_source(&observation.view,descriptor,psp_read32(descriptor+12),64,0,0,0,camera_overlay==descriptor?"temporary_camera":"committed_camera");
}
static uint32_t method(uint32_t object){
 if(!memory(object,4))return 0;uint32_t vt=psp_read32(object);
 return memory(vt,20)?psp_read32(vt+16):0;
}
static int unsupported(unsigned line){
 if(!inspect_rejection_line){
  inspect_rejection_line=line;observation.schema_rejections++;rejection("schema_contract",line);
  observation.last_schema_line=line;observation.last_schema_phase=inspect_phase;
  observation.last_schema_root=observation.roots;observation.last_schema_owner=observation.owner;
 }
 static unsigned last;if(getenv("ASPHALT_NATIVE_TRACE")&&line!=last){fprintf(stderr,"native60: unsupported task contract at line %u\n",line);last=line;}return 0;
}
static int group(uint32_t at,task *out,unsigned n,int timing,int reduced){
 if(!memory(at,68)||method(at)!=0x08879d38u)return unsupported(__LINE__);
 uint32_t d=psp_read32(at+16);if(!memory(d,8)||psp_read32(d+4)!=n)return unsupported(__LINE__);
 uint32_t list=psp_read32(d);if(!memory(list,n*24))return unsupported(__LINE__);
 if(timing){
  float scale=psp_read_f32(at+52);
  if(psp_read8(at+56)||psp_read8(at+57)||!isfinite(scale)||scale<=0||scale>1||(!reduced&&scale!=1))return unsupported(__LINE__);
  float limit=psp_read_f32(at+48);if(limit!=(n==15?.079999f:1.0f))return unsupported(__LINE__);
  uint32_t dynamic=psp_read32(at+28);if(dynamic&&(!memory(dynamic,1)||psp_read8(dynamic)))return unsupported(__LINE__);
 }
 for(unsigned i=0;i<n;i++){
  uint32_t r=list+24*i;out[i]=(task){.pc=method(psp_read32(r+20)),.object=psp_read32(r+20),.mode=psp_read32(r+16),.record=r,.rate=psp_read_f32(r+12),.active=psp_read8(r)==1};
  if(!out[i].pc||!isfinite(out[i].rate))return unsupported(__LINE__);
 }
 return 1;
}
static int inspect(uint32_t owner,schema *s,int timing,const char *phase){
 observation.schema_attempts++;inspect_rejection_line=0;inspect_phase=phase;
 static const uint32_t root_pc[8]={0x0887a448,0x08879d38,0x088e4fa8,0x0896ba24,0x089573b0,0x088f632c,0x08879d38,0x08879d38};
 static const uint32_t car_pc[15]={0x089cb930,0x0897bc60,0x0892082c,0x0883ad34,0x08879d38,0x0892082c,0x0883ad34,0x089794c0,0x08a2ef74,0x0885d02c,0x089a4028,0x08a34640,0x089ba7f0,0x088c7688,0x089d2430};
 memset(s,0,sizeof *s);s->owner=owner;
 if(!memory(owner,120)||!group(owner+32,s->root,8,timing,0))return unsupported(__LINE__);
 for(unsigned i=0;i<8;i++)if(s->root[i].pc!=root_pc[i]||s->root[i].mode||s->root[i].rate!=-1||s->root[i].active!=(i!=1))return unsupported(__LINE__);
 s->vehicle=s->root[6].object;s->render=s->root[7].object;
 if(!group(s->vehicle,s->car,15,timing,1)||!group(s->render,s->draw,8,timing,1))return unsupported(__LINE__);
 for(unsigned i=0;i<15;i++)if(!s->car[i].active||s->car[i].pc!=car_pc[i]||s->car[i].mode!=((i==5||i==6)?2u:0u)||s->car[i].rate!=(i==4?50.0f:-1.0f))return unsupported(__LINE__);
 s->physics=s->car[4].object;s->world=s->car[2].object;s->manager=s->car[3].object;s->scene=s->car[13].object;
 if(s->car[5].object!=s->world||s->car[6].object!=s->manager||!group(s->physics,s->physical,2,timing,0))return unsupported(__LINE__);
 for(unsigned i=0;i<2;i++)if(!s->physical[i].active||s->physical[i].pc!=(i?0x0883ad34u:0x0892082cu)||s->physical[i].object!=(i?s->manager:s->world)||s->physical[i].mode!=1||s->physical[i].rate!=-1)return unsupported(__LINE__);
 for(unsigned i=0;i<8;i++){
  task *t=&s->draw[i];uint32_t pc=(i==0||i==7)?0x088dd2fc:i==6?0x08955a0c:0x088c7688;
  if(t->pc!=pc||t->active!=(i!=6)||t->mode!=(i==0?0:i>=6?1:i)||t->rate!=-1)return unsupported(__LINE__);
  if(i>=1&&i<=5&&t->object!=s->scene)return unsupported(__LINE__);
 }
 if(s->draw[0].object!=s->draw[7].object)return unsupported(__LINE__);
 return 1;
}
static int identities(void){
 schema current;if(!bound.owner||!inspect(bound.owner,&current,0,"identity_check"))return 0;
 if(memcmp(&current,&bound,sizeof current)){rejection("identity_changed",__LINE__);return 0;}return 1;
}
static int alive_group(uint32_t address,const task *items,unsigned n){
 if(!memory(address,68)||method(address)!=0x08879d38u)return 0;
 uint32_t d=psp_read32(address+16);if(!memory(d,8)||psp_read32(d)!=items[0].record||psp_read32(d+4)!=n)return 0;
 for(unsigned i=0;i<n;i++){
  const task *t=&items[i];if(!memory(t->record,24)||psp_read32(t->record+20)!=t->object||psp_read32(t->record+16)!=t->mode||(psp_read8(t->record)==1)!=t->active||method(t->object)!=t->pc)return 0;
 }return 1;
}
static int alive(void){return bound.owner&&memory(bound.owner,120)&&alive_group(bound.owner+32,bound.root,8)&&alive_group(bound.vehicle,bound.car,15)&&alive_group(bound.physics,bound.physical,2)&&alive_group(bound.render,bound.draw,8);}
/* Original mode1 cinematic consumes its previous published camera position.
 * Keep that render feedback at the matching fractional phase; the original
 * camera computes final matrices inside the existing rollback scope. */
typedef struct {
 uint32_t owner,world,vehicle,render,manager,list,child,child_vt,controller,controller_vt;
 uint32_t cinematic,descriptor,source,buffers[4],main,blend,wrapper[2],actor[2],body[2],parts[2],wrapper_vt[2],actor_mode[2];
 uint32_t members,count,vehicle_scale,render_scale,environment,follow,cin_configuration,cin_stage;
} feedback_key;
static struct {feedback_key key;uint64_t generation,root;uint32_t packet;unsigned armed,valid;float before[3],after[3],interval;unsigned char committed[64];} feedback;
static void feedback_reset(void){memset(&feedback,0,sizeof feedback);}
static int feedback_mode1(void){
 uint32_t manager=bound.car[11].object;if(!memory(manager,28)||psp_read32(manager+24)!=1)return 0;
 uint32_t list=psp_read32(manager+20);if(!memory(list,4))return 0;uint32_t child=psp_read32(list);if(!memory(child,12))return 0;
 uint32_t ctrl=psp_read32(child+8);if(!memory(ctrl,180))return 0;uint32_t cin=psp_read32(ctrl+176);
 return memory(cin,560)&&psp_read32(cin)==0x08b3b05cu&&psp_read32(cin+4)==9&&psp_read32(cin+68)==1;
}
static int feedback_read(feedback_key *key){
 if(!configured||!enabled||!in_root||!bound.owner||!alive()||!feedback_mode1())return 0;
 memset(key,0,sizeof *key);key->owner=bound.owner;key->world=bound.world;key->vehicle=bound.vehicle;key->render=bound.render;
 key->manager=bound.car[11].object;key->list=psp_read32(key->manager+20);key->child=psp_read32(key->list);
 key->child_vt=psp_read32(key->child);if(key->child_vt!=0x08b3b164u||!memory(key->child_vt,16)||psp_read32(key->child_vt+12)!=0x08a352e0u)return 0;
 key->controller=psp_read32(key->child+8);if(!memory(key->controller,220))return 0;
 key->controller_vt=psp_read32(key->controller);if(key->controller_vt!=0x08b3ae98u||!memory(key->controller_vt,20)||psp_read32(key->controller_vt+16)!=0x08a1fce4u)return 0;
 key->cinematic=psp_read32(key->controller+176);key->blend=psp_read32(key->controller+68);if(key->blend!=key->cinematic)return 0;
 for(unsigned i=0;i<3;i++){float w=psp_read_f32(key->controller+24+4*i);if(!isfinite(w)||w<0||w>1)return 0;}
 key->environment=psp_read32(key->manager+16);key->cin_configuration=psp_read32(key->cinematic+84);key->cin_stage=psp_read32(key->cinematic+88);
 key->main=psp_read32(key->controller+52);key->descriptor=psp_read32(key->controller+36);if(!memory(key->descriptor,64)||!memory(key->main,28))return 0;
 if(psp_read32(key->main)!=0x08b3b004u||psp_read32(key->main+4))return 0;key->follow=psp_read32(key->main+24);if(!memory(key->follow,96)||psp_read32(key->follow)!=0x08b3aed8u||psp_read32(key->follow+12)!=1)return 0;
 for(unsigned i=0;i<4;i++){key->buffers[i]=psp_read32(key->descriptor+4*i);if(!memory(key->buffers[i],i?64:32))return 0;}
 key->source=psp_read32(key->cinematic+60);if(key->source!=key->buffers[3]||!memory(key->source,64))return 0;
 key->members=psp_read32(bound.world+0x289dc);key->count=psp_read32(bound.world+0x289e0);if(!key->count||key->count>32||!memory(key->members,key->count*4))return 0;
 for(unsigned i=0;i<2;i++){
  key->wrapper[i]=psp_read32(key->cinematic+72+4*i);if(!memory(key->wrapper[i],1120))return 0;
  key->actor[i]=psp_read32(key->wrapper[i]+1116);if(!memory(key->actor[i],1260)||psp_read32(key->cinematic+120+4*i)!=key->actor[i]+192)return 0;
  unsigned found=0;for(unsigned j=0;j<key->count;j++)if(psp_read32(key->members+4*j)==key->actor[i])found++;if(found!=1)return 0;
  key->body[i]=psp_read32(key->actor[i]+1256);key->parts[i]=psp_read32(key->actor[i]+552);key->wrapper_vt[i]=psp_read32(key->wrapper[i]);key->actor_mode[i]=psp_read32(key->actor[i]+376);
  if(!memory(key->body[i],56)||!memory(key->parts[i],184))return 0;
 }
 key->vehicle_scale=psp_read32(bound.vehicle+52);key->render_scale=psp_read32(bound.render+52);
 float scale=psp_read_f32(bound.vehicle+52);if(!isfinite(scale)||scale<=0||scale>1||key->vehicle_scale!=key->render_scale)return 0;
 for(unsigned i=0;i<16;i++)if(!isfinite(psp_read_f32(key->source+4*i)))return 0;
 return 1;
}
static int feedback_validate(void){
 feedback_key now;if(!feedback.valid)return 0;
 if(!feedback_read(&now)||memcmp(&now,&feedback.key,sizeof now)||memcmp(psp_mem_ptr(now.source,64),feedback.committed,64)){feedback.valid=feedback.armed=0;return 0;}
 return 1;
}
static void feedback_begin(task t){
 if(asphalt_preview_active())return;feedback.armed=feedback.valid=0;
 if(camera_overlay||pose_backups||t.pc!=0x08a34640u||t.object!=bound.car[11].object||!feedback_read(&feedback.key))return;
 for(unsigned i=0;i<3;i++)feedback.before[i]=psp_read_f32(feedback.key.source+48+4*i);feedback.armed=1;
}
static void feedback_end(int ok,const float delta[3]){
 feedback_key now;if(asphalt_preview_active()||!feedback.armed)return;feedback.armed=0;
 if(!ok||!feedback_read(&now)||memcmp(&now,&feedback.key,sizeof now)||!isfinite(delta[1])||delta[1]<=0)return;
 feedback.generation++;feedback.root=observation.roots;feedback.packet=packet_tick;feedback.interval=delta[1];memcpy(feedback.committed,psp_mem_ptr(now.source,64),64);
 for(unsigned i=0;i<3;i++)feedback.after[i]=psp_read_f32(now.source+48+4*i);feedback.valid=1;
}
static int feedback_prepare(void *user){
 if(!feedback_mode1()){feedback.valid=feedback.armed=0;return 1;}
 if(!feedback_validate())return 0;float phase=*(float*)user;
 if(!isfinite(phase)||phase<0||phase>1)return 0;
 /* Original08A26704..08A267C8 consumes this exact aliased source translation.
  * Supply its preceding-publication phase inside the existing rollback scope. */
 float sampled[3];for(unsigned i=0;i<3;i++){sampled[i]=feedback.before[i]+(feedback.after[i]-feedback.before[i])*phase;if(!isfinite(sampled[i]))return 0;}
 for(unsigned i=0;i<3;i++)psp_write_f32(feedback.key.source+48+4*i,sampled[i]);
 return 1;
}
static void feedback_release(uint32_t object){
 if(!object||(!feedback.valid&&!feedback.armed))return;
 const feedback_key *k=&feedback.key;
 if(object==k->owner||object==k->world||object==k->vehicle||object==k->render||object==k->manager||object==k->list||object==k->child||object==k->controller||object==k->cinematic||object==k->descriptor||object==k->source||object==k->main||object==k->follow||object==k->environment||object==k->members){feedback_reset();return;}
 for(unsigned i=0;i<4;i++)if(object==k->buffers[i]){feedback_reset();return;}
 for(unsigned i=0;i<2;i++)if(object==k->wrapper[i]||object==k->actor[i]||object==k->body[i]||object==k->parts[i]){feedback_reset();return;}
}
static void forget(void){
 feedback_reset();
 recovery_reset();
 event_reset();
 retire_sources();
 restore_poses();counts.resets++;memset(&bound,0,sizeof bound);asphalt_native_packets_reset(&packets);
 enabled=stage=finished=slots=0;
}
static void notify(uint32_t pc,uint32_t object,uint32_t delta,uint32_t mode){if(observer)observer(pc,object,delta,mode);}
/* Host calls do not preserve guest registers. Reserve an ABI-safe guest frame
 * for each immutable vector, and restore the complete CPU state afterwards. */
static int invoke(task t,const float dt[3],int coarse){
 if(!bound.owner||!memory(t.object,4)||method(t.object)!=(t.pc==0x0884f528u?0x0884f0b8u:t.pc))return -1;
 psp_cpu_state saved=psp_cpu;uint32_t stack=saved.r[29]-48;
 if(!memory(stack,48))return -1;
 uint32_t owner=bound.owner,old_tick=psp_read32(owner),old_delta=psp_read32(owner+12);
 if(coarse){psp_write32(owner,packet_tick);psp_write_f32(owner+12,packet_previous);}
 memcpy(psp_mem_ptr(stack+32,12),dt,12);psp_cpu.r[29]=stack;psp_cpu.r[4]=t.object;psp_cpu.r[5]=stack+32;psp_cpu.r[6]=t.mode;
 int feedback_call=coarse&&t.pc==0x08a34640u&&t.object==bound.car[11].object;if(feedback_call)feedback_begin(t);
 manual_depth++;notify(t.pc,t.object,stack+32,t.mode);psp_call_from_host(t.pc);manual_depth--;
 int ok=psp_cpu.r[29]==stack;psp_cpu=saved;if(feedback_call)feedback_end(ok,dt);
 /* A release hook may have invalidated the owner while the callback ran. */
 if(coarse&&bound.owner==owner){psp_write32(owner,old_tick);psp_write32(owner+12,old_delta);}
 if(!ok){fprintf(stderr,"native60: original call stack mismatch at %08X\n",t.pc);return -1;}
 return bound.owner?0:-1;
}
static int emit(void *unused,int kind,const float dt[3]){
 (void)unused;
 if(!alive())return -1;
 if(kind==ASPHALT_PACKET_PRE)return invoke(bound.car[2],dt,1)||invoke(bound.car[3],dt,1);
 if(kind==ASPHALT_PACKET_POST){recovery_posted(0);int failed=invoke(bound.car[5],dt,1)||invoke(bound.car[6],dt,1);recovery_posted(!failed);return failed;}
 counts.steps++;return invoke(bound.car[4],dt,1);
}
static int suffix(void){
 for(unsigned i=7;i<13;i++){if(i==7)recovery_begin(bound.car[i]);int failed=invoke(bound.car[i],packets.sim,1);if(i==7)recovery_end(!failed);if(failed)return -1;}
 return 0;
}
static int finish_tail(int after_scene){
 if(packets.pending){
  if(asphalt_native_packets_begin(&packets,NULL)!=ASPHALT_PACKET_FINISH||asphalt_native_packets_run(&packets,NULL,50,1,emit,NULL)||suffix())return -1;
  finished=1;
 }
 if(after_scene&&finished){finished=0;return invoke(bound.car[14],packets.sim,1);}
 return 0;
}
static void restore_coarse_clock(void){
 if(bound.owner&&memory(bound.owner,120)){
  if(psp_read_f32(bound.owner+4)==.015f)psp_write_f32(bound.owner+4,.03f);
  psp_write32(bound.owner,coarse_tick);psp_write_f32(bound.owner+12,coarse_delta);
  if(in_group){deferred_owner=bound.owner;deferred_tick=coarse_tick;deferred_delta=coarse_delta;}
 }
}
static void apply_requested_rate(void){
 if(in_root||manual_depth||applying_rate)return;
 applying_rate=1;
 while(requested_rate>=0){
  int next=requested_rate;requested_rate=-1;
  if(next==configured)continue;
  if(!next&&bound.owner){
   /* Complete the immutable old packet before returning its clock to the
    * original root. Each original callback retains invoke/emit's release
    * guards: a freed generation must never be called or written again. */
   if(alive()){if(finish_tail(1))counts.fallbacks++;}
   else counts.fallbacks++;
   /* Like root_enter, keep a zero-interval loading/clock-reset epoch that
    * the original established while this request was waiting. */
   if(bound.owner&&memory(bound.owner,120)&&psp_read_f32(bound.owner+4)!=0)restore_coarse_clock();
   forget();
  }
  configured=next;
 }
 applying_rate=0;
}
int asphalt_native_engine_request_rate(int fps){
 if(fps!=30&&fps!=60)return -1;
 feedback_reset();requested_rate=fps==60;apply_requested_rate();return 0;
}
void asphalt_native_engine_configure(int fps){
 feedback_reset();
 recovery_reset();
 event_reset();
 restore_rears();memset(&rear_cache,0,sizeof rear_cache);rear_camera_pose=(asphalt_camera_range){0};restore_models();configured=fps==60;pose_backups=0;camera_overlay=0;memset(&bound,0,sizeof bound);asphalt_native_packets_reset(&packets);memset(&counts,0,sizeof counts);
 enabled=in_root=in_group=manual_depth=stage=finished=slots=0;deferred_owner=0;requested_rate=-1;applying_rate=0;
 memset(&observation,0,sizeof observation);inspect_rejection_line=0;
}
void asphalt_native_engine_observe(asphalt_native_task_observer fn){observer=fn;}
asphalt_native_engine_stats asphalt_native_engine_counters(void){return counts;}
void asphalt_native_engine_report(FILE *out){
 if(!out)return;
 const observed_source *m=&observation.model,*v=&observation.view;
 fprintf(out,"native-cadence: {\"schema\":1,\"cumulative\":true,\"configured_fps\":%d,\"native_active\":%d,\"root_owner\":%u,\"bound_owner\":%u,"
  "\"root_attempts\":%llu,\"root_schema_attempts\":%llu,\"root_rejections\":%llu,\"schema_attempts\":%llu,\"schema_rejections\":%llu,"
  "\"last_reject_reason\":\"%s\",\"last_reject_line\":%u,\"last_reject_owner\":%u,\"last_reject_root\":%llu,"
  "\"last_schema_phase\":\"%s\",\"last_schema_line\":%u,\"last_schema_owner\":%u,\"last_schema_root\":%llu,"
  "\"frames\":%llu,\"packets\":%llu,\"steps\":%llu,\"visuals\":%llu,\"resets\":%llu,\"fallbacks\":%llu,"
  "\"camera_attempts\":%llu,\"camera_applied\":%llu,\"camera_refused\":%llu,\"last_camera_refusal_reason\":\"%s\",\"last_camera_refusal_line\":%u,"
  "\"model_scope\":\"selected_bound_body_getter\",\"model_phase\":\"%s\",\"model_id\":%u,\"model_source\":%u,\"model_samples\":%llu,\"model_comparisons\":%llu,\"model_changes\":%llu,\"model_selections\":%llu,"
  "\"view_scope\":\"selected_camera_loader\",\"view_phase\":\"%s\",\"view_id\":%u,\"view_source\":%u,\"view_samples\":%llu,\"view_comparisons\":%llu,\"view_changes\":%llu,\"view_selections\":%llu}\n",
  configured?60:30,enabled&&bound.owner!=0,observation.owner,bound.owner,
  (unsigned long long)observation.roots,(unsigned long long)observation.root_schema_attempts,(unsigned long long)observation.root_rejections,(unsigned long long)observation.schema_attempts,(unsigned long long)observation.schema_rejections,
  observation.last_reject_reason?observation.last_reject_reason:"none",observation.last_reject_line,observation.last_reject_owner,(unsigned long long)observation.last_reject_root,
  observation.last_schema_phase?observation.last_schema_phase:"none",observation.last_schema_line,observation.last_schema_owner,(unsigned long long)observation.last_schema_root,
  (unsigned long long)counts.frames,(unsigned long long)counts.packets,(unsigned long long)counts.steps,(unsigned long long)counts.visuals,(unsigned long long)counts.resets,(unsigned long long)counts.fallbacks,
  (unsigned long long)observation.camera_attempts,(unsigned long long)observation.camera_applied,(unsigned long long)observation.camera_refused,
  observation.last_camera_reason?observation.last_camera_reason:"none",observation.last_camera_line,
  m->phase?m->phase:"unavailable",m->id,m->source,(unsigned long long)m->samples,(unsigned long long)m->comparisons,(unsigned long long)m->changes,(unsigned long long)m->selections,
  v->phase?v->phase:"unavailable",v->id,v->source,(unsigned long long)v->samples,(unsigned long long)v->comparisons,(unsigned long long)v->changes,(unsigned long long)v->selections);
}
void asphalt_native_engine_root_enter(uint32_t owner){
 observation.roots++;if(observation.owner!=owner)retire_sources();observation.owner=owner;
 restore_poses();event_root();in_root=in_group=0;apply_requested_rate();in_root=1;if(!configured)return;
 observation.root_schema_attempts++;
 schema next;int supported=inspect(owner,&next,1,"root_schema");float minimum=memory(owner,120)?psp_read_f32(owner+4):0;
 if(bound.owner&&(!supported||memcmp(&next,&bound,sizeof next)||(minimum!=.03f&&minimum!=.015f))){
  if(alive()){if(finish_tail(1))counts.fallbacks++;}
  /* A zero interval is an original loading/clock reset. Preserve that new
   * epoch instead of writing the retired race clock back over it. */
  if(minimum!=0)restore_coarse_clock();
  forget();
 }
 if(!supported||(minimum!=.03f&&minimum!=.015f)){
  observation.root_rejections++;if(supported)rejection("minimum_interval",__LINE__);
  counts.fallbacks++;return;
 }
 if(!bound.owner){bound=next;coarse_tick=psp_read32(owner);coarse_delta=psp_read_f32(owner+12);slots=0;}
 psp_write_f32(owner+4,.015f);forced_before=psp_read32(0x08b3e850u);in_root=enabled=1;stage=0;finished=0;if(feedback.valid)feedback_validate();
}
static float original_seconds(uint32_t ticks){
 float f=ticks&0x80000000u?(float)((ticks>>1)|(ticks&1))*2.0f:(float)(int32_t)ticks;
 return psp_read_f32(0x08adaa10u)*f;
}
void asphalt_native_engine_group_enter(uint32_t group_address,uint32_t delta){
 if(!in_root||!enabled||manual_depth||group_address!=bound.owner+32)return;
 in_group=1;
 if(!identities()||!memory(delta,12)){restore_coarse_clock();forget();return;}
 /* The original root consumes its forced-frame counter per outer call.
  * Undo that consumption before any buffered old work, then consume once
  * only when a complete coarse packet starts. Callback changes survive. */
 if((int32_t)forced_before>0&&psp_read32(0x08b3e850u)==forced_before-1)psp_write32(0x08b3e850u,forced_before);
 memcpy(native_dt,psp_mem_ptr(delta,12),12);counts.frames++;slots++;
 uint32_t tick=psp_read32(bound.owner);float elapsed=original_seconds(tick-coarse_tick);
 if(!isfinite(elapsed)||elapsed<0){restore_coarse_clock();forget();return;}
 if(elapsed<.03f)return;
 int late=slots==1||packets.pending;
 /* Flush old post/event work before the next original input callback. */
 if(packets.pending&&finish_tail(1)){restore_coarse_clock();forget();return;}
 packet_tick=tick;packet_previous=coarse_delta;
 uint32_t forced=psp_read32(0x08b3e850u);if((int32_t)forced>0)psp_write32(0x08b3e850u,forced-1);
 float raw[3];for(unsigned i=0;i<3;i++)raw[i]=(int32_t)forced>0?.03f:elapsed;
 if(asphalt_native_packets_begin(&packets,raw)!=ASPHALT_PACKET_START){restore_coarse_clock();forget();return;}
 memcpy(root_dt,raw,12);root_dt[0]=fminf(root_dt[0],1.0f);
 coarse_tick=tick;coarse_delta=raw[0];slots=0;stage=1;split_packet=!late;counts.packets++;
}
/* The original graphics task also advances effect/animation counters. Keep
 * that task coarse. Between packets, refresh only its car pose conversion and
 * the final parent-matrix copy used by ordinary bound car models. These are
 * actual current physics poses, with no interpolation or timer evaluation. */
static void refresh_car_poses(void){
 uint32_t gfx=bound.car[9].object;if(!memory(gfx,1780))return;
 uint32_t list=psp_read32(gfx+1572),n=psp_read32(gfx+1576),world_list=psp_read32(bound.world+0x289dc),world_n=psp_read32(bound.world+0x289e0);
 if(n>32||world_n>128||!memory(list,n*4)||!memory(world_list,world_n*4))return;
 uint32_t outputs[160],components[32];unsigned noutputs=0,ncomponents=0;float zero[3]={0};
 for(unsigned i=0;i<n;i++){
  uint32_t v=psp_read32(list+4*i);if(!memory(v,712)||method(v)!=0x0884f0b8u||!psp_read8(v+8))continue;
  uint32_t owner=psp_read32(v+112);if(!memory(owner,304))continue;
  uint32_t link=psp_read32(owner+300);if(!memory(link,100))continue;
  uint32_t actor=psp_read32(link+96);int found=0;
  for(unsigned j=0;j<world_n;j++)if(psp_read32(world_list+4*j)==actor)found=1;
  if(!found||!memory(actor,1260)||!memory(psp_read32(actor+1256),56))continue;
  int valid=1;for(unsigned k=0;k<4;k++){
   uint32_t out=psp_read32(v+80+4*k),local=psp_read32(v+96+4*k);
   /* Original constructor owns these matrices inside this typed component. */
   if(out&&(out<v+16||out>v+648||!memory(out,64)||local<v+16||local>v+648||!memory(local,64)))valid=0;
  }
  if(!valid)continue;
  if(!save_pose(v+16)){restore_poses();return;}
  for(unsigned k=0;k<4;k++){uint32_t out=psp_read32(v+80+4*k);if(out&&!save_pose(out)){restore_poses();return;}}
  task helper={.pc=0x0884f528u,.object=v};if(invoke(helper,zero,0))return;components[ncomponents++]=v;
  for(unsigned k=0;k<4;k++){uint32_t out=psp_read32(v+80+4*k);if(out)outputs[noutputs++]=out;}
 }
 /* Original08852A84 binds the model component to one of the physical car's
  * matrices. Repeat only that copy; its surrounding update also changes
  * previous-position caches, effects and counters and remains coarse. */
 uint32_t models=psp_read32(gfx+1632);unsigned count=psp_read32(gfx+1636);
 if(count<=32&&memory(models,count*4))for(unsigned i=0;i<count;i++){
  uint32_t v=psp_read32(models+4*i);if(!memory(v,164)||method(v)!=0x0884adc4u||psp_read32(v+156))continue;
  uint32_t node=psp_read32(v+160);if(!memory(node,200))continue;
  uint32_t component=psp_read32(node+196);int found=0;for(unsigned j=0;j<ncomponents;j++)if(component==components[j])found=1;
  int32_t index=(int32_t)psp_read32(node+156);if(!found||index< -1||index>3)continue;
  uint32_t source=index<0?component+16:psp_read32(component+80+4*index);if(!memory(source,64))continue;
  if(!save_pose(v+16)){restore_poses();return;}
  memcpy(psp_mem_ptr(v+16,64),psp_mem_ptr(source,64),64);outputs[noutputs++]=v+16;
 }
 unsigned nodes=psp_read32(gfx+16);if(nodes>128)return;
 for(unsigned i=0;i<nodes;i++){
  uint32_t node=psp_read32(gfx+1060+4*i);if(!memory(node,396))continue;
  uint32_t parent=psp_read32(node+32);int car=0;for(unsigned j=0;j<noutputs;j++)if(outputs[j]==parent)car=1;
  uint32_t kind=psp_read32(node+180);
  if(!car||psp_read32(node+4)==0xffffffffu||(kind!=1&&kind!=2)||(kind==1&&psp_read32(node+148)==2))continue;
  /* Original08842BA4's bound-parent branch copies these exact64 bytes after
   * animation evaluation. Recopy without advancing any animation or event. */
  if(!save_pose(node+48)){restore_poses();return;}
  memcpy(psp_mem_ptr(node+48,64),psp_mem_ptr(parent,64),64);
 }
}
/* The camera controller mixes view calculation with gameplay writes. Run its
 * original calculation on this actual intermediate state in a guarded journal,
 * export only camera data, and discard EVERY other write before rendering. */
static void camera_refused(const char *reason,unsigned line){
 observation.camera_refused++;observation.last_camera_reason=reason;observation.last_camera_line=line;
}
static void refresh_camera(uint32_t expected_descriptor){
 observation.camera_attempts++;
 uint32_t manager=bound.car[11].object;

 if(!memory(manager,28)||psp_read32(manager+24)!=1){camera_refused("manager_contract",__LINE__);return;}
 uint32_t list=psp_read32(manager+20);if(!memory(list,4)||!packets.total){camera_refused("list_or_packet",__LINE__);return;}
 uint32_t child=psp_read32(list);if(!memory(child,12)){camera_refused("child_contract",__LINE__);return;}
 uint32_t controller=psp_read32(child+8);
 if(!memory(controller,40)||psp_read32(controller+36)!=expected_descriptor||!alive()){camera_refused("controller_or_lifetime",__LINE__);return;}
 unsigned first=packets.total-packets.pending;
 float ratio=(float)first/(float)packets.total;
 float dt[3]={packets.step[0]*first,packets.sim[1]*ratio,packets.sim[2]*ratio};
 uint32_t owner=bound.owner,old_tick=psp_read32(owner),old_delta=psp_read32(owner+12);
 psp_write32(owner,packet_tick);psp_write_f32(owner+12,packet_previous);
 asphalt_camera_frame frame;
 int ok=asphalt_native_camera_calculate_prepared(psp_read32(list),psp_read32(manager+16),dt,feedback_prepare,NULL,&ratio,&frame);
 psp_write32(owner,old_tick);psp_write32(owner+12,old_delta);
 if(!ok){camera_refused("calculation",__LINE__);return;}
 /* New York's original camera aliases its 32-byte parameter buffer with the
  * 64-byte view buffer. Journal all full matrices before the shorter range,
  * so the shared address is already protected at its complete size. Keep all
  * backups before publication and the original publication order unchanged. */
 static const unsigned backup_order[5]={0,2,3,4,1};
 for(unsigned i=0;i<5;i++){const asphalt_camera_range *r=&frame.ranges[backup_order[i]];if(!save_range(r->address,r->size)){camera_refused("pose_journal",__LINE__);restore_poses();return;}}
 for(unsigned i=0;i<5;i++)memcpy(psp_mem_ptr(frame.ranges[i].address,frame.ranges[i].size),frame.ranges[i].bytes,frame.ranges[i].size);
 rear_camera_pose=frame.rear_pose;camera_overlay=frame.descriptor;
 observation.camera_applied++;
}
void asphalt_native_engine_camera_enter(uint32_t renderer,uint32_t descriptor){
 if(event_frame.valid)event_validate();
 /* Scene preparation may free unrelated temporary objects and restore pose
  * buffers. Evaluate only when this exact camera is about to be consumed. */
 if(!camera_overlay&&enabled&&in_root){if(packets.pending)refresh_camera(descriptor);else event_render_camera(descriptor);}
 if(camera_overlay&&descriptor==camera_overlay&&memory(renderer,24)&&memory(descriptor,52))
  /* Keep the committed camera ID intact; force this exact temporary view to
   * rebuild through the original loader. The next real ID cannot collide. */
  psp_write32(renderer+20,~psp_read32(descriptor+48));
 observe_view(renderer,descriptor);
}
/* The ordinary car meshes have a separate packed pose cache. Its original
 * publisher is part of coarse world post-processing, while the camera sees
 * current substeps. Select the same current physical source at this exact
 * render-only getter, without publishing into simulation-owned cache memory. */
/* Use the original pure wheel matrix builder on an ABI-local copy. Neither
 * current wheel spin/steering nor actor/cache bytes are modified. The exact
 * caller immediately copies 48 bytes into its own sp+128 render matrix. */
static uint32_t wheel_render_pose(uint32_t actor,uint32_t wheel){
 uint32_t sp=psp_cpu.r[29],stack=sp-512,out=sp+128;
 if(!memory(wheel,340)||!memory(stack,512)||!memory(out,48)||psp_vfpu_prefix_pending())return 0;
 uint32_t copy=stack+32,rotation=stack+384;psp_cpu_state saved=psp_cpu;
 if(psp_mem_write_block(copy,psp_mem_ptr(wheel,340),340))return 0;
 psp_cpu.r[29]=stack;psp_cpu.r[4]=actor+112;psp_cpu.r[5]=rotation;psp_call_from_host(0x088803a4u);
 psp_cpu.r[4]=copy;psp_cpu.r[5]=rotation;psp_call_from_host(0x0892dd4cu);
 int ok=psp_cpu.r[29]==stack;psp_cpu=saved;if(!ok)return 0;
 float r[9],local[3],position[3];memcpy(r,psp_mem_ptr(rotation,36),36);memcpy(local,psp_mem_ptr(wheel+164,12),12);memcpy(position,psp_mem_ptr(actor+228,12),12);
 for(unsigned c=0;c<3;c++)psp_write_f32(copy+212+4*c,((r[c]*local[0]+r[c+3]*local[1])+r[c+6]*local[2])+position[c]);
 if(psp_mem_write_block(out,psp_mem_ptr(copy+176,48),48))return 0;return out;
}
/* The original tag50 branch computes the Shadow_Plane pose from current
 * body and committed ground data. Use an ABI-local part copy: its simulation
 * cache and original coarse publisher remain untouched. */
static uint32_t shadow_render_pose(uint32_t actor,uint32_t part){
 uint32_t sp=psp_cpu.r[29],stack=sp-512,out=sp+128;
 uint32_t body=psp_read32(actor+1256),ground=psp_read32(actor+556)+224;
 if(!memory(part,184)||!memory(body,56)||!memory(ground,96)||!memory(stack-128,640)||!memory(out,48)||psp_vfpu_prefix_pending())return 0;
 uint32_t copy=stack+32,context=stack+256;psp_cpu_state saved=psp_cpu;
 if(psp_mem_write_block(copy,psp_mem_ptr(part,184),184))return 0;static const unsigned char zero_context[80]={0};if(psp_mem_write_block(context,zero_context,80))return 0;
 psp_write32(context+20,body+8);psp_write32(context+72,ground);
 psp_cpu.r[29]=stack;psp_cpu.r[4]=copy;psp_cpu.r[5]=context;psp_cpu.r[6]=0;
 psp_call_from_host(0x08912458u);int ok=psp_cpu.r[29]==stack;psp_cpu=saved;if(!ok)return 0;
 if(psp_mem_write_block(out,psp_mem_ptr(copy+88,48),48))return 0;return out;
}
/* Render-only cinematic sampling using original calculations and rollback. */
static int event_signature_read(event_signature *s){
 memset(s,0,sizeof *s);if(!alive()||!memory(bound.vehicle,68)||!memory(bound.render,68))return 0;
 float scale=psp_read_f32(bound.vehicle+52);if(!isfinite(scale)||scale<=0||scale>=1||psp_read_f32(bound.render+52)!=scale)return 0;
 uint32_t manager=bound.car[11].object;if(!memory(manager,28)||psp_read32(manager+24)!=1)return 0;
 uint32_t list=psp_read32(manager+20);if(!memory(list,4))return 0;uint32_t child=psp_read32(list);if(!memory(child,104))return 0;
 uint32_t child_vt=psp_read32(child);if(child_vt!=0x08b3b164u||!memory(child_vt,16)||psp_read32(child_vt+12)!=0x08a352e0u)return 0;
 uint32_t controller=psp_read32(child+8);if(!memory(controller,180)||psp_read32(controller)!=0x08b3ae98u||method(controller)!=0x08a1fce4u)return 0;
 uint32_t descriptor=psp_read32(controller+36),main=psp_read32(controller+52),blend=psp_read32(controller+68),cin=psp_read32(controller+176);
 if(!memory(descriptor,64)||!memory(main,28)||psp_read32(main)!=0x08b3b004u||psp_read32(main+4)||!memory(cin,556)||psp_read32(cin)!=0x08b3b05cu||psp_read32(cin+4)!=9||(blend&&blend!=cin))return 0;
 uint32_t mode=psp_read32(cin+68);if(mode!=1&&mode!=2)return 0;
 uint32_t follow=psp_read32(main+24);if(!memory(follow,96)||psp_read32(follow)!=0x08b3aed8u||psp_read32(follow+12)!=1)return 0;
 uint32_t source=psp_read32(cin+124),actors=psp_read32(bound.world+0x289dc),n=psp_read32(bound.world+0x289e0);unsigned found=0;
 uint32_t wrapper=psp_read32(cin+76);if(!memory(wrapper,1120)||psp_read32(wrapper+1116)+192!=source)return 0;
 if(n>32||!memory(actors,n*4))return 0;for(unsigned i=0;i<n;i++)if(psp_read32(actors+i*4)+192==source)found++;
 if(found!=1)return 0;
 uint32_t w[]={bound.owner,bound.world,bound.physical[0].object,bound.physics,bound.vehicle,bound.render,manager,list,child,controller,descriptor,main,blend,cin,source,psp_read32(controller+24),psp_read32(controller+28),psp_read32(controller+32),psp_read32(bound.vehicle+52),actors,n,psp_read32(descriptor),psp_read32(descriptor+4),psp_read32(descriptor+12),psp_read32(descriptor+8),follow,child_vt,psp_read32(controller),psp_read32(manager+16),mode,psp_read32(cin+72),wrapper};
 memcpy(s->words,w,sizeof w);s->count=sizeof w/sizeof *w;return 1;
}
static int event_signature_same(const event_signature *s){
 event_signature current;if(!event_signature_read(&current)||memcmp(s,&current,sizeof current))return 0;
 if(event_frame.epoch_valid&&(!memory(event_frame.epoch_actor,1260)||psp_read32(event_frame.epoch_actor+1256)!=event_frame.epoch_body||psp_read32(event_frame.epoch_actor+552)!=event_frame.epoch_parts))return 0;
 return 1;
}
static void event_reset(void){memset(&event_frame,0,sizeof event_frame);}
static void event_root(void){
 event_frame.root=observation.roots;event_frame.attempted=event_frame.valid=event_frame.coarse=0;event_frame.actors_n=event_frame.parts_n=0;
 if(event_frame.epoch_valid&&!event_signature_same(&event_frame.epoch))event_frame.epoch_valid=0;
}
static void event_commit(void){
 event_frame.epoch_valid=0;
 if(event_frame.packet_scale<=0||event_frame.packet_scale>=1||packets.pending||packets.total!=1)return;
 if(event_signature_read(&event_frame.epoch)&&psp_read_f32(bound.vehicle+52)==event_frame.packet_scale){
  uint32_t actor=event_frame.epoch.words[14]-192;if(!memory(actor,1260))return;
  event_frame.epoch_actor=actor;event_frame.epoch_body=psp_read32(actor+1256);event_frame.epoch_parts=psp_read32(actor+552);
  event_frame.epoch_valid=1;event_frame.tick=coarse_tick;
 }
}
static int event_sources(void){
 uint32_t list=psp_read32(bound.world+0x289dc),n=psp_read32(bound.world+0x289e0);if(n>32||!memory(list,n*4))return 0;
 event_frame.actors_n=event_frame.parts_n=0;
 for(unsigned i=0;i<n;i++){
  uint32_t a=psp_read32(list+i*4);if(!memory(a,1260))return 0;
  uint32_t parts=psp_read32(a+552),count=psp_read32(a+1052),body=psp_read32(a+1256);if(count>256||!memory(parts,count*184)||!memory(body,56))return 0;
  unsigned valid_body=0;for(unsigned j=0;j<count;j++){uint32_t p=parts+j*184;if(psp_read32(p+136)==37&&(psp_read32(p+44)||psp_read32(p+76))&&!memcmp(psp_mem_ptr(p+88,48),psp_mem_ptr(body+8,48),48))valid_body++;}
  event_actor *actor=&event_frame.actors[event_frame.actors_n++];*actor=(event_actor){.actor=a,.parts=parts,.body=body,.count=count,.publish_body=!!valid_body};
  actor->ground=psp_read32(a+556);for(unsigned k=0;k<4;k++)actor->wheels[k]=psp_read32(a+1160+k*4);memcpy(actor->baseline,psp_mem_ptr(a+112,128),128);
  if(!valid_body)continue;
  for(unsigned j=0;j<count;j++){
   uint32_t p=parts+j*184,tag=psp_read32(p+136),vt=psp_read32(p);
   if(((tag<37||tag>41)&&tag!=50)||(!psp_read32(p+44)&&!psp_read32(p+76)))continue;
   if(!memory(vt,32)||psp_read32(vt+28)!=0x08913d40u||psp_read32(vt+16)!=0x08927ad4u)return 0;
   if(tag==37&&memcmp(psp_mem_ptr(p+88,48),psp_mem_ptr(body+8,48),48))continue;
   if(event_frame.parts_n==256)return 0;event_part *part=&event_frame.parts[event_frame.parts_n++];
   *part=(event_part){.part=p,.actor=a,.parts=parts,.body=body,.tag=tag};memcpy(part->baseline,psp_mem_ptr(p+88,48),48);
  }
 }
 return event_frame.actors_n&&event_frame.parts_n;
}
static int event_sources_same(void){
 if(!event_signature_same(&event_frame.epoch))return 0;
 if(event_frame.actors_n!=event_frame.epoch.words[20])return 0;
 for(unsigned i=0;i<event_frame.actors_n;i++)if(psp_read32(event_frame.epoch.words[19]+i*4)!=event_frame.actors[i].actor)return 0;
 for(unsigned i=0;i<event_frame.actors_n;i++){event_actor *a=&event_frame.actors[i];if(!memory(a->actor,1260)||psp_read32(a->actor+552)!=a->parts||psp_read32(a->actor+1052)!=a->count||psp_read32(a->actor+1256)!=a->body||!memory(a->parts,a->count*184)||!memory(a->body,56)||psp_read32(a->actor+556)!=a->ground||memcmp(a->baseline,psp_mem_ptr(a->actor+112,128),128))return 0;for(unsigned k=0;k<4;k++)if(psp_read32(a->actor+1160+k*4)!=a->wheels[k])return 0;}
 for(unsigned i=0;i<event_frame.parts_n;i++){event_part *p=&event_frame.parts[i];uint32_t vt=psp_read32(p->part);if(psp_read32(p->part+136)!=p->tag||(!psp_read32(p->part+44)&&!psp_read32(p->part+76))||!memory(vt,32)||psp_read32(vt+28)!=0x08913d40u||psp_read32(vt+16)!=0x08927ad4u||memcmp(p->baseline,psp_mem_ptr(p->part+88,48),48))return 0;}
 return 1;
}
static int event_prepare_physics(void *unused){
 (void)unused;uint32_t stack=psp_cpu.r[29]-64;if(!memory(stack,64))return 0;
 for(unsigned i=0;i<3;i++)psp_write_f32(stack+32+i*4,event_frame.delta[i]);
 psp_cpu.r[29]=stack;psp_cpu.r[4]=bound.physics;psp_cpu.r[5]=stack+32;psp_cpu.r[6]=0;psp_call_from_host(0x08879d38u);if(psp_cpu.r[29]!=stack)return 0;float phase=feedback.interval>0?event_frame.delta[1]/feedback.interval:NAN;return feedback_prepare(&phase);
}
static int event_capture_poses(void *unused){
 (void)unused;
 for(unsigned i=0;i<event_frame.actors_n;i++){event_actor *a=&event_frame.actors[i];memcpy(a->pose,psp_mem_ptr(a->body+8,48),48);memcpy(a->spatial,psp_mem_ptr(a->actor+228,12),12);for(unsigned k=0;k<3;k++)if(!isfinite(psp_read_f32(a->actor+228+k*4)))return 0;}
 for(unsigned i=0;i<event_frame.parts_n;i++){
  event_part *p=&event_frame.parts[i];uint32_t source=0;
  if(p->tag==37)source=p->body+8;
  else if(p->tag==50)source=shadow_render_pose(p->actor,p->part);
  else{static const unsigned index[4]={0,2,1,3};source=wheel_render_pose(p->actor,psp_read32(p->actor+1160+index[p->tag-38]*4));}
  if(!source||!memory(source,48))return 0;
  for(unsigned k=0;k<12;k++)if(!isfinite(psp_read_f32(source+k*4)))return 0;memcpy(p->pose,psp_mem_ptr(source,48),48);
 }
 return 1;
}
static int event_validate(void){
 if(!event_frame.valid)return 0;
 if(event_frame.root!=observation.roots||!enabled||!in_root||event_frame.coarse||packets.pending||!event_sources_same()){
  restore_poses();event_frame.valid=event_frame.epoch_valid=0;event_frame.attempted=1;return 0;
 }
 return 1;
}
static int event_visual_ready(void){return event_validate()&&camera_overlay==event_frame.camera.descriptor;}
static int event_rear_prepare(void){
 if(!event_frame.valid)return 1;
 /* Original Radar reads player/opponent actor+228/232, independently of
  * the body matrix. Publish sampled spatial inputs only inside its guard. */
 for(unsigned i=0;i<event_frame.actors_n;i++){event_actor *a=&event_frame.actors[i];if(psp_mem_write_block(a->actor+228,a->spatial,12))return 0;}
 return 1;
}
static int event_publish(void){
 if(!event_sources_same())return 0;
 asphalt_camera_frame projected;asphalt_camera_frame *frame=&event_frame.camera;
 float near_plane=psp_read_f32(frame->descriptor+16),far_plane=psp_read_f32(frame->descriptor+20),saved_near,saved_far;memcpy(&saved_near,frame->ranges[0].bytes+16,4);memcpy(&saved_far,frame->ranges[0].bytes+20,4);
 if(near_plane!=saved_near||far_plane!=saved_far){if(!asphalt_native_camera_project(frame,event_frame.epoch.words[8],near_plane,far_plane,&projected))goto fail;frame=&projected;}
 static const unsigned order[5]={0,2,3,4,1};for(unsigned i=0;i<5;i++){asphalt_camera_range *r=&frame->ranges[order[i]];if(!save_range(r->address,r->size))goto fail;}
 for(unsigned i=0;i<event_frame.actors_n;i++)if(event_frame.actors[i].publish_body&&!save_range(event_frame.actors[i].body+8,48))goto fail;
 for(unsigned i=0;i<5;i++){asphalt_camera_range *r=&frame->ranges[i];memcpy(psp_mem_ptr(r->address,r->size),r->bytes,r->size);}
 for(unsigned i=0;i<event_frame.actors_n;i++){event_actor *a=&event_frame.actors[i];if(a->publish_body)memcpy(psp_mem_ptr(a->body+8,48),a->pose,48);}
 camera_overlay=frame->descriptor;rear_camera_pose=frame->rear_pose;observation.camera_applied++;return 1;
fail:restore_poses();event_frame.valid=0;return 0;
}
static int event_render_camera(uint32_t descriptor){
 if(!enabled||!in_root||event_frame.root!=observation.roots||event_frame.coarse||packets.pending||!event_frame.epoch_valid||descriptor!=event_frame.epoch.words[10])return 0;
 if(event_frame.valid)return event_publish();
 if(event_frame.attempted)return 0;event_frame.attempted=1;
 if(!event_signature_same(&event_frame.epoch)||!event_sources()){event_frame.epoch_valid=0;return 0;}
 float elapsed=original_seconds(psp_read32(bound.owner)-event_frame.tick),scale=psp_read_f32(bound.vehicle+52);
 if(!isfinite(elapsed)||elapsed<=0||elapsed>=.03f)return 0;
 event_frame.delta[0]=fminf(elapsed*scale,psp_read_f32(bound.vehicle+48));event_frame.delta[1]=elapsed;event_frame.delta[2]=elapsed*scale;
 event_frame.attempts++;observation.camera_attempts++;
 uint32_t manager=bound.car[11].object;
 if(!asphalt_native_camera_calculate_prepared(event_frame.epoch.words[8],psp_read32(manager+16),event_frame.delta,event_prepare_physics,event_capture_poses,NULL,&event_frame.camera)){observation.camera_refused++;return 0;}
 event_frame.valid=1;return event_publish();
}
static uint32_t event_render_part(uint32_t part,uint32_t caller){
 if(caller!=0x088c3a04u||!event_visual_ready())return 0;
 for(unsigned i=0;i<event_frame.parts_n;i++)if(event_frame.parts[i].part==part){uint32_t out=psp_cpu.r[29]+128;if(!memory(out,48))return 0;memcpy(psp_mem_ptr(out,48),event_frame.parts[i].pose,48);return out;}
 return 0;
}
static void event_release(uint32_t object){
 if(!object)return;int retire=0;
 if(object==event_frame.epoch_actor||object==event_frame.epoch_body||object==event_frame.epoch_parts)retire=1;
 for(unsigned i=0;i<event_frame.epoch.count;i++)if(i<15||i==19||i>20)if(event_frame.epoch.words[i]==object)retire=1;
 for(unsigned i=0;i<event_frame.actors_n;i++){event_actor *a=&event_frame.actors[i];if(object==a->actor||object==a->parts||object==a->body||object==a->ground)retire=1;for(unsigned k=0;k<4;k++)if(object==a->wheels[k])retire=1;}
 if(retire){event_frame.valid=event_frame.epoch_valid=0;}
}

static void recovery_reset(void){memset(&recovery,0,sizeof recovery);}
static int recovery_contract_any(void){
 if(asphalt_preview_active())return 0;
 if(recovery.owner&&enabled&&in_root&&(recovery.owner!=bound.owner||recovery.world!=bound.world||!memory(bound.vehicle,56)||!memory(bound.render,56)||psp_read_f32(bound.vehicle+52)!=1||psp_read_f32(bound.render+52)!=1)){recovery_reset();return 0;}
 return configured&&enabled&&in_root&&bound.owner&&alive()&&psp_read_f32(bound.vehicle+52)==1&&psp_read_f32(bound.render+52)==1;
}
static int recovery_contract(void){return recovery_contract_any()&&!packets.pending;}
static void recovery_posted(int ready){
 recovery_reset();if(!ready||!recovery_contract())return;
 uint32_t list=psp_read32(bound.world+0x289dc),n=psp_read32(bound.world+0x289e0);if(!n||n>32||!memory(list,n*4))return;
 recovery.owner=bound.owner;recovery.world=bound.world;recovery.list=list;recovery.count=n;for(unsigned i=0;i<n;i++)recovery.members[i]=psp_read32(list+4*i);recovery.ready_root=observation.roots;recovery.ready=1;
}
static int recovery_members(void){
 if(!recovery_contract_any()||recovery.owner!=bound.owner||recovery.world!=bound.world||psp_read32(bound.world+0x289dc)!=recovery.list||psp_read32(bound.world+0x289e0)!=recovery.count||!memory(recovery.list,recovery.count*4))return 0;
 for(unsigned i=0;i<recovery.count;i++)if(psp_read32(recovery.list+4*i)!=recovery.members[i])return 0;return 1;
}
static int recovery_identity(recovery_actor *a){
 if(!memory(a->actor,1260)||psp_read32(a->actor+240)!=a->vt||psp_read32(a->actor+376)!=1||psp_read32(a->actor+1256)!=a->body||psp_read32(a->actor+552)!=a->parts||psp_read32(a->actor+1052)!=a->count||psp_read32(a->actor+556)!=a->ground||!memory(a->body,56)||!memory(a->parts,a->count*184))return 0;
 for(unsigned i=0;i<4;i++)if(psp_read32(a->actor+1160+4*i)!=a->wheels[i])return 0;
 for(unsigned i=0;i<a->n;i++){recovery_part *p=&recovery.parts[a->first+i];if(psp_read32(p->part)!=p->vt||psp_read32(p->part+136)!=p->tag||psp_read32(p->part+44)!=p->render[0]||psp_read32(p->part+76)!=p->render[1]||memcmp(p->cache,psp_mem_ptr(p->part+88,48),48))return 0;}return 1;
}
static void recovery_begin(task t){
 if(!recovery.ready||recovery.ready_root!=observation.roots||t.pc!=0x089794c0u||t.object!=bound.car[7].object||!recovery_contract()||!recovery_members()){recovery_reset();return;}recovery.ready=0;recovery.pending=1;
 for(unsigned i=0;i<recovery.count;i++){
  uint32_t actor=recovery.members[i];if(!memory(actor,1260)||psp_read32(actor+376)!=1||psp_read32(actor+240)!=0x08b3863cu)continue;
  uint32_t body=psp_read32(actor+1256),parts=psp_read32(actor+552),count=psp_read32(actor+1052);if(count>256||!memory(parts,count*184)||!memory(body,56))continue;
  recovery_actor *a=&recovery.actors[recovery.actors_n];*a=(recovery_actor){.actor=actor,.body=body,.parts=parts,.count=count,.vt=psp_read32(actor+240),.ground=psp_read32(actor+556),.first=recovery.parts_n,.valid=1};for(unsigned k=0;k<4;k++)a->wheels[k]=psp_read32(actor+1160+4*k);memcpy(a->before,psp_mem_ptr(body+8,48),48);
  unsigned bodies=0;
  for(unsigned j=0;j<count;j++){
   uint32_t p=parts+j*184;if(!psp_read32(p+44)&&!psp_read32(p+76))continue;uint32_t tag=psp_read32(p+136),vt=psp_read32(p);
   if(((tag<37||tag>41)&&tag!=50)||!memory(vt,32)||psp_read32(vt+28)!=0x08913d40u||psp_read32(vt+16)!=0x08927ad4u||recovery.parts_n==256){a->valid=0;break;}
   if(tag==37){if(memcmp(a->before,psp_mem_ptr(p+88,48),48)){a->valid=0;break;}bodies++;}
   recovery_part *out=&recovery.parts[recovery.parts_n++];*out=(recovery_part){.part=p,.vt=vt,.tag=tag,.render={psp_read32(p+44),psp_read32(p+76)}};memcpy(out->cache,psp_mem_ptr(p+88,48),48);a->n++;
  }
  if(!a->valid||!bodies){recovery.parts_n=a->first;continue;}recovery.actors_n++;
 }
}
static void recovery_end(int ok){
 if(!ok||!recovery.pending||!recovery_contract()||!recovery_members()){recovery_reset();return;}recovery.pending=0;
 for(unsigned i=0;i<recovery.actors_n;i++){recovery_actor *a=&recovery.actors[i];if(!a->valid||!recovery_identity(a)||!memcmp(a->before,psp_mem_ptr(a->body+8,48),48)){a->valid=0;continue;}
  for(unsigned k=0;k<12;k++)if(!isfinite(psp_read_f32(a->body+8+4*k)))a->valid=0;
  memcpy(a->pose,psp_mem_ptr(a->body+8,48),48);memcpy(a->spatial,psp_mem_ptr(a->actor+112,128),128);
 }
}
static int recovery_actor_ready(recovery_actor *a){
 if(!a->valid||recovery.pending)return 0;
 if(!recovery_contract()||!recovery_members()){recovery_reset();return 0;}
 if(!recovery_identity(a)||memcmp(a->pose,psp_mem_ptr(a->body+8,48),48)||memcmp(a->spatial,psp_mem_ptr(a->actor+112,128),128)){a->valid=0;return 0;}return 1;
}
static uint32_t recovery_render_part(uint32_t part,uint32_t caller){
 if(caller!=0x088c3a04u||!recovery_contract())return 0;
 for(unsigned i=0;i<recovery.actors_n;i++){recovery_actor *a=&recovery.actors[i];if(!recovery_actor_ready(a))continue;for(unsigned j=0;j<a->n;j++){recovery_part *p=&recovery.parts[a->first+j];if(p->part!=part)continue;if(p->tag==37)return a->body+8;if(p->tag==50)return shadow_render_pose(a->actor,part);static const unsigned index[4]={0,2,1,3};return wheel_render_pose(a->actor,a->wheels[index[p->tag-38]]);}}return 0;
}
static int recovery_model_ready(uint32_t matrix){
 if(matrix<128||!recovery_contract())return 0;uint32_t e=matrix-128;if(!memory(e,416)||psp_read32(e)!=0x08b399a4u||method(e)!=0x08987eb4u)return 0;uint32_t owner=psp_read32(e+280);if(!memory(owner,100)||psp_read32(owner)!=0x08b39804u||!psp_read8(owner+4))return 0;uint32_t actor=psp_read32(owner+28);if(actor!=psp_read32(owner+96))return 0;
 for(unsigned i=0;i<recovery.actors_n;i++)if(recovery.actors[i].actor==actor)return recovery_actor_ready(&recovery.actors[i]);return 0;
}
static void recovery_pending_validate(void){
 if(asphalt_preview_active()||!packets.pending||!recovery.owner||recovery.pending)return;
 if(!recovery_members()){recovery_reset();return;}
 for(unsigned i=0;i<recovery.actors_n;i++)if(recovery.actors[i].valid&&!recovery_identity(&recovery.actors[i]))recovery.actors[i].valid=0;
}
/* Original graphics may publish the relocation body after the last packed
 * part POST. The next ordinary partial step moves physical state again.
 * That saved publication is an additional exact base witness for this actor
 * only, until the next real POST; it is not a predicted/current pose check. */
static int recovery_anchor_base(uint32_t actor,uint32_t matrix){
 if(!packets.pending||recovery.pending||!memory(matrix,64))return 0;
 if(!recovery_members()){recovery_reset();return 0;}
 for(unsigned i=0;i<recovery.actors_n;i++){recovery_actor*a=&recovery.actors[i];if(a->actor!=actor||!a->valid)continue;if(!recovery_identity(a)){a->valid=0;return 0;}
  uint32_t out[16]={0};for(unsigned c=0;c<4;c++)for(unsigned r=0;r<3;r++)memcpy(out+c*4+r,a->pose+c*12+r*4,4);out[15]=0x3f800000u;return !memcmp(out,psp_mem_ptr(matrix,64),64);
 }return 0;
}
static void recovery_release(uint32_t object){
 if(!object)return;if(object==recovery.owner||object==recovery.world||object==recovery.list){recovery_reset();return;}
 for(unsigned i=0;i<recovery.actors_n;i++){recovery_actor *a=&recovery.actors[i];if(object==a->actor||object==a->body||object==a->parts||object==a->ground||object==a->vt)a->valid=0;for(unsigned k=0;k<4;k++)if(object==a->wheels[k])a->valid=0;for(unsigned j=0;j<a->n;j++){recovery_part*p=&recovery.parts[a->first+j];if(object==p->part||object==p->vt||object==p->render[0]||object==p->render[1])a->valid=0;}}
}

/* Civilian traffic is owned by a separate world vector and AI property set;
 * it is not a full race-car actor. Its original renderer consumes the same
 * packed part getter through 088CF110 and copies the result to sp+64. */
static uint32_t traffic_owner(uint32_t part){
 if(!memory(bound.world,0x289e4)||method(bound.world)!=0x0892082cu||
    !memory(bound.manager,100)||method(bound.manager)!=0x0883ad34u)return 0;
 uint32_t list=psp_read32(bound.world+0x289cc),n=psp_read32(bound.world+0x289d0),actor=0;
 if(n>128||!memory(list,n*4))return 0;
 for(unsigned i=0;i<n;i++){
  uint32_t a=psp_read32(list+4*i);if(!memory(a,816)||psp_read32(a+240)!=0x08b3871cu||psp_read8(a+724)!=1)continue;
  uint32_t parts=psp_read32(a+392),count=psp_read32(a+756);
  if(count>256||!memory(parts,count*184)||part<parts||part-parts>=count*184||(part-parts)%184)continue;
  if(actor)return 0;actor=a;
 }
 if(!actor)return 0;
 list=psp_read32(bound.manager+80);n=psp_read32(bound.manager+84);unsigned owners=0;
 if(n>128||!memory(list,n*4))return 0;
 for(unsigned i=0;i<n;i++){
  uint32_t raw=psp_read32(list+4*i);
  if(memory(raw,324)&&psp_read32(raw+28)==0x08b368d4u&&psp_read32(raw+312)==actor&&psp_read32(raw+316)==actor)owners++;
 }
 return owners==1?actor:0;
}
static uint32_t traffic_render_pose(uint32_t actor,uint32_t part,unsigned tag){
 unsigned mode=psp_read32(actor+384);if(mode!=1&&mode!=2)return 0;
 if(mode==1&&tag==37&&(psp_read32(actor+128)&2))return actor+192;
 uint32_t sp=psp_cpu.r[29],stack=sp-1536,out=sp+64;
 if(!memory(stack-256,1792)||!memory(out,48)||psp_vfpu_prefix_pending())return 0;
 uint32_t ground=0;
 if(mode==2||tag==50){
  uint32_t owner=psp_read32(actor+376);
  if(!memory(owner,352)||psp_read32(owner)!=0x08b38588u||psp_read32(owner+8)!=actor)return 0;
  ground=owner+224;
 }
 uint32_t copy=stack+32,part_copy=stack+864,context=stack+1056,source=copy+192;
 psp_cpu_state saved=psp_cpu;memcpy(psp_mem_ptr(copy,816),psp_mem_ptr(actor,816),816);psp_cpu.r[29]=stack;
 /* AI path placement can still be metres above the road in a partial
  * packet. Original mode2 physics aligns the body at 0892B2F0 before its
  * wheel builder. Apply that pure conversion to this local copy as well;
  * mode1 collision/airborne poses must retain their physical height. */
 if(mode==2){psp_cpu.r[4]=copy;psp_call_from_host(0x0892bef0u);}
 if(tag>=38&&tag<=41){
  /* Pure original conversion uses the committed wheelspin; it advances no
   * traffic path, speed, physics, animation, timers or event state. */
  psp_cpu.r[4]=copy;psp_call_from_host(0x0892b77cu);
  static const unsigned index[4]={0,2,1,3};source=copy+508+48*index[tag-38];
 }else{
  if(!(psp_read32(copy+128)&2)){psp_cpu.r[4]=copy+112;psp_cpu.r[5]=copy+192;psp_call_from_host(0x088803a4u);}
  if(tag==50){
   memcpy(psp_mem_ptr(part_copy,184),psp_mem_ptr(part,184),184);memset(psp_mem_ptr(context,80),0,80);
   psp_write32(context+20,copy+192);psp_write32(context+72,ground);
   psp_cpu.r[4]=part_copy;psp_cpu.r[5]=context;psp_cpu.r[6]=0;psp_call_from_host(0x08912458u);source=part_copy+88;
  }
 }
 int ok=psp_cpu.r[29]==stack;psp_cpu=saved;if(!ok)return 0;
 memcpy(psp_mem_ptr(out,48),psp_mem_ptr(source,48),48);return out;
}
uint32_t asphalt_native_engine_part_pose(uint32_t part,uint32_t caller){
 recovery_pending_validate();
 uint32_t recovery_pose=recovery_render_part(part,caller);if(recovery_pose)return recovery_pose;
 uint32_t event_pose=event_render_part(part,caller);if(event_pose)return event_pose;
 if(caller==0x088c3a04u&&in_root&&(!enabled||!packets.pending))observe_committed_body(part);
 if((caller!=0x088c3a04u&&caller!=0x088cf110u)||!enabled||!in_root||!packets.pending||!memory(part,184))return 0;
 unsigned tag=psp_read32(part+136);if((tag<37||tag>41)&&tag!=50)return 0;
 uint32_t vt=psp_read32(part);if(!memory(vt,32)||psp_read32(vt+28)!=0x08913d40u||psp_read32(vt+16)!=0x08927ad4u)return 0;
 if(caller==0x088cf110u){uint32_t actor=traffic_owner(part);return actor?traffic_render_pose(actor,part,tag):0;}
 if(!memory(bound.world,0x289e4)||method(bound.world)!=0x0892082cu)return 0;
 uint32_t list=psp_read32(bound.world+0x289dc),n=psp_read32(bound.world+0x289e0);
 if(n>128||!memory(list,n*4))return 0;
 for(unsigned i=0;i<n;i++){
  uint32_t actor=psp_read32(list+4*i);if(!memory(actor,1260))continue;
  uint32_t parts=psp_read32(actor+552),count=psp_read32(actor+1052);
  if(count>256||!memory(parts,count*184)||part<parts||part-parts>=count*184||(part-parts)%184)continue;
  if(tag==37){uint32_t body=psp_read32(actor+1256);if(!memory(body,56))return 0;
   observe_source(&observation.model,part,body+8,48,actor,parts,body,"current_physics");return body+8;}
  if(tag==50)return shadow_render_pose(actor,part);
  static const unsigned index[4]={0,2,1,3};return wheel_render_pose(actor,psp_read32(actor+1160+4*index[tag-38]));
 }
 return 0;
}
static void unpack_anchor(uint32_t source,uint32_t out[16]){
 memset(out,0,64);
 for(unsigned column=0;column<4;column++)for(unsigned row=0;row<3;row++)out[column*4+row]=psp_read32(source+column*12+row*4);
 out[15]=0x3f800000u;
}
/* Original089C0184 expands the bound car body into the effect's base matrix.
 * For this exact data-free transform descriptor,089882A8 publishes that base
 * unchanged. Export only the same expansion from the current physical body;
 * neither the effect updater nor its age/color/event calculation runs here. */
static int effect_anchor(uint32_t matrix,uint32_t out[16]){
 if(matrix<128)return 0;
 uint32_t effect=matrix-128;
 if(!memory(effect,416)||psp_read32(effect)!=0x08b399a4u||method(effect)!=0x08987eb4u||
    psp_read32(effect+8)!=1||psp_read8(effect+32)!=1||psp_read8(effect+112)!=1||psp_read32(effect+348)!=0x400000u)return 0;
 unsigned state=psp_read32(effect+272);if(state!=1&&state!=2)return 0;
 if(memcmp(psp_mem_ptr(effect+48,64),psp_mem_ptr(matrix,64),64))return 0;
 uint32_t descriptor=psp_read32(effect+12);
 if(!memory(descriptor,68)||psp_read32(descriptor+56)!=1)return 0;
 uint32_t properties=psp_read32(descriptor+64);if(!memory(properties,4))return 0;
 uint32_t property=psp_read32(properties);
 if(!memory(property,12)||psp_read32(property)!=0x400000u||psp_read32(property+4)||psp_read32(property+8))return 0;
 uint32_t owner=psp_read32(effect+280);
 if(!memory(owner,100)||psp_read32(owner)!=0x08b39804u||!psp_read8(owner+4))return 0;
 uint32_t actor=psp_read32(owner+28);
 if(actor!=psp_read32(owner+96)||!memory(actor,1260)||!memory(bound.world,0x289e4)||method(bound.world)!=0x0892082cu)return 0;
 uint32_t list=psp_read32(bound.world+0x289dc),count=psp_read32(bound.world+0x289e0);
 if(count>128||!memory(list,count*4))return 0;
 int found=0;for(unsigned i=0;i<count;i++)if(psp_read32(list+4*i)==actor)found=1;
 if(!found)return 0;
 /* Refuse local/offset attachments: the cached base must be byte-exactly
  * this actor's original, committed body pose from the ordinary part getter. */
 uint32_t parts=psp_read32(actor+552),nparts=psp_read32(actor+1052);
 if(nparts>128||!memory(parts,nparts*184))return 0;
 found=recovery_anchor_base(actor,effect+48);uint32_t coarse[16];
 for(unsigned i=0;i<nparts;i++){
  uint32_t part=parts+184*i;if(psp_read32(part+136)!=37)continue;
  uint32_t vt=psp_read32(part);
  if(!memory(vt,32)||psp_read32(vt+28)!=0x08913d40u||psp_read32(vt+16)!=0x08927ad4u)continue;
  unpack_anchor(part+88,coarse);if(!memcmp(coarse,psp_mem_ptr(effect+48,64),64))found=1;
 }
 if(!found)return 0;
 uint32_t body=psp_read32(actor+1256);if(!memory(body,56))return 0;
 for(unsigned i=0;i<12;i++)if(!isfinite(psp_read_f32(body+8+4*i)))return 0;
 unpack_anchor(body+8,out);return 1;
}
/* The rank billboards use the same coarse car base as NOS, with additional
 * original camera-facing and distance-scale properties. Rebuild their final
 * matrix at the exact consumer, at the existing effect age. Every original
 * cache/property write is journaled and discarded; only the matrix is exported. */
typedef struct {uint32_t effect,body;uint32_t *out;} rank_capture;
static int prepare_rank(void *user){
 rank_capture *c=user;uint32_t expanded[16];unpack_anchor(c->body,expanded);
 for(unsigned i=0;i<16;i++)psp_write32(c->effect+48+4*i,expanded[i]);
 return 1;
}
static int capture_rank(void *user){
 rank_capture *c=user;
 if(psp_read8(c->effect+112)!=1)return 0;
 for(unsigned i=0;i<16;i++)if(!isfinite(psp_read_f32(c->effect+128+4*i)))return 0;
 memcpy(c->out,psp_mem_ptr(c->effect+128,64),64);return 1;
}
static int rank_anchor(uint32_t matrix,uint32_t out[16]){
 if(matrix<128||psp_vfpu_prefix_pending())return 0;uint32_t e=matrix-128;
 if(!memory(e,416)||psp_read32(e)!=0x08b399a4u||method(e)!=0x08987eb4u||
    psp_read32(e+8)!=1||psp_read8(e+32)!=1||psp_read8(e+112)!=1||psp_read8(e+332)!=1||psp_read32(e+348)!=0x20412044u)return 0;
 unsigned state=psp_read32(e+272);if(state!=1&&state!=2)return 0;
 uint32_t d=psp_read32(e+12);if(!memory(d,68)||psp_read32(d)!=0x08b399e0u||psp_read32(d+56)!=6||psp_read32(d+60)!=0x20412044u)return 0;
 uint32_t list=psp_read32(d+64);if(!memory(list,24))return 0;
 static const uint32_t types[6]={0x2000,0x40,4,0x10000,0x400000,0x20000000};
 static const unsigned lengths[6]={1,0,3,0,0,3};
 for(unsigned i=0;i<6;i++){
  uint32_t p=psp_read32(list+4*i);if(!memory(p,12)||psp_read32(p)!=types[i]||psp_read32(p+4)!=lengths[i])return 0;
  uint32_t data=psp_read32(p+8);if(!lengths[i]){if(data)return 0;continue;}
  if(!memory(data,lengths[i]*4))return 0;
  for(unsigned j=0;j<lengths[i];j++)if(!isfinite(psp_read_f32(data+4*j)))return 0;
 }
 uint32_t camera=psp_read32(e+336);if(!memory(camera,64))return 0;
 for(unsigned i=0;i<16;i++)if(!isfinite(psp_read_f32(camera+4*i)))return 0;
 float age=psp_read_f32(e+352);if(!isfinite(age)||age<0)return 0;
 uint32_t owner=psp_read32(e+280);
 if(!memory(owner,100)||psp_read32(owner)!=0x08b39804u||!psp_read8(owner+4))return 0;
 uint32_t actor=psp_read32(owner+28);
 if(actor!=psp_read32(owner+96)||!memory(actor,1260)||!memory(bound.world,0x289e4)||method(bound.world)!=0x0892082cu)return 0;
 list=psp_read32(bound.world+0x289dc);unsigned count=psp_read32(bound.world+0x289e0);
 if(count>128||!memory(list,count*4))return 0;
 int found=0;for(unsigned i=0;i<count;i++)if(psp_read32(list+4*i)==actor)found=1;if(!found)return 0;
 uint32_t parts=psp_read32(actor+552);unsigned n=psp_read32(actor+1052);
 if(n>128||!memory(parts,n*184))return 0;
 found=recovery_anchor_base(actor,e+48);uint32_t coarse[16];
 for(unsigned i=0;i<n;i++){
  uint32_t part=parts+184*i;if(psp_read32(part+136)!=37)continue;
  uint32_t vt=psp_read32(part);if(!memory(vt,32)||psp_read32(vt+28)!=0x08913d40u||psp_read32(vt+16)!=0x08927ad4u)continue;
  unpack_anchor(part+88,coarse);if(!memcmp(coarse,psp_mem_ptr(e+48,64),64))found=1;
 }
 if(!found||!alive())return 0;
 uint32_t body=psp_read32(actor+1256);if(!memory(body,56))return 0;
 for(unsigned i=0;i<12;i++)if(!isfinite(psp_read_f32(body+8+4*i)))return 0;
 rank_capture c={e,body+8,out};float zero[3]={0};psp_cpu_state saved=psp_cpu;psp_cpu.f[12]=age;
 int ok=asphalt_preview_call_prepared(0x089882a8u,e,0,zero,prepare_rank,capture_rank,&c);
 psp_cpu=saved;return ok;
}
/* Night rear coronas have an authored local translation and the original
 * CARLIGHTCORONA camera calculation, so the pure-base shortcut above cannot
 * handle them. Original089A3638 stores StageGlobal.NightStage at stage+136;
 * inspect() binds its089A4028 update as vehicle job10. Replay only this exact
 * three-property contract at the committed age/current body. The journal
 * discards every visibility, colour, custom-data and time write; only the
 * final64-byte model matrix is published for this consumer's scope. */
static int night_corona_anchor(uint32_t matrix,uint32_t out[16]){
 if(matrix<128||psp_vfpu_prefix_pending())return 0;uint32_t e=matrix-128;
 if(!memory(e,416)||psp_read32(e)!=0x08b399a4u||method(e)!=0x08987eb4u||
    psp_read32(e+8)!=1||psp_read8(e+32)!=1||psp_read8(e+112)!=1||psp_read8(e+332)!=1||psp_read32(e+348)!=0x08400004u)return 0;
 unsigned state=psp_read32(e+272);if(state!=1&&state!=2)return 0;
 uint32_t night=bound.car[10].object;
 if(bound.car[10].pc!=0x089a4028u||!memory(night,140)||psp_read32(night)!=0x08b39c00u||psp_read8(night+136)!=1)return 0;
 uint32_t d=psp_read32(e+12);
 if(!memory(d,68)||psp_read32(d)!=0x08b399e0u||psp_read32(d+56)!=3||psp_read32(d+60)!=0x08400004u)return 0;
 uint32_t list=psp_read32(d+64);if(!memory(list,12))return 0;
 static const uint32_t types[3]={0x08000000u,4,0x00400000u};
 for(unsigned i=0;i<3;i++){
  uint32_t p=psp_read32(list+4*i);unsigned n=i==1?3:0;
  if(!memory(p,12)||psp_read32(p)!=types[i]||psp_read32(p+4)!=n)return 0;
  uint32_t data=psp_read32(p+8);if(!n){if(data)return 0;continue;}
  if(!memory(data,12))return 0;
  for(unsigned j=0;j<3;j++)if(!isfinite(psp_read_f32(data+4*j)))return 0;
 }
 uint32_t camera=psp_read32(e+336),custom=psp_read32(e+400);
 if(!memory(camera,64)||!memory(custom,32))return 0;
 for(unsigned i=0;i<16;i++)if(!isfinite(psp_read_f32(camera+4*i)))return 0;
 for(unsigned i=0;i<8;i++)if(!isfinite(psp_read_f32(custom+4*i)))return 0;
 float age=psp_read_f32(e+352);if(!isfinite(age)||age<0)return 0;
 uint32_t owner=psp_read32(e+280);
 if(!memory(owner,100)||psp_read32(owner)!=0x08b39804u||!psp_read8(owner+4))return 0;
 uint32_t actor=psp_read32(owner+28);
 if(actor!=psp_read32(owner+96)||!memory(actor,1260)||!memory(bound.world,0x289e4)||method(bound.world)!=0x0892082cu)return 0;
 list=psp_read32(bound.world+0x289dc);unsigned count=psp_read32(bound.world+0x289e0);
 if(count>128||!memory(list,count*4))return 0;
 int found=0;for(unsigned i=0;i<count;i++)if(psp_read32(list+4*i)==actor)found=1;if(!found)return 0;
 uint32_t parts=psp_read32(actor+552);unsigned n=psp_read32(actor+1052);
 if(n>128||!memory(parts,n*184))return 0;
 found=recovery_anchor_base(actor,e+48);uint32_t coarse[16];
 for(unsigned i=0;i<n;i++){
  uint32_t part=parts+184*i;if(psp_read32(part+136)!=37)continue;uint32_t vt=psp_read32(part);
  if(!memory(vt,32)||psp_read32(vt+28)!=0x08913d40u||psp_read32(vt+16)!=0x08927ad4u)continue;
  unpack_anchor(part+88,coarse);if(!memcmp(coarse,psp_mem_ptr(e+48,64),64))found=1;
 }
 if(!found||!alive())return 0;
 uint32_t body=psp_read32(actor+1256);if(!memory(body,56))return 0;
 for(unsigned i=0;i<12;i++)if(!isfinite(psp_read_f32(body+8+4*i)))return 0;
 rank_capture c={e,body+8,out};float zero[3]={0};psp_cpu_state saved=psp_cpu;psp_cpu.f[12]=age;
 int ok=asphalt_preview_call_prepared(0x089882a8u,e,0,zero,prepare_rank,capture_rank,&c);
 psp_cpu=saved;return ok;
}
/* The original pre-queue culler at08A817E8 must see the same pure NOS
 * base as the later model load; otherwise it can discard a visible flame
 * before the existing drawing scope corrects its position. */
int asphalt_native_engine_model_enter(uint32_t matrix,uint32_t caller){
 if(asphalt_preview_active())return 0;
 recovery_pending_validate();
 unsigned slot=model_depth++;if(slot>=MODEL_SCOPE_LIMIT)return 0;
 model_scope *scope=&model_scopes[slot];scope->matrix=0;
 if((caller!=0x08a7d99cu&&caller!=0x08a819acu&&caller!=0x08a817e8u)||!configured||!enabled||!in_root||(!packets.pending&&!event_visual_ready()&&!recovery_model_ready(matrix)))return 0;
 uint32_t current[16];int ok=caller==0x08a817e8u?effect_anchor(matrix,current):
  caller==0x08a819acu?rank_anchor(matrix,current):(effect_anchor(matrix,current)||night_corona_anchor(matrix,current));if(!ok||!alive())return 0;
 scope->matrix=matrix;memcpy(scope->bytes,psp_mem_ptr(matrix,64),64);
 memcpy(psp_mem_ptr(matrix,64),current,64);return 1;
}
void asphalt_native_engine_model_leave(void){
 if(asphalt_preview_active()||!model_depth)return;
 unsigned slot=--model_depth;if(slot>=MODEL_SCOPE_LIMIT)return;
 model_scope *scope=&model_scopes[slot];
 if(scope->matrix){void *p=psp_mem_ptr(scope->matrix,64);if(p)memcpy(p,scope->bytes,64);scope->matrix=0;}
}
/* RearRadar is authored projected UI. Its ordinary producer runs before the
 * fractional render camera load. Preview that exact producer once per pending
 * native frame, at frozen UI time and the exact original calculator pose.
 * Preserve its chosen vehicles/order/visibility/alpha/scale and vertex caches;
 * only parentXY/pivot/rotation of existing generic circles can change. */
static int rear_items(uint32_t radar,rear_item out[REAR_LIMIT],unsigned *count){
 uint32_t n=psp_read32(radar+288),pool=psp_read32(radar+268),list=psp_read32(radar+284);
 if(!n||n>REAR_LIMIT||psp_read32(radar+272)!=REAR_LIMIT||!memory(pool,REAR_LIMIT*644)||!memory(list,n*4))return 0;
 for(unsigned i=0;i<n;i++){
  unsigned slot=psp_read32(list+4*i);if(slot>=REAR_LIMIT)return 0;
  uint32_t p=pool+644*slot,q=p+216,set=psp_read32(q+208);rear_item *r=&out[i];
  if(psp_read8(p+642)||psp_read32(q)!=0x08b3786cu||psp_read8(q+24)!=1||psp_read32(q+176)!=25||!memory(set,60)||memcmp(psp_mem_ptr(set,11),"SpriteSet0",11))return 0;
  *r=(rear_item){.quad=q,.id=psp_read16(p+640),.colour=psp_read32(q+120),.alpha=psp_read8(q+128),.resource=set};
  for(unsigned j=0;j<i;j++)if(out[j].id==r->id||out[j].quad==q)return 0;
  memcpy(r->spatial,psp_mem_ptr(q+40,8),8);memcpy(r->spatial+2,psp_mem_ptr(q+64,32),32);
  memcpy(r->authored,psp_mem_ptr(q+48,16),16);memcpy(r->uv,psp_mem_ptr(q+180,16),16);
  for(unsigned j=0;j<10;j++){float value;memcpy(&value,&r->spatial[j],4);if(!isfinite(value))return 0;}
 }
 *count=n;return 1;
}
/* Original glyph25 scale is authored size times the alert frame counter, not
 * camera projection. Freeze it independently of the quantized alpha byte. */
static int rear_same(const rear_item *a,const rear_item *b){return a->id==b->id&&a->colour==b->colour&&a->alpha==b->alpha&&a->resource==b->resource&&!memcmp(a->spatial+2,b->spatial+2,8)&&!memcmp(a->authored,b->authored,16)&&!memcmp(a->uv,b->uv,16);}
typedef struct {uint32_t radar,context;unsigned n;rear_item input[REAR_LIMIT],output[REAR_LIMIT];} rear_capture;
static int prepare_rear(void *user){
 if(event_frame.valid&&!event_rear_prepare())return 0;
 rear_capture *c=user;for(unsigned i=0;i<16;i++){uint32_t v;memcpy(&v,rear_camera_pose.bytes+4*i,4);psp_write32(rear_camera_pose.address+4*i,v);}
 psp_write_f32(c->context+12,0);return 1;
}
static int capture_rear(void *user){
 rear_capture *c=user;rear_item result[REAR_LIMIT];unsigned n;
 if(!psp_read8(c->radar+252)||psp_read8(c->radar+107)||!rear_items(c->radar,result,&n)||n!=c->n)return 0;
 for(unsigned i=0;i<n;i++){
  unsigned j=0;while(j<n&&result[j].id!=c->input[i].id)j++;
  if(j==n||!rear_same(&c->input[i],&result[j]))return 0;
  c->output[i]=c->input[i];memcpy(c->output[i].spatial,result[j].spatial,40);
 }
 return 1;
}
int asphalt_native_engine_rear_enter(uint32_t radar){
 if(asphalt_preview_active())return 0;
 unsigned slot=rear_depth++;if(slot>=REAR_SCOPE_LIMIT)return 0;rear_scope *scope=&rear_scopes[slot];scope->n=0;
 if(!configured||!enabled||!in_root||(!packets.pending&&!event_visual_ready())||!camera_overlay||rear_camera_pose.size!=64||!memory(radar,344)||psp_read32(radar)!=0x08b3ac14u||psp_read32(radar+32)!=21||!psp_read8(radar+252)||psp_read8(radar+107)||psp_read8(radar+254)||psp_read8(radar+255)||!alive())return 0;
 uint32_t owner=psp_read32(radar+16),context=psp_read32(radar+12),controller=psp_read32(radar+264);
 if(!memory(owner,248)||psp_read32(owner)!=0x08b3ac6cu||psp_read32(owner+244)!=radar||!memory(context,16)||!memory(controller,56)||psp_read32(controller+36)!=camera_overlay)return 0;
 uint32_t list=psp_read32(bound.car[11].object+20);if(!memory(list,4)||!memory(psp_read32(list),12)||psp_read32(psp_read32(list)+8)!=controller)return 0;
 uint32_t calc=psp_read32(controller+52);if(!memory(calc,28)||psp_read32(calc)!=0x08b3b004u)return 0;
 uint32_t source=psp_read32(calc+24);if(!memory(source,96)||psp_read32(source)!=0x08b3aed8u||psp_read32(source+12)!=1||source+32!=rear_camera_pose.address)return 0;
 rear_capture c={.radar=radar,.context=context};if(!rear_items(radar,c.input,&c.n))return 0;
 if(rear_cache.attempted&&rear_cache.frame==counts.frames&&rear_cache.radar==radar){
  if(!rear_cache.valid||rear_cache.n!=c.n)return 0;
  for(unsigned i=0;i<c.n;i++)if(c.input[i].quad!=rear_cache.input[i].quad||!rear_same(&c.input[i],&rear_cache.input[i])||
    (memcmp(c.input[i].spatial,rear_cache.input[i].spatial,40)&&memcmp(c.input[i].spatial,rear_cache.output[i].spatial,40)))return 0;
  memcpy(c.output,rear_cache.output,c.n*sizeof *c.output);
 }else{
  rear_cache.frame=counts.frames;rear_cache.radar=radar;rear_cache.attempted=1;rear_cache.valid=0;
  float zero[3]={0};if(!asphalt_preview_call_prepared(0x08a13b50u,radar,0,zero,prepare_rear,capture_rear,&c))return 0;
  rear_cache.n=c.n;memcpy(rear_cache.input,c.input,c.n*sizeof *c.input);memcpy(rear_cache.output,c.output,c.n*sizeof *c.output);rear_cache.valid=1;
 }
 scope->n=c.n;memcpy(scope->items,c.input,c.n*sizeof *c.input);
 for(unsigned i=0;i<c.n;i++){uint32_t q=c.input[i].quad;memcpy(psp_mem_ptr(q+40,8),c.output[i].spatial,8);memcpy(psp_mem_ptr(q+64,32),c.output[i].spatial+2,32);}
 return 1;
}
void asphalt_native_engine_rear_leave(void){
 if(asphalt_preview_active()||!rear_depth)return;unsigned slot=--rear_depth;if(slot>=REAR_SCOPE_LIMIT)return;
 rear_scope *scope=&rear_scopes[slot];for(unsigned i=0;i<scope->n;i++){rear_item *p=&scope->items[i];memcpy(psp_mem_ptr(p->quad+40,8),p->spatial,8);memcpy(psp_mem_ptr(p->quad+64,32),p->spatial+2,32);}scope->n=0;
}
static int vehicle(void){
 int authoritative=stage||packets.pending;if(authoritative)event_frame.coarse=1;
 /* Original input/race callbacks can pause or alter a group in this packet.
  * In that case run the original group once with its full coarse vector;
  * do not apply the normal-race split to the new timing contract. */
 schema current;
 if(!inspect(bound.owner,&current,1,"vehicle_schema")){
  int result=stage?invoke(bound.root[6],root_dt,1):finish_tail(1);
  restore_coarse_clock();forget();return result;
 }
 if(stage){
  float effective[3];memcpy(effective,root_dt,12);float scale=psp_read_f32(bound.vehicle+52);effective[0]=fminf(effective[0]*scale,.079999f);effective[2]*=scale;event_frame.packet_scale=scale;
  for(unsigned i=0;i<2;i++)if(invoke(bound.car[i],effective,1))return -1;
  if(asphalt_native_packets_run(&packets,effective,50,split_packet,emit,NULL))return -1;
  if(!packets.pending){if(suffix())return -1;finished=1;}
  else refresh_car_poses();
 }else if(finish_tail(0))return -1;
 if(!bound.owner)return -1;
 counts.visuals++;if(invoke(bound.car[13],native_dt,0))return -1;
 /* A new split packet retains its tail until the NEXT native slot. Only the
  * final event after the scene belongs here when post work already finished. */
 if(finished){finished=0;if(invoke(bound.car[14],packets.sim,1))return -1;}
 if(authoritative)event_commit();
 return 0;
}
int asphalt_native_engine_task(uint32_t pc,uint32_t object,uint32_t delta,uint32_t mode,uint32_t record,uint32_t parent){
 if(manual_depth||!enabled||!in_root||parent!=bound.owner+32){notify(pc,object,delta,mode);return 0;}
 if(!identities()){restore_coarse_clock();forget();notify(pc,object,delta,mode);return 0;}
 if(object==bound.render){notify(pc,object,delta,mode);return 0;}
 int failed=0;
 if(object==bound.vehicle)failed=vehicle();
 else if(stage){
  task t={pc,object,mode,record,-1,1};failed=invoke(t,root_dt,1);
 }
 if(failed){restore_coarse_clock();forget();}
 return 1;
}
void asphalt_native_engine_root_leave(uint32_t owner){
 restore_poses();
 if(deferred_owner==owner){
  psp_write32(owner,deferred_tick);psp_write_f32(owner+12,deferred_delta);deferred_owner=0;
 }
 in_root=in_group=0;
 apply_requested_rate();
}
static int owns_task_storage(const task *items,unsigned n,uint32_t object){
 if(object==items[0].record)return 1;
 for(unsigned i=0;i<n;i++)if(items[i].active&&object==items[i].object)return 1;
 return 0;
}
static int owns_group_storage(uint32_t group_address,uint32_t object){return memory(group_address,20)&&object==psp_read32(group_address+16);}
void asphalt_native_engine_released(uint32_t object){
 observed_source *sources[2]={&observation.model,&observation.view};
 for(unsigned i=0;i<2;i++){observed_source *s=sources[i];
  if(object&&(object==s->id||object==s->source||object==s->parent||object==s->storage||object==s->auxiliary))retire_source(s);
 }
 /* Every free hook runs before deallocation. Restore while ALL saved ranges
  * still exist, then retain no pointers into a potentially reused allocation. */
 restore_poses();feedback_release(object);event_release(object);recovery_release(object);
 if(object==deferred_owner)deferred_owner=0;
 if(!bound.owner||!object)return;
 if(object==bound.owner||object==bound.owner+32||
    owns_task_storage(bound.root,8,object)||owns_task_storage(bound.car,15,object)||owns_task_storage(bound.physical,2,object)||owns_task_storage(bound.draw,8,object)||
    owns_group_storage(bound.owner+32,object)||owns_group_storage(bound.vehicle,object)||owns_group_storage(bound.physics,object)||owns_group_storage(bound.render,object)){
  restore_coarse_clock();if(object==bound.owner)deferred_owner=0;forget();
 }
}
