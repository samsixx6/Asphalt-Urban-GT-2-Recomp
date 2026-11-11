#include "race_controls.h"
#include "psprecomp/mem.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

/* Race identity survives cinematic input suspension. Separate read-only
 * original countdown and fatal-camera witnesses control touch visibility;
 * native60 pacing and jumps do not determine it. */
typedef struct {uint32_t object,record;} item;
typedef struct {uint32_t at,descriptor,list,dynamic;item items[15];} group;
typedef struct {
 uint32_t owner,world,controllers,actors,actor_count,controller,actor,state,status,input;
 uint32_t actor_ids[128];
 group root,vehicle;
} proof;
static proof bound,retired,screen_proof;
static int configured,automatic_controls,has_bound,has_retired,screen_blocked,has_screen_proof,saw_pause;
static uint32_t in_root;
static atomic_int active,racing,hidden,loading_hidden;

/* 088DF0A4 draws the original loading gauge from this temporary context.
 * Its worker can yield inside one root call, so loading publication must be
 * independent of the root's race/cinematic publication. Guest callbacks are
 * serialized by the CPU owner; the Android reader sees atomic flags only. */
typedef struct {uint32_t context,compositor,renderer,resource,sprites[2];} loading_view;
static loading_view loading;
typedef struct {uint32_t race,counter,manager,list,child,controller,camera;} cinematic_view;
static cinematic_view cinematic;
static uint32_t crash_wrappers[128];
static int initial_complete;
typedef struct {uint32_t address,size;} allocation;
static allocation pending[64];
static unsigned pending_count,generation,screen_generation;

static int memory(uint32_t at,unsigned size){
 if(at<0x08800000u||size>0x1800000u||at>0x0a000000u-size)return 0;
 for(unsigned i=0;i<pending_count;i++)
  if(at<pending[i].address+pending[i].size&&pending[i].address<at+size)return 0;
 return psp_mem_ptr(at,size)!=NULL;
}
void asphalt_race_controls_loading_end(uint32_t context){
 if(context!=loading.context)return;
 memset(&loading,0,sizeof loading);atomic_store_explicit(&loading_hidden,0,memory_order_release);
}
static int inspect_loading(uint32_t context,loading_view *out){
 loading_view next={0};
 /* 088DEAD8 initializes +16/+20; 088DED54 binds +44 from compositor+48.
  * 088DEFCC invokes the draw only with +17 enabled. Mode4 returns before
  * renderer work; the other original modes0/1/2/3/5 draw owned sprites.
  * A screen request, FPS, elapsed time or an untyped busy flag proves none
  * of this and cannot hide the controls. */
 if(!configured||(context&3)||!memory(context,60)||psp_read8(context+16)!=1||psp_read8(context+17)!=1)return 0;
 unsigned mode=psp_read32(context+36);if(mode>5||mode==4)return 0;
 next.context=context;next.compositor=psp_read32(context+20);next.renderer=psp_read32(context+44);next.resource=psp_read32(context+40);
 if(!memory(next.compositor,84)||psp_read32(next.compositor)!=0x08b382b8u||
    !memory(0x08b382b8u,20)||psp_read32(0x08b382b8u+16)!=0x088dd2fcu||
    !memory(next.renderer,40)||psp_read32(next.compositor+48)!=next.renderer||!memory(next.resource,4))return 0;
 unsigned owned=0;
 for(unsigned i=0;i<2;i++){
  next.sprites[i]=psp_read32(context+28+4*i);uint32_t sprite=next.sprites[i];
  if(!sprite)continue;
  /* The actual 088BE03C sprite constructor records self+24/resource+28. */
  if(!memory(sprite,76)||psp_read32(sprite+24)!=sprite||psp_read32(sprite+28)!=next.resource)return 0;
  owned++;
 }
 if(!owned)return 0;
 *out=next;return 1;
}
void asphalt_race_controls_loading_draw(uint32_t context){
 loading_view next;
 if(!inspect_loading(context,&next)){asphalt_race_controls_loading_end(loading.context);return;}
 /* Refresh a valid draw without a false visible interval for the UI reader. */
 loading=next;atomic_store_explicit(&loading_hidden,1,memory_order_release);
}
static uint32_t method(uint32_t object){
 if(!memory(object,4))return 0;
 uint32_t vt=psp_read32(object);
 return memory(vt,20)?psp_read32(vt+16):0;
}
static int inspect_group(uint32_t at,group *out,unsigned count,const uint32_t *callbacks,int car,int *running,int *paused){
 if(!memory(at,68)||method(at)!=0x08879d38u)return 0;
 out->at=at;out->descriptor=psp_read32(at+16);out->dynamic=psp_read32(at+28);
 if(!memory(out->descriptor,8)||psp_read32(out->descriptor+4)!=count)return 0;
 out->list=psp_read32(out->descriptor);
 if(!memory(out->list,count*24))return 0;
 if(out->dynamic&&!memory(out->dynamic,1))return 0;
 /* 08879D38 uses +48/+52 as delta cap/scale. The vehicle group's +56/+57
  * also suspend/select clocks during events: 089B88C0 sets +56 while the
  * verified original input context remains race mode 3, and the captured
  * intro uses +57. Neither flag alone proves the Pause menu. Root/dynamic
  * pause remains authoritative; the verified player context below separates
  * original race modes 3/4 from the captured Pause context 5. */
 float scale=psp_read_f32(at+52),limit=psp_read_f32(at+48);
 if(!isfinite(scale)||scale<0||!isfinite(limit)||limit<=0)return 0;
 if(car&&(psp_read8(at+56)>1||psp_read8(at+57)>1))return 0;
 if((!car&&(psp_read8(at+56)||psp_read8(at+57)))||(out->dynamic&&psp_read8(out->dynamic))){*running=0;*paused=1;}
 for(unsigned i=0;i<count;i++){
  item *t=&out->items[i];t->record=out->list+24*i;t->object=psp_read32(t->record+20);
  unsigned mode=car&&((i==5)||(i==6))?2u:0u;
  /* 08879BD0 toggles the vehicle jobs for original intro/event scheduling.
   * Their identities and descriptors remain required even while disabled. */
  unsigned enabled=psp_read8(t->record);
  if(method(t->object)!=callbacks[i]||(car?enabled>1:enabled!=(i!=1))||
     psp_read32(t->record+16)!=mode||psp_read_f32(t->record+12)!=(car&&i==4?50.0f:-1.0f))return 0;
 }
 return 1;
}
static int inspect(uint32_t owner,proof *out,int *running,int *paused){
 static const uint32_t root_pc[8]={0x0887a448,0x08879d38,0x088e4fa8,0x0896ba24,0x089573b0,0x088f632c,0x08879d38,0x08879d38};
 static const uint32_t car_pc[15]={0x089cb930,0x0897bc60,0x0892082c,0x0883ad34,0x08879d38,0x0892082c,0x0883ad34,0x089794c0,0x08a2ef74,0x0885d02c,0x089a4028,0x08a34640,0x089ba7f0,0x088c7688,0x089d2430};
 memset(out,0,sizeof *out);*running=1;*paused=0;out->owner=owner;
 if(!memory(owner,120)||!inspect_group(owner+32,&out->root,8,root_pc,0,running,paused))return 0;
 if(psp_read8(owner+20)){*running=0;*paused=1;}
 float minimum=psp_read_f32(owner+4);
 if(minimum!=.03f&&minimum!=.015f)*running=0;
 if(!inspect_group(out->root.items[6].object,&out->vehicle,15,car_pc,1,running,paused))return 0;
 group *g=&out->vehicle;out->world=g->items[2].object;
 if(g->items[5].object!=out->world||g->items[3].object!=g->items[6].object||!memory(out->world,0x289e4))return 0;
 out->controllers=psp_read32(out->world+0x289c0);out->actors=psp_read32(out->world+0x289dc);
 unsigned count=psp_read32(out->world+0x289e0);out->actor_count=count;
 /* Original single-player controller collection. Unknown collections close
  * the layout instead of guessing which AI/player actor owns input. */
 if(psp_read32(out->world+0x289c4)!=1||!memory(out->controllers,4)||!count||count>128||!memory(out->actors,count*4))return 0;
 out->controller=psp_read32(out->controllers);
 if(!memory(out->controller,160)||psp_read32(out->controller)!=0x08b385c8u)return 0;
 uint32_t c=out->controller;out->actor=psp_read32(c+4);out->state=psp_read32(c+76);out->status=psp_read32(c+60);out->input=psp_read32(c+64);
 if(!memory(out->actor,1256)||!memory(out->state,20)||!memory(out->status,24)||!memory(out->input,40)||
    psp_read32(out->input)!=0x08b3b65cu||psp_read32(out->state)!=0x08b385b8u||psp_read32(c+156)!=out->state||
    psp_read32(out->actor+1248)!=c||psp_read32(out->actor+1252)!=out->state)return 0;
 int found=0;for(unsigned i=0;i<count;i++){out->actor_ids[i]=psp_read32(out->actors+4*i);if(out->actor_ids[i]==out->actor)found++;}
 if(found!=1)return 0;
 uint32_t mode=psp_read32(out->status+20);
 /* 089213E4 -> 089114CC -> 08901AD0 disables controller +48 during
  * countdown and cinematics. State +4 gates command processing in 0891155C.
  * Neither changes the verified race/player lifetime or layout identity. */
 if(psp_read32(0x08b3eb14u)!=52||
    psp_read32(0x08b3ee9cu)!=3||psp_read32(0x08b3eea0u)!=4)*running=0;
 /* Actual captured Race_Pause uses context 5. Only a known input schema
  * can grant this pause->resume witness; cinematic flags and unknown schema
  * must not revive a screen-invalidated graph. */
 else if(mode==5){*running=0;*paused=1;}
 else if(mode!=3&&mode!=4)*running=0;
 return 1;
}
static void publish(int value){
 atomic_store_explicit(&racing,value!=0,memory_order_release);
 atomic_store_explicit(&active,value&&automatic_controls,memory_order_release);
 if(!value)atomic_store_explicit(&hidden,0,memory_order_release);
}
static int same(const proof *a,const proof *b){return !memcmp(a,b,sizeof *a);}
void asphalt_race_controls_set_automatic(int automatic){
 if(automatic!=0&&automatic!=1)return;
 automatic_controls=automatic;
 publish(atomic_load_explicit(&racing,memory_order_acquire));
}
static int wrapper_index(const proof *p,uint32_t wrapper){
 if(!memory(wrapper,1120)||psp_read32(wrapper)!=0x08b39b5cu||psp_read32(wrapper+524)!=p->world||!memory(p->actors,p->actor_count*4))return -1;
 uint32_t actor=psp_read32(wrapper+1116);
 if(!memory(actor,1256))return -1;
 for(unsigned i=0;i<p->actor_count;i++)if(psp_read32(p->actors+4*i)==actor)return (int)i;
 return -1;
}
static int cinematic_hidden(const proof *p){
 cinematic_view v={0};v.race=p->vehicle.items[7].object;
 if(!memory(v.race,224)||psp_read32(v.race)!=0x08b39828u)return 0;
 v.counter=psp_read32(v.race+220);
 if(!memory(v.counter,28)||psp_read8(v.counter)>1||psp_read8(v.counter+9)>1)return 0;
 cinematic.race=v.race;cinematic.counter=v.counter;
 /* Original 08979FA4 marks GO at +9. Later events reuse this counter;
  * completion belongs to this verified race lifetime. */
 if(psp_read8(v.counter+9))initial_complete=1;
 if(!initial_complete)return 1;
 v.manager=p->vehicle.items[11].object;
 if(!memory(v.manager,28)||psp_read32(v.manager)!=0x08b3b148u||method(v.manager)!=0x08a34640u||psp_read32(v.manager+24)!=1)return 0;
 v.list=psp_read32(v.manager+20);if(!memory(v.list,4))return 0;
 v.child=psp_read32(v.list);if(!memory(v.child,12)||psp_read32(v.child)!=0x08b3b164u||psp_read32(0x08b3b164u+12)!=0x08a352e0u)return 0;
 v.controller=psp_read32(v.child+8);
 if(!memory(v.controller,224)||psp_read32(v.controller)!=0x08b3ae98u||method(v.controller)!=0x08a1fce4u||
    psp_read32(v.controller+212)!=v.race||psp_read32(v.controller+220)!=p->world)return 0;
 v.camera=psp_read32(v.controller+176);
 if(!memory(v.camera,560)||psp_read32(v.camera)!=0x08b3b05cu||psp_read32(v.camera+4)!=9)return 0;
 cinematic=v;
 if(psp_read32(v.controller+68)!=v.camera||psp_read32(v.camera+68)!=1)return 0;
 for(unsigned i=0;i<2;i++){
  uint32_t wrapper=psp_read32(v.camera+72+4*i);int index=wrapper_index(p,wrapper);
  if(index>=0&&crash_wrappers[index]==wrapper&&psp_read32(v.camera+120+4*i)==psp_read32(wrapper+1116)+192)return 1;
 }
 return 0;
}
void asphalt_race_controls_script(uint32_t interpreter,uint32_t program,uint32_t return_address){
 /* Observe original 0899C398. Any replacement revokes the producer witness.
  * The interpreter PC advances and cannot serve as script identity. */
 if(interpreter<400)return;
 uint32_t wrapper=interpreter-400;
 for(unsigned i=0;i<128;i++)if(crash_wrappers[i]==wrapper)crash_wrappers[i]=0;
 if(!configured||!has_bound||in_root!=bound.owner||has_retired||screen_blocked||!program||!memory(program,1))return;
 switch(return_address){
 case 0x089991a4u:case 0x089a0220u:case 0x089a0384u:case 0x089a0530u:case 0x089a062cu:
 case 0x089a07e4u:case 0x089a0e7cu:case 0x089a0fc8u:case 0x089a10fcu:break;
 default:return;
 }
 int index=wrapper_index(&bound,wrapper);if(index>=0)crash_wrappers[index]=wrapper;
}
static void observe(uint32_t owner){
 proof next;int running,paused;
 if(!inspect(owner,&next,&running,&paused)){publish(0);return;}
 if(has_retired&&pending_count&&same(&next,&retired)){publish(0);return;}
 if(has_retired){has_retired=0;pending_count=0;generation++;}
 if(screen_blocked){
  if(has_screen_proof&&same(&next,&screen_proof)){
   if(paused)saw_pause=1;
   if(!saw_pause&&screen_generation==generation)running=0;
   else if(running)screen_blocked=0;
  }else screen_blocked=0;
 }
 if(!has_bound||!same(&bound,&next)){initial_complete=0;memset(&cinematic,0,sizeof cinematic);memset(crash_wrappers,0,sizeof crash_wrappers);}
 bound=next;has_bound=1;publish(running);
 if(running)atomic_store_explicit(&hidden,cinematic_hidden(&next),memory_order_release);
}
void asphalt_race_controls_configure(int automatic){
 configured=automatic==0||automatic==1;automatic_controls=automatic==1;has_bound=has_retired=screen_blocked=has_screen_proof=saw_pause=0;in_root=0;atomic_store(&hidden,0);
 pending_count=generation=screen_generation=0;
 initial_complete=0;memset(&cinematic,0,sizeof cinematic);memset(crash_wrappers,0,sizeof crash_wrappers);
 memset(&loading,0,sizeof loading);atomic_store(&loading_hidden,0);
 memset(&bound,0,sizeof bound);memset(&retired,0,sizeof retired);memset(&screen_proof,0,sizeof screen_proof);publish(0);
}
void asphalt_race_controls_root_enter(uint32_t owner){
 in_root=configured?owner:0;
 if(configured)observe(owner);else publish(0);
}
void asphalt_race_controls_root_leave(uint32_t owner){
 /* A release may have retired this call. Never follow its cached identities
  * after deallocation, even while the old bytes still resemble a race. */
 if(configured&&in_root&&in_root==owner)observe(owner);else publish(0);
 in_root=0;
}
static int group_owns(const group *g,unsigned count,uint32_t object){
 if(object==g->at||object==g->descriptor||object==g->list||object==g->dynamic)return 1;
 for(unsigned i=0;i<count;i++)if(object==g->items[i].object)return 1;
 return 0;
}
static unsigned extent(const proof *p,uint32_t object){
 if(object==p->owner)return 120;
 if(object==p->world)return 0x289e4;
 if(object==p->controller)return 160;
 if(object==p->actor)return 1256;
 if(object==p->state)return 20;
 if(object==p->status)return 24;
 if(object==p->input)return 40;
 if(object==p->controllers)return 4;
 if(object==p->actors)return p->actor_count*4;
 if(object==p->vehicle.items[7].object)return 224;
 const group *groups[]={&p->root,&p->vehicle};
 for(unsigned i=0;i<2;i++){
  const group *g=groups[i];unsigned count=i?15:8;
  if(object==g->at)return 68;
  if(object==g->descriptor)return 8;
  if(object==g->list)return count*24;
  if(object==g->dynamic)return 1;
 }
 return 4;
}
void asphalt_race_controls_released(uint32_t object){
 unsigned extra=0,loading_extra=0;
 if(object&&loading.context){
  if(object==loading.context)loading_extra=60;
  if(object==loading.compositor)loading_extra=84;
  if(object==loading.renderer)loading_extra=40;
  if(object==loading.resource)loading_extra=4;
  for(unsigned i=0;i<2;i++)if(object==loading.sprites[i])loading_extra=76;
  if(loading_extra)asphalt_race_controls_loading_end(loading.context);
 }
 for(unsigned i=0;i<128;i++)if(object&&crash_wrappers[i]==object){extra=1120;crash_wrappers[i]=0;atomic_store_explicit(&hidden,0,memory_order_release);}
 if(has_bound)for(unsigned i=0;i<bound.actor_count;i++)if(object&&bound.actor_ids[i]==object){extra=1256;crash_wrappers[i]=0;atomic_store_explicit(&hidden,0,memory_order_release);}
 if(object&&object==cinematic.counter)extra=28;
 if(object&&object==cinematic.list)extra=4;
 if(object&&object==cinematic.child)extra=12;
 if(object&&object==cinematic.controller)extra=224;
 if(object&&object==cinematic.camera)extra=560;
 if(extra||loading_extra){
  if(object==cinematic.list||object==cinematic.child||object==cinematic.controller||object==cinematic.camera)memset(crash_wrappers,0,sizeof crash_wrappers);
  unsigned i=0;for(;i<pending_count;i++)if(pending[i].address==object)break;
  unsigned size=extra>loading_extra?extra:loading_extra;
  if(i==pending_count&&pending_count<64)pending[pending_count++]=(allocation){object,size};
  if(extra)atomic_store_explicit(&hidden,0,memory_order_release);
 }
 if(!object||(!has_bound&&!has_retired))return;
 const proof *p=has_bound?&bound:&retired;
 if(object==p->owner||object==p->world||object==p->controllers||object==p->actors||
    object==p->controller||object==p->actor||object==p->state||object==p->status||object==p->input||
    group_owns(&p->root,8,object)||group_owns(&p->vehicle,15,object)){
  unsigned i=0;for(;i<pending_count;i++)if(pending[i].address==object)break;
  if(i==pending_count&&pending_count<64)pending[pending_count++]=(allocation){object,extent(p,object)};
  if(has_bound)retired=bound;
  has_retired=1;has_bound=0;in_root=0;publish(0);
 }
}
void asphalt_race_controls_allocated(uint32_t address,uint32_t size){
 /* Allocation is evidence of a new lifetime, not evidence of a race. No
  * reads happen here; the next root must validate the reconstructed graph. */
 if(address<0x08800000u||size>0x1800000u||address>0x0a000000u-size)return;
 unsigned previous=pending_count;
 for(unsigned i=0;i<pending_count;){
  allocation p=pending[i];
  if(p.address>=address&&p.address-address<=size&&p.size<=size-(p.address-address))pending[i]=pending[--pending_count];
  else i++;
 }
 /* Retain integer identities through reconstruction: another member can be
  * freed after this allocation and before the next root establishes proof. */
 if(previous!=pending_count)generation++;
}
void asphalt_race_controls_screen(const char *name){
 /* Every requested screen, including unknown/empty names, revokes current
  * proof immediately. Same-graph reactivation requires an observed pause /
  * resume; a newly verified race graph can establish its own eligibility. */
 (void)name;screen_blocked=1;has_screen_proof=has_bound;
 if(has_bound)screen_proof=bound;
 saw_pause=0;screen_generation=generation;publish(0);
}
int asphalt_race_controls_active(void){return atomic_load_explicit(&active,memory_order_acquire);}
int asphalt_race_controls_racing(void){return atomic_load_explicit(&racing,memory_order_acquire);}
int asphalt_race_controls_hidden(void){return atomic_load_explicit(&hidden,memory_order_acquire)||atomic_load_explicit(&loading_hidden,memory_order_acquire);}
