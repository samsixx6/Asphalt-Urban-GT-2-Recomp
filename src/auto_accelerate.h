#ifndef ASPHALT_AUTO_ACCELERATE_H
#define ASPHALT_AUTO_ACCELERATE_H
#include <stdint.h>
/* Guest CPU owner only. Unconfigured/invalid settings preserve manual input. */
void asphalt_auto_accelerate_configure(int enabled);
/* Runs AFTER the original action query, preserving its input bookkeeping. */
void asphalt_auto_accelerate_query_complete(uint32_t input_map);
#endif
