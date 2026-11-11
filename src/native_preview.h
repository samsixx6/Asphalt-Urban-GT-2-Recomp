#ifndef ASPHALT_NATIVE_PREVIEW_H
#define ASPHALT_NATIVE_PREVIEW_H
#include <stdint.h>
typedef int (*asphalt_preview_capture)(void *user);
/* CPU-owner-only original-code calculation. All guest writes and CPU/frame
 * state are rolled back even on refusal. Callbacks may export copied render
 * data and run pure original helpers. Every scratch guest write must use the
 * guarded memory APIs so it is journaled; raw pointer writes are forbidden.
 * No HLE, guest allocation/free, checkpoints, nested preview or other host
 * side effects are permitted. */
int asphalt_preview_call(uint32_t pc,uint32_t object,uint32_t mode,const float delta[3],asphalt_preview_capture capture,void *user);
/* Optional render-data preparation runs under the same guards, before the
 * original target. Return zero to refuse. Its guest writes are journaled;
 * CPU helper scratch and target arguments are restored before the target.
 * Preparation must not perform host side effects or enter another preview. */
int asphalt_preview_call_prepared(uint32_t pc,uint32_t object,uint32_t mode,const float delta[3],asphalt_preview_capture prepare,asphalt_preview_capture capture,void *user);
int asphalt_preview_active(void);
void asphalt_preview_forbid(void);
unsigned asphalt_preview_last_pages(void);
#endif
