/* Included by render_gl.c: renderer-owned visual transforms and shadow GPU
 * history. No guest calls, guest clocks, or simulation state. */
typedef struct { gstate state;psp_render_transform pose;uint32_t first,count,hash;int w,h;size_t next; } scene_draw;
static scene_draw *g_scene_prev;
static unsigned char *g_scene_used;
static size_t g_scene_nprev,g_scene_capprev;
static gbuf g_scene_prev_vertices,g_scene_between;
static uint64_t g_scene_last,g_scene_native_count,g_scene_mid_count,g_scene_matched,g_scene_changed;
static rfb *g_scene_targets;static int g_scene_target_count;
static unsigned char *g_scene_needed;static size_t g_scene_needed_cap;
static GLuint g_scene_vao,g_scene_vbo;
static size_t *g_scene_buckets,g_scene_bucket_count;
typedef struct { uint32_t hash;unsigned count; } scene_key_count;
static scene_key_count *g_scene_current_keys;static size_t g_scene_current_cap;
static int g_scene_profile=-1;
static uint64_t g_scene_swap_lead;
static uint64_t g_scene_wait_us,g_scene_swap_us,g_scene_profile_sum[6];static unsigned g_scene_profile_frames;
#ifdef ASPHALT_GPU_TESTS
#include <stdatomic.h>
static atomic_int g_scene_test_waiting;
int psp_render_gl_test_scene_waiting(void){return atomic_load(&g_scene_test_waiting);}
#endif
void psp_render_gl_scene_stats(uint64_t *native_scenes,uint64_t *midpoints,uint64_t *matched_vertices,uint64_t *changed_vertices){
 host_mutex_lock(&g_m);if(native_scenes)*native_scenes=g_scene_native_count;if(midpoints)*midpoints=g_scene_mid_count;
 if(matched_vertices)*matched_vertices=g_scene_matched;if(changed_vertices)*changed_vertices=g_scene_changed;host_mutex_unlock(&g_m);
}

static int scene_wait(uint64_t deadline){
 uint64_t start=g_scene_profile>0?host_pacing_now():0;
#ifdef ASPHALT_GPU_TESTS
 const char *hold=getenv("ASPHALT_GPU_TEST_SCENE_HOLD");if(hold&&*hold=='1')deadline=host_pacing_now()+5000000;
#endif
 host_mutex_lock(&g_m);
 while(!g_quit&&host_pacing_now()<deadline){
#ifdef ASPHALT_GPU_TESTS
  atomic_store(&g_scene_test_waiting,1);
#endif
  host_pacing_until(&g_native_wait,&g_m,deadline);
#ifdef ASPHALT_GPU_TESTS
  atomic_store(&g_scene_test_waiting,0);
#endif
 }
 int ok=!g_quit;host_mutex_unlock(&g_m);if(start)g_scene_wait_us+=host_pacing_now()-start;return ok;
}
static void scene_present(const c_present *p){uint64_t start=host_pacing_now();present(p);uint64_t elapsed=host_pacing_now()-start;if(g_scene_profile)g_scene_swap_us+=elapsed;if(g_host.swap_interval)g_scene_swap_lead=(g_scene_swap_lead*7+(elapsed>16667?16667:elapsed))/8;}
static void scene_dimensions(const gframe *f,int id,int *w,int *h){
 *w=*h=0;
 if(id>=0&&id<g_nrfb&&g_rfb[id].fbo){*w=g_rfb[id].w/g_scale;*h=g_rfb[id].h/g_scale;return;}
 for(size_t at=0;at<f->cmd.n;){const chead *c=(const chead *)(f->cmd.p+at);at+=c->size;
  if(c->op==C_FB_NEW){const c_fb_new *n=(const c_fb_new *)c;if(n->id==id){*w=n->w;*h=n->hgt;return;}}
 }
}
static int scene_same_material(const gstate *a,const gstate *b){
 gstate x=*a,y=*b;
 /* Front-buffer resource IDs alternate. Their role/material flags stay fixed. */
 if(x.tex_kind>=2&&y.tex_kind==x.tex_kind)x.tex_id=y.tex_id=0;
 return !memcmp(&x,&y,sizeof x);
}
static uint32_t scene_hash(const psp_render_transform *t,const gstate *state,unsigned count,int w,int h){
 uint32_t hash=2166136261u,words[]={t->mesh,t->indices,t->vertex_format,t->count,t->primitive,count,(uint32_t)w,(uint32_t)h,(uint32_t)t->instance,(uint32_t)(t->instance>>32)};
 for(unsigned i=0;i<sizeof words/sizeof words[0];i++)hash=(hash^words[i])*16777619u;
 gstate s=*state;if(s.tex_kind>=2)s.tex_id=0;
 for(size_t at=0;at<sizeof s;at+=4){uint32_t word;memcpy(&word,(const unsigned char *)&s+at,4);hash=(hash^word)*16777619u;}
 return hash;
}
static unsigned scene_current_count(uint32_t hash){
 size_t slot=hash&(g_scene_current_cap-1);
 while(g_scene_current_keys[slot].count&&g_scene_current_keys[slot].hash!=hash)slot=(slot+1)&(g_scene_current_cap-1);
 return g_scene_current_keys[slot].count;
}
static void scene_index_current(const gframe *f){
 size_t draws=0;for(size_t at=0;at<f->cmd.n;){const chead *h=(const chead *)(f->cmd.p+at);at+=h->size;if(h->op==C_DRAW&&((const c_draw *)h)->transform.valid)draws++;}
 size_t needed=16;while(needed<draws*2)needed*=2;
 if(needed>g_scene_current_cap){g_scene_current_keys=(scene_key_count *)realloc(g_scene_current_keys,needed*sizeof *g_scene_current_keys);if(!g_scene_current_keys)abort();g_scene_current_cap=needed;}
 memset(g_scene_current_keys,0,g_scene_current_cap*sizeof *g_scene_current_keys);int fb=-1;
 for(size_t at=0;at<f->cmd.n;){const chead *h=(const chead *)(f->cmd.p+at);at+=h->size;
  if(h->op==C_FB_BIND)fb=((const c_id *)h)->id;
  if(h->op!=C_DRAW||!((const c_draw *)h)->transform.valid)continue;
  const c_draw *d=(const c_draw *)h;int w,height;scene_dimensions(f,fb,&w,&height);
  uint32_t hash=scene_hash(&d->transform,&d->s,d->count,w,height);size_t slot=hash&(g_scene_current_cap-1);
  while(g_scene_current_keys[slot].count&&g_scene_current_keys[slot].hash!=hash)slot=(slot+1)&(g_scene_current_cap-1);
  g_scene_current_keys[slot].hash=hash;g_scene_current_keys[slot].count++;
 }
}
static int scene_between(const gframe *f,uint64_t now,uint64_t *matched,uint64_t *changed){
 unsigned rejected[6]={0};int sampled=0;
 *matched=*changed=0;gbuf_copy(&g_scene_between,&f->vtx);
 if(!g_scene_nprev||!g_scene_last||now<g_scene_last||now-g_scene_last>100000)return 0;
 scene_index_current(f);
 memset(g_scene_used,0,g_scene_nprev);
 gvert *dst=(gvert *)g_scene_between.p;const gvert *old=(const gvert *)g_scene_prev_vertices.p;
 int fb=-1;
 for(size_t at=0;at<f->cmd.n;){
  const chead *h=(const chead *)(f->cmd.p+at);at+=h->size;
  if(h->op==C_FB_BIND)fb=((const c_id *)h)->id;
  if(h->op!=C_DRAW)continue;
  const c_draw *d=(const c_draw *)h;const psp_render_transform *t=&d->transform;
  if(!t->valid)continue;
  int w,height;scene_dimensions(f,fb,&w,&height);if(!w||!height)continue;
  uint32_t hash=scene_hash(t,&d->s,d->count,w,height);
  if(scene_current_count(hash)!=1){rejected[0]++;continue;}
  size_t best=(size_t)-1;unsigned candidates=0;
  for(size_t j=g_scene_buckets[hash&(g_scene_bucket_count-1)];j!=(size_t)-1;j=g_scene_prev[j].next){
   const scene_draw *e=&g_scene_prev[j];const psp_render_transform *p=&e->pose;
   if(e->hash!=hash||g_scene_used[j]||!p->valid||e->w!=w||e->h!=height||e->count!=d->count||
      p->instance!=t->instance||p->mesh!=t->mesh||p->indices!=t->indices||p->vertex_format!=t->vertex_format||p->count!=t->count||p->primitive!=t->primitive||
      !scene_same_material(&e->state,&d->s))continue;
   best=j;if(++candidates>1)break;
  }
  /* A nearest-position guess can silently exchange two identical AI cars
   * when they cross. Without a unique counterpart, keep native geometry. */
  if(candidates!=1){rejected[1]++;continue;}
  const scene_draw *e=&g_scene_prev[best];psp_render_transform mid;
  /* Untagged geometry may use camera interpolation only while its world
   * pose is unchanged. It cannot inherit another culled car's world pose. */
  if(!t->instance&&memcmp(e->pose.world,t->world,sizeof t->world)){
   rejected[2]++;
   if(g_scene_profile&&g_scene_native_count%30==0&&!sampled++){
    fprintf(stderr,"scene-unknown-world: mesh=%08X old=",t->mesh);for(unsigned k=0;k<12;k++)fprintf(stderr," %g",e->pose.world[k]);fprintf(stderr," new=");for(unsigned k=0;k<12;k++)fprintf(stderr," %g",t->world[k]);fprintf(stderr,"\n");
   }
   continue;
  }
  if(!psp_transform_between(&e->pose,t,.5f,&mid)){
   rejected[3]++;
   if(g_scene_profile&&g_scene_native_count%30==0&&!sampled++)fprintf(stderr,"scene-rejected-transform: owner=%llX mesh=%08X proj0=%g/%g proj5=%g/%g viewport_changed=%d\n",(unsigned long long)t->instance,t->mesh,e->pose.proj[0],t->proj[0],e->pose.proj[5],t->proj[5],!!memcmp(e->pose.viewport,t->viewport,sizeof t->viewport));
   continue;
  }
  int valid=1;uint64_t moved=0;
  for(uint32_t i=0;i<d->count&&valid;i++){
   const gvert *a=old+e->first+i;gvert *b=dst+d->first+i;float c[4],raw_depth;
   if(!a->local_valid||!b->local_valid||memcmp(a->local,b->local,sizeof a->local)||
      (a->w>0)!=(b->w>0)||fabsf(ndc(a->x,a->w)-ndc(b->x,b->w))>.25f||fabsf(ndc(a->y,a->w)-ndc(b->y,b->w))>.25f||
      !psp_transform_project_depth(&mid,b->local,w,height,c,&raw_depth)){valid=0;break;}
   if(fabsf(c[0]-b->x)>1e-5f||fabsf(c[1]-b->y)>1e-5f||fabsf(c[2]-b->z)>1e-5f)moved++;
   if(d->s.mode[0]&MODE_DEPTHCLAMP){if(raw_depth<0)raw_depth=0;else if(raw_depth>65535)raw_depth=65535;}
   else if(raw_depth<0 || raw_depth>=65536){valid=0;break;}
   b->x=c[0];b->y=c[1];b->z=c[2];b->w=c[3];b->depth=raw_depth;b->fog=(a->fog+b->fog)*.5f;
  }
  if(!valid){rejected[4]++;memcpy(dst+d->first,(const gvert *)f->vtx.p+d->first,d->count*sizeof *dst);continue;}
  g_scene_used[best]=1;*matched+=d->count;*changed+=moved;
 }
 if(g_scene_profile&&g_scene_native_count%30==0)fprintf(stderr,"scene-match-rejects: current_duplicates=%u absent_or_ambiguous=%u unknown_owner=%u transform=%u local_or_jump=%u matched_vertices=%llu changed=%llu\n",rejected[0],rejected[1],rejected[2],rejected[3],rejected[4],(unsigned long long)*matched,(unsigned long long)*changed);
 return *changed>0;
}
static void scene_remember(const gframe *f,uint64_t at){
 gbuf_copy(&g_scene_prev_vertices,&f->vtx);g_scene_nprev=0;int fb=-1;
 for(size_t pos=0;pos<f->cmd.n;){const chead *h=(const chead *)(f->cmd.p+pos);pos+=h->size;
  if(h->op==C_FB_BIND)fb=((const c_id *)h)->id;
  if(h->op!=C_DRAW||!((const c_draw *)h)->transform.valid)continue;
  if(g_scene_nprev==g_scene_capprev){g_scene_capprev=g_scene_capprev?g_scene_capprev*2:1024;
   g_scene_prev=(scene_draw *)realloc(g_scene_prev,g_scene_capprev*sizeof *g_scene_prev);g_scene_used=(unsigned char *)realloc(g_scene_used,g_scene_capprev);
   if(!g_scene_prev||!g_scene_used){fputs("render_gl: out of memory\n",stderr);abort();}}
  const c_draw *d=(const c_draw *)h;scene_draw *e=&g_scene_prev[g_scene_nprev++];
  e->state=d->s;e->pose=d->transform;e->first=d->first;e->count=d->count;scene_dimensions(f,fb,&e->w,&e->h);e->hash=scene_hash(&e->pose,&e->state,e->count,e->w,e->h);
 }
 size_t needed=16;while(needed<g_scene_nprev*2)needed*=2;
 if(needed>g_scene_bucket_count){g_scene_buckets=(size_t *)realloc(g_scene_buckets,needed*sizeof *g_scene_buckets);if(!g_scene_buckets)abort();g_scene_bucket_count=needed;}
 for(size_t i=0;i<g_scene_bucket_count;i++)g_scene_buckets[i]=(size_t)-1;
 for(size_t i=0;i<g_scene_nprev;i++){scene_draw *e=&g_scene_prev[i];size_t b=e->hash&(g_scene_bucket_count-1);e->next=g_scene_buckets[b];g_scene_buckets[b]=i;}
 g_scene_last=at;
}
static void scene_drop_target(rfb *f){
 if(f->fbo)glDeleteFramebuffers(1,&f->fbo);if(f->tex)glDeleteTextures(1,&f->tex);if(f->depth)glDeleteRenderbuffers(1,&f->depth);
 if(f->feedback_fbo)glDeleteFramebuffers(1,&f->feedback_fbo);if(f->feedback_tex)glDeleteTextures(1,&f->feedback_tex);
 if(f->clut_fbo)glDeleteFramebuffers(1,&f->clut_fbo);if(f->clut_tex)glDeleteTextures(1,&f->clut_tex);memset(f,0,sizeof *f);
}
static int scene_clone(int id){
 if(id<0||id>=g_nrfb||!g_rfb[id].fbo)return 1;
 g_scene_targets=(rfb *)grow(g_scene_targets,&g_scene_target_count,id,sizeof *g_scene_targets);
 rfb *s=&g_rfb[id],*d=&g_scene_targets[id];
 if(d->fbo&&(d->w!=s->w||d->h!=s->h||d->fmt!=s->fmt))scene_drop_target(d);
 if(!d->fbo){
  d->w=s->w;d->h=s->h;d->fmt=s->fmt;d->tex=new_texture(d->w,d->h,NULL,GL_LINEAR);
  glGenRenderbuffers(1,&d->depth);glBindRenderbuffer(GL_RENDERBUFFER,d->depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,d->w,d->h);
  glGenFramebuffers(1,&d->fbo);glBindFramebuffer(GL_FRAMEBUFFER,d->fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,d->tex,0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,d->depth);
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE||glGetError()!=0){scene_drop_target(d);while(glGetError()!=0){}return 0;}
 }
 glDisable(GL_SCISSOR_TEST);glBindFramebuffer(GL_READ_FRAMEBUFFER,s->fbo);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,d->fbo);
 glBlitFramebuffer(0,0,s->w,s->h,0,0,d->w,d->h,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT,GL_NEAREST);
 if(glGetError()!=0)return 0;d->stencil_dirty=s->stencil_dirty;d->color_version++;return 1;
}
static int scene_prepare_targets(const gframe *f){
 int maximum=g_nrfb;
 for(size_t at=0;at<f->cmd.n;){const chead *h=(const chead *)(f->cmd.p+at);at+=h->size;
  if(h->op==C_FB_NEW){int n=((const c_fb_new *)h)->id+1;if(n>maximum)maximum=n;}}
 if((size_t)maximum>g_scene_needed_cap){g_scene_needed=(unsigned char *)realloc(g_scene_needed,(size_t)maximum);if(!g_scene_needed)abort();g_scene_needed_cap=(size_t)maximum;}
 memset(g_scene_needed,0,g_scene_needed_cap);
 for(size_t at=0;at<f->cmd.n;){const chead *h=(const chead *)(f->cmd.p+at);at+=h->size;int id=-1;
  if(h->op==C_FB_BIND)id=((const c_id *)h)->id;
  if(h->op==C_PRESENT)id=((const c_present *)h)->fb;
  if(id>=0&&id<maximum)g_scene_needed[id]=1;
  if(h->op==C_DRAW){const gstate *s=&((const c_draw *)h)->s;if(s->tex_kind>=2&&s->tex_id>=0&&s->tex_id<maximum)g_scene_needed[s->tex_id]=1;}}
 for(int id=0;id<g_nrfb;id++)if(g_scene_needed[id]&&!scene_clone(id))return 0;
 g_applied_valid=0;g_cur_fb_id=-1;g_cur_fb_h=0;return 1;
}
static void scene_draw_command(const c_draw *c,int target,int shadow){
 rfb *native=g_rfb;int native_count=g_nrfb;
 if(shadow){g_rfb=g_scene_targets;g_nrfb=g_scene_target_count;}
 if(target>=0&&target<g_nrfb&&g_rfb[target].fbo){
  if(g_cur_fb_id==target&&g_cur_fb_h==g_rfb[target].h)glBindFramebuffer(GL_FRAMEBUFFER,g_rfb[target].fbo);else fb_bind(target);
  /* Native/shadow targets have identical dimensions and draw state. Only
   * framebuffer-backed samplers change texture names when the bank changes;
   * ordinary mesh materials can retain their uniform/state cache. */
  if(c->s.tex_kind>=2)g_applied_valid=0;
  apply(&c->s);
  glBindVertexArray(shadow?g_scene_vao:g_vao);glBindBuffer(GL_ARRAY_BUFFER,shadow?g_scene_vbo:g_vbo);
  glDrawArrays(GL_TRIANGLES,(GLint)c->first,(GLsizei)c->count);
  if(c->s.stencil&&c->s.stencil_write&&(c->s.stencil_fail!=GL_KEEP||c->s.stencil_zfail!=GL_KEEP||c->s.stencil_zpass!=GL_KEEP))g_rfb[target].stencil_dirty=1;
 }
 g_rfb=native;g_nrfb=native_count;
}
static void scene_play(gframe *f,uint64_t end){
 if(g_scene_profile<0)g_scene_profile=getenv("ASPHALT_SCENE_PROFILE")?1:0;
 if(!g_scene_native_count)g_scene_swap_lead=g_host.swap_interval?16667:0;
 uint64_t started=g_scene_profile?host_pacing_now():0;g_scene_wait_us=g_scene_swap_us=0;
 const c_present *front=find_present(f);uint64_t matched=0,changed=0;
 int smooth=g_native_fps==60&&front&&front->fb>=0&&scene_between(f,host_pacing_now(),&matched,&changed);
 uint64_t posed=g_scene_profile?host_pacing_now():0;
 if(smooth&&!scene_prepare_targets(f)){static int warned;if(!warned++)fputs("render_gl: visual target unavailable; retaining original scene output\n",stderr);smooth=0;}
 uint64_t cloned=g_scene_profile?host_pacing_now():0;
 glUseProgram(g_prog);glBindVertexArray(g_vao);glBindBuffer(GL_ARRAY_BUFFER,g_vbo);if(f->vtx.n)upload_vertices(&f->vtx);
 if(smooth){
  if(!g_scene_vao){glGenVertexArrays(1,&g_scene_vao);glGenBuffers(1,&g_scene_vbo);glBindVertexArray(g_scene_vao);glBindBuffer(GL_ARRAY_BUFFER,g_scene_vbo);vertex_layout();}
  glBindBuffer(GL_ARRAY_BUFFER,g_scene_vbo);upload_vertices(&g_scene_between);
 }
 int target=-1;unsigned mid_shown=0;
 for(size_t at=0;at<f->cmd.n;){
  chead *h=(chead *)(f->cmd.p+at);at+=h->size;
  switch(h->op){
  case C_FB_BIND:target=((const c_id *)h)->id;break;
  case C_DRAW:
   scene_draw_command((const c_draw *)h,target,0);if(smooth)scene_draw_command((const c_draw *)h,target,1);break;
  case C_FB_NEW:{const c_fb_new *c=(const c_fb_new *)h;fb_new(c->id,c->w,c->hgt,c->fmt);if(smooth&&!scene_clone(c->id))smooth=0;break;}
  case C_FB_FREE:{int id=((const c_id *)h)->id;fb_free(id);if(id>=0&&id<g_scene_target_count)scene_drop_target(&g_scene_targets[id]);break;}
  case C_PRESENT:{
   const c_present *p=(const c_present *)h;uint64_t half=end>16667?end-16667:0;
   uint64_t lead=g_host.swap_interval?g_scene_swap_lead:0;
   if(smooth&&host_pacing_now()<=half+2000&&scene_wait(half>lead?half-lead:0)){
    rfb *native=g_rfb;int n=g_nrfb;g_rfb=g_scene_targets;g_nrfb=g_scene_target_count;scene_present(p);g_rfb=native;g_nrfb=n;mid_shown++;
   }
   lead=g_host.swap_interval?g_scene_swap_lead:0;
   if(scene_wait(end>lead?end-lead:0))scene_present(p);else free(p->pixels);
   scene_wait(end);
   target=-1;g_cur_fb_id=-1;g_cur_fb_h=0;break;
  }
  default:{gframe one={0};one.cmd.p=(uint8_t *)h;one.cmd.n=h->size;gbuf empty={0};play(&one,PLAY_MAKE|PLAY_FREE,&empty);break;}
  }
 }
 uint64_t played=g_scene_profile?host_pacing_now():0;
 if(front&&front->fb>=0)scene_remember(f,host_pacing_now());else{g_scene_nprev=0;g_scene_last=0;}
 host_mutex_lock(&g_m);g_scene_native_count+=!!front;g_scene_mid_count+=mid_shown;g_scene_matched+=matched;g_scene_changed+=changed;host_mutex_unlock(&g_m);
 glBindVertexArray(g_vao);glBindBuffer(GL_ARRAY_BUFFER,g_vbo);g_applied_valid=0;
 if(g_scene_profile){
  uint64_t finished=host_pacing_now(),values[]={posed-started,cloned-posed,played-cloned-g_scene_wait_us-g_scene_swap_us,g_scene_swap_us,g_scene_wait_us,finished-played};
  for(int i=0;i<6;i++)g_scene_profile_sum[i]+=values[i];
  if(++g_scene_profile_frames==30){fprintf(stderr,"scene-profile-us: transform=%llu clone=%llu submit=%llu swap=%llu wait=%llu remember=%llu\n",(unsigned long long)(g_scene_profile_sum[0]/30),(unsigned long long)(g_scene_profile_sum[1]/30),(unsigned long long)(g_scene_profile_sum[2]/30),(unsigned long long)(g_scene_profile_sum[3]/30),(unsigned long long)(g_scene_profile_sum[4]/30),(unsigned long long)(g_scene_profile_sum[5]/30));memset(g_scene_profile_sum,0,sizeof g_scene_profile_sum);g_scene_profile_frames=0;}
 }
}
static void scene_discard(void){
 free(g_scene_prev);g_scene_prev=NULL;free(g_scene_used);g_scene_used=NULL;g_scene_nprev=g_scene_capprev=0;
 free(g_scene_buckets);g_scene_buckets=NULL;g_scene_bucket_count=0;
 free(g_scene_current_keys);g_scene_current_keys=NULL;g_scene_current_cap=0;
 free(g_scene_prev_vertices.p);memset(&g_scene_prev_vertices,0,sizeof g_scene_prev_vertices);free(g_scene_between.p);memset(&g_scene_between,0,sizeof g_scene_between);
 free(g_scene_targets);g_scene_targets=NULL;g_scene_target_count=0;free(g_scene_needed);g_scene_needed=NULL;g_scene_needed_cap=0;g_scene_vao=g_scene_vbo=0;
 g_scene_last=g_scene_native_count=g_scene_mid_count=g_scene_matched=g_scene_changed=0;
 g_scene_profile=-1;g_scene_profile_frames=0;memset(g_scene_profile_sum,0,sizeof g_scene_profile_sum);
 g_scene_swap_lead=0;
}
