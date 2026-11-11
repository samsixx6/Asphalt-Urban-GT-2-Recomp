#ifndef ASPHALT_DISPLAY_SEMANTICS_H
#define ASPHALT_DISPLAY_SEMANTICS_H
#include <stdint.h>
/* CPU owner only. Metadata never changes the original command or vertex. */
void asphalt_semantics_begin_session(int enabled);
/* CPU-owner generation: suspended guest threads discard their old TLS scopes. */
uint64_t asphalt_semantics_generation(void);
void asphalt_semantics_record_command(uint32_t at,uint32_t word,int role);
void asphalt_semantics_record_vertices(uint32_t batch,uint32_t position,uint32_t stride,unsigned count,unsigned bytes,int role);
void asphalt_semantics_batch_enter(uint32_t batch);
void asphalt_semantics_batch_leave(uint32_t batch);
void asphalt_semantics_released(uint32_t batch);
/* position=bytes=0 retires a completed PRIM after its final vertex query. */
int asphalt_semantics_role(uint32_t prim,uint32_t position,unsigned bytes);
#if ASPHALT_HUD_DIAGNOSTICS
/* Read-only explanation for a bounded diagnostic build, never a fallback. */
void asphalt_semantics_debug_query(uint32_t prim,uint32_t position,unsigned bytes,int role);
#endif
#endif
