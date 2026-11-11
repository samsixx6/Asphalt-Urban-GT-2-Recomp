#ifndef ASPHALT_STARTUP_LOGOS_H
#define ASPHALT_STARTUP_LOGOS_H
#include <stdint.h>
/* Guest CPU owner only. Tick at original08AD1CB8 entry, outside previews. */
void asphalt_startup_logos_reset(void);
void asphalt_startup_logos_tick(uint32_t object,uint32_t buttons);
void asphalt_startup_logos_released(uint32_t address);
#endif
