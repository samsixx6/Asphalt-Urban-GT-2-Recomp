#ifndef ASPHALT_DISPLAY_ICON_H
#define ASPHALT_DISPLAY_ICON_H
#include <stdint.h>
int asphalt_display_install_icon(uint32_t object,uint32_t (*allocate)(unsigned),void (*release)(uint32_t));
/* Notify before game-owned Options allocations are released or reused. */
void asphalt_display_icon_released(uint32_t address);
int asphalt_display_icon_enqueue(uint32_t component,uint32_t offset,uint32_t quad);
#endif
