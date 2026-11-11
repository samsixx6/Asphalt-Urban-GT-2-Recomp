/* CPU-owner-only provenance. Observe original allocations and matrix writes;
 * never change guest memory, registers, clocks or original call ordering. */
#include "display_pose.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include <string.h>
typedef struct {uint32_t base,size,generation;} allocation;
typedef struct {uint32_t at,hash;uint64_t instance;} pose_command;
static allocation blocks[8192];static unsigned block_count;static uint32_t generation;
static pose_command commands[32768];
#if defined(_MSC_VER)
#define POSE_TLS __declspec(thread)
#else
#define POSE_TLS _Thread_local
#endif
static POSE_TLS uint64_t models[64];static POSE_TLS unsigned model_depth;
static POSE_TLS struct {uint32_t at;uint64_t owner;} matrices[64];static POSE_TLS unsigned matrix_depth;
static uint32_t canonical(uint32_t p){return p&0x1fffffffu;}
static int range(uint32_t at,uint32_t size){return at>=0x08800000u&&size<=0x02000000u&&at<=0x0a000000u-size;}
static unsigned lower(uint32_t at){unsigned lo=0,hi=block_count;while(lo<hi){unsigned m=(lo+hi)/2;if(blocks[m].base<at)lo=m+1;else hi=m;}return lo;}
void asphalt_pose_allocated(uint32_t address,uint32_t size){
 address=canonical(address);if(!range(address,size))return;
 /* Invalidate overlapping old records even when the replacement is too big
  * or untyped to be eligible. Arrays/pools never become trusted instances. */
 for(unsigned i=0;i<block_count;)if((uint64_t)blocks[i].base+blocks[i].size>address&&(uint64_t)address+size>blocks[i].base){memmove(blocks+i,blocks+i+1,(--block_count-i)*sizeof *blocks);}else i++;
 if(size<64||size>65536||block_count==8192||generation==UINT32_MAX)return;
 unsigned at=lower(address);memmove(blocks+at+1,blocks+at,(block_count-at)*sizeof *blocks);block_count++;
 blocks[at]=(allocation){address,size,++generation};
}
void asphalt_pose_released(uint32_t address){
 address=canonical(address);unsigned i=lower(address);if(i==block_count||blocks[i].base!=address){if(!i)return;i--;}
 if(address>=blocks[i].base&&(uint64_t)address<(uint64_t)blocks[i].base+blocks[i].size)memmove(blocks+i,blocks+i+1,(--block_count-i)*sizeof *blocks);
}
uint64_t asphalt_pose_instance(uint32_t matrix){
 matrix=canonical(matrix);unsigned i=lower(matrix);if(i==block_count||blocks[i].base!=matrix){if(!i)return 0;i--;}
 allocation *b=&blocks[i];if(matrix<b->base||(uint64_t)matrix+64>(uint64_t)b->base+b->size)return 0;
 uint32_t vt=psp_read32(b->base);
 if(!(b->base==matrix&&b->size==64)&&(vt<0x08b30000u||vt>=0x08b4e270u||(vt&3)))return 0; /* standalone matrix or typed object; never array/scratch */
 return (uint64_t)b->generation<<32|matrix;
}
static uint32_t command_hash(uint32_t at){
 if(!range(at,52)||!psp_mem_ptr(at,52)||psp_read32(at)!=0x3a000000u)return 0;
 uint32_t h=2166136261u;
 for(unsigned i=0;i<13;i++){uint32_t word=psp_read32(at+4*i);if(i&&word>>24!=0x3b)return 0;h=(h^word)*16777619u;}return h;
}
void asphalt_pose_record(uint32_t command,uint64_t instance){
 command=canonical(command);unsigned slot=(command>>2)&32767;commands[slot]=(pose_command){0};
 if(!instance)return;uint32_t hash=command_hash(command);if(hash)commands[slot]=(pose_command){command,hash,instance};
}
uint64_t asphalt_pose_command_owner(uint32_t command){
 command=canonical(command);pose_command *c=&commands[(command>>2)&32767];if(c->at!=command)return 0;
 uint64_t owner=c->hash==command_hash(command)?c->instance:0;c->at=0;return owner;
}
void asphalt_pose_begin_session(void){
 memset(blocks,0,sizeof blocks);block_count=0;generation=0;memset(commands,0,sizeof commands);model_depth=matrix_depth=0;
 psp_ge_set_pose_owner_provider(asphalt_pose_command_owner);
}
static uint64_t model_owner(uint32_t matrix){
 if(!matrix)return UINT64_MAX; /* verified original explicit identity path */
 uint64_t owner=asphalt_pose_instance(matrix);if(owner)return owner;
 matrix=canonical(matrix);if(!range(matrix,64)||!psp_mem_ptr(matrix,64))return 0;
 /* Identity world geometry has fixed world-space ownership even when the
  * renderer subtracts a moving camera origin before uploading its matrix. */
 for(unsigned i=0;i<16;i++){uint32_t bits=psp_read32(matrix+4*i);if(i%5==0){if(bits!=0x3f800000)return 0;}else if(bits&0x7fffffff)return 0;}
 return UINT64_MAX;
}
void asphalt_pose_hook_enter(uint32_t pc,uint32_t object){
 if(pc==0x08804088u||pc==0x088040c8u)asphalt_pose_released(object);
 if(pc==0x08a3bed4u){if(model_depth<64)models[model_depth]=model_owner(psp_cpu.r[5]);model_depth++;}
 if(pc==0x0880834cu&&object==2){
  if(matrix_depth<64){uint32_t context=psp_read32(0x08b3dd50u),at=0;if(context&&psp_mem_ptr(context,12))at=psp_read32(context+8);
   matrices[matrix_depth].at=at;matrices[matrix_depth].owner=model_depth&&model_depth<=64?models[model_depth-1]:0;}
  matrix_depth++;
 }
}
void asphalt_pose_hook_leave(uint32_t pc,uint32_t object){
 if(pc==0x08804000u||pc==0x08804044u)asphalt_pose_allocated(psp_cpu.r[2],object);
 if(pc==0x0880834cu&&object==2&&matrix_depth){if(--matrix_depth<64)asphalt_pose_record(matrices[matrix_depth].at,matrices[matrix_depth].owner);}
 if(pc==0x08a3bed4u&&model_depth)model_depth--;
}
