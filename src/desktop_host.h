#ifndef ASPHALT_DESKTOP_HOST_H
#define ASPHALT_DESKTOP_HOST_H
#include <stdint.h>
#include <stdio.h>
/* Win32 adapter for this Windows spike. Guest memory remains CPU-owner-only. */
int asphalt_desktop_start(void);
/* Returns 1 for a close request, -1 for device failure. Merges live input. */
int asphalt_desktop_tick(uint64_t elapsed_ms,uint32_t *buttons,uint8_t *ax,uint8_t *ay);
void asphalt_desktop_stop(void);
void asphalt_desktop_report(FILE *out);
#endif
