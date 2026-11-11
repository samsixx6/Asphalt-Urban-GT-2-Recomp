/* Provenance stays beside original guest data. Deferred commands retain an
 * epoch snapshot; later batches can invalidate but cannot inherit its role. */
#include "display_semantics.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include <string.h>
#include <limits.h>
#if ASPHALT_HUD_DIAGNOSTICS
#include <stdio.h>
#endif
enum { TAGS=65536,BATCHES=4096,SCOPES=64 };
typedef struct {uint32_t at;uint64_t life,epoch;} batch_tag;
typedef struct {uint32_t at,word,batch;uint64_t life,epoch;int role;} command_tag;
typedef struct {uint32_t at,hash,batch;uint64_t epoch;unsigned bytes;int role;} vertex_tag;
static batch_tag batches[BATCHES];
static command_tag commands[TAGS];
static vertex_tag vertices[TAGS];
static uint64_t serial,generation;
static int enabled;
#if defined(_MSC_VER)
#define SCREEN_TLS __declspec(thread)
#else
#define SCREEN_TLS _Thread_local
#endif
static SCREEN_TLS batch_tag scopes[SCOPES];
static SCREEN_TLS unsigned depth;
static SCREEN_TLS uint64_t scope_generation;
static void sync_scopes(void){
 if(scope_generation==generation)return;
 depth=0;memset(scopes,0,sizeof scopes);scope_generation=generation;
}
static uint32_t canonical(uint32_t at){return at&0x1fffffffu;}
static int valid(uint32_t at,uint32_t bytes){return at>=0x08800000u&&bytes&&bytes<=0x01800000u&&at<=0x0a000000u-bytes&&psp_mem_ptr(at,bytes)!=NULL;}
static int valid_role(int role){return role>=PSP_SCREEN_PRESERVE&&role<PSP_SCREEN_ROLE_COUNT;}
static uint64_t next_serial(void){if(serial==UINT64_MAX){enabled=0;return 0;}return ++serial;}
static batch_tag *batch(uint32_t at,int create){
 at=canonical(at);if(!valid(at,4))return NULL;
 batch_tag *b=&batches[(at>>2)&(BATCHES-1)];
 if(b->at!=at){if(!create)return NULL;*b=(batch_tag){at,next_serial(),next_serial()};}
 return b;
}
static uint32_t hash(uint32_t at,unsigned bytes){
 const unsigned char *p=psp_mem_ptr(at,bytes);uint32_t h=2166136261u;
 for(unsigned i=0;i<bytes;i++)h=(h^p[i])*16777619u;
 return h;
}
void asphalt_semantics_begin_session(int on){
 /* A post-present reset can run while another guest thread is suspended
  * inside a producer. Never reuse its batch identity or retained TLS scope. */
 if(generation!=UINT64_MAX)generation++;
 enabled=on!=0&&generation!=UINT64_MAX&&serial!=UINT64_MAX;
 sync_scopes();
 memset(batches,0,sizeof batches);memset(commands,0,sizeof commands);memset(vertices,0,sizeof vertices);
}
uint64_t asphalt_semantics_generation(void){return generation;}
void asphalt_semantics_record_vertices(uint32_t owner,uint32_t pos,uint32_t stride,unsigned count,unsigned bytes,int role){
 if(!enabled)return;
 pos=canonical(pos);
 if(!valid_role(role)||!bytes||bytes>16||stride<bytes||!count||count>8192||
    (uint64_t)(count-1)*stride+bytes>0x01800000u||!valid(pos,(count-1)*stride+bytes))return;
 batch_tag *b=batch(owner,1);if(!b||!enabled)return;
 for(unsigned i=0;i<count;i++){
  uint32_t at=pos+i*stride;vertex_tag *v=&vertices[(at>>1)&(TAGS-1)];
  *v=(vertex_tag){at,hash(at,bytes),b->at,b->epoch,bytes,role};
 }
}
void asphalt_semantics_batch_enter(uint32_t at){
 if(!enabled)return;
 sync_scopes();
 if(depth<SCOPES){batch_tag *b=batch(at,1);scopes[depth]=b?*b:(batch_tag){0};}
 if(depth<UINT_MAX)depth++;else enabled=0;
}
void asphalt_semantics_batch_leave(uint32_t at){
 if(!enabled)return;
 sync_scopes();if(!depth)return;
 if(--depth>=SCOPES)return;
 batch_tag old=scopes[depth];scopes[depth]=(batch_tag){0};
 if(old.at!=canonical(at)){depth=0;return;}
 batch_tag *b=batch(at,0);
 if(b&&b->life==old.life&&b->epoch==old.epoch)b->epoch=next_serial();
}
void asphalt_semantics_record_command(uint32_t at,uint32_t word,int role){
 if(!enabled)return;
 sync_scopes();
 at=canonical(at);command_tag *c=&commands[(at>>2)&(TAGS-1)];*c=(command_tag){0};
 if(!valid_role(role)||!valid(at,4)||word>>24!=4||psp_read32(at)!=word||depth>SCOPES)return;
 batch_tag s=depth?scopes[depth-1]:(batch_tag){0};
 if(depth&&!s.at)return;
 /* A flush can be invoked by a different currently active component than
  * the items it drains. Only per-position ownership can classify that batch. */
 /* The dedicated frontend text-widget flush proves the complete PRIM's
  * source independently of its shared atlas positions. Retain the batch
  * lifetime fence, without granting generic callbacks a batch fallback. */
 *c=(command_tag){at,word,s.at,s.life,s.epoch,s.at&&role!=PSP_SCREEN_FRONTEND_TEXT?0:role};
}
void asphalt_semantics_released(uint32_t at){
 if(!enabled)return;
 batch_tag *b=batch(at,0);if(b)*b=(batch_tag){0};
}
int asphalt_semantics_role(uint32_t prim,uint32_t pos,unsigned bytes){
 if(!enabled)return 0;
 prim=canonical(prim);pos=canonical(pos);command_tag *c=&commands[(prim>>2)&(TAGS-1)];
 /* The GE sends this sentinel after the complete PRIM has consumed every
  * vertex. Identical raw command reuse then starts without stale ownership. */
 if(!pos&&!bytes){if(c->at==prim)*c=(command_tag){0};return 0;}
 if(c->at!=prim||!valid(prim,4)||psp_read32(prim)!=c->word)return 0;
 if(!c->batch)return c->role;
 batch_tag *b=batch(c->batch,0);if(!b||b->life!=c->life)return 0;
 if(c->role==PSP_SCREEN_FRONTEND_TEXT)return c->role;
 vertex_tag *v=&vertices[(pos>>1)&(TAGS-1)];
 if(v->at!=pos||v->batch!=c->batch||v->epoch!=c->epoch||v->bytes!=bytes||!valid(pos,bytes)||hash(pos,bytes)!=v->hash)return 0;
 return v->role;
}
#if ASPHALT_HUD_DIAGNOSTICS
void asphalt_semantics_debug_query(uint32_t prim,uint32_t pos,unsigned bytes,int role){
 prim=canonical(prim);pos=canonical(pos);
 const command_tag *c=&commands[(prim>>2)&(TAGS-1)];
 const vertex_tag *v=&vertices[(pos>>1)&(TAGS-1)];
 const batch_tag *b=c->batch?batch(c->batch,0):NULL;
 const char *reason="valid";
 if(!enabled)reason="disabled";
 else if(c->at!=prim||!valid(prim,4)||psp_read32(prim)!=c->word)reason="command_identity";
 else if(c->batch&&(!b||b->life!=c->life))reason="batch_lifetime";
 else if(c->batch&&c->role!=PSP_SCREEN_FRONTEND_TEXT){
  if(v->at!=pos||v->batch!=c->batch)reason="vertex_identity";
  else if(v->epoch!=c->epoch)reason="vertex_epoch";
  else if(v->bytes!=bytes)reason="vertex_format";
  else if(!valid(pos,bytes)||hash(pos,bytes)!=v->hash)reason="vertex_payload";
 }
 if(!role&&!strcmp(reason,"valid"))reason="source_unclassified";
 fprintf(stderr,"hud-query: prim=%08X pos=%08X bytes=%u role=%d reason=%s generation=%llu command_at=%08X command_batch=%08X command_life=%llu command_epoch=%llu batch_life=%llu vertex_at=%08X vertex_batch=%08X vertex_epoch=%llu vertex_role=%d\n",
  prim,pos,bytes,role,reason,(unsigned long long)generation,c->at,c->batch,
  (unsigned long long)c->life,(unsigned long long)c->epoch,b?(unsigned long long)b->life:0,
  v->at,v->batch,(unsigned long long)v->epoch,v->role);
}
#endif
