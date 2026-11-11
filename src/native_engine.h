#ifndef ASPHALT_NATIVE_ENGINE_H
#define ASPHALT_NATIVE_ENGINE_H
#include <stdint.h>
#include <stdio.h>
/* All functions run on the guest CPU owner. Configure before starting it. */
typedef struct {uint64_t frames,packets,steps,visuals,resets,fallbacks;} asphalt_native_engine_stats;
typedef void (*asphalt_native_task_observer)(uint32_t pc,uint32_t object,uint32_t delta,uint32_t mode);
void asphalt_native_engine_configure(int fps);
/* Accept 30/60 on the CPU owner after presentation. A request made inside an
 * original root/callback settles at its leave; configure is startup-only. */
int asphalt_native_engine_request_rate(int fps);
void asphalt_native_engine_root_enter(uint32_t owner);
void asphalt_native_engine_root_leave(uint32_t owner);
void asphalt_native_engine_group_enter(uint32_t group,uint32_t delta);
void asphalt_native_engine_camera_enter(uint32_t renderer,uint32_t descriptor);
/* Pair every original model load, including unsupported/nested calls. */
int asphalt_native_engine_model_enter(uint32_t matrix,uint32_t caller);
void asphalt_native_engine_model_leave(void);
/* Pair every original RearRadar draw, including refused/nested passes. */
int asphalt_native_engine_rear_enter(uint32_t radar);
void asphalt_native_engine_rear_leave(void);
void asphalt_native_engine_released(uint32_t object);
int asphalt_native_engine_task(uint32_t pc,uint32_t object,uint32_t delta,uint32_t mode,uint32_t record,uint32_t parent);
void asphalt_native_engine_observe(asphalt_native_task_observer observer);
asphalt_native_engine_stats asphalt_native_engine_counters(void);
/* CPU-owner-only, cumulative machine-readable observations. Reading the report
 * never samples guest memory or changes the engine/observation counters. */
void asphalt_native_engine_report(FILE *out);

/* Exact ordinary-mesh getter; zero leaves the original cache path intact. */
uint32_t asphalt_native_engine_part_pose(uint32_t part,uint32_t caller);
#endif
