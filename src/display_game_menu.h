#ifndef ASPHALT_DISPLAY_GAME_MENU_H
#define ASPHALT_DISPLAY_GAME_MENU_H
#include <stdint.h>
/* The original vertical carousel lays increasing indices above Back (zero). */
enum { ASPHALT_OPTIONS_RECOMP_INDEX=1 };
/* The allocator/releaser must use the game's own heap on its CPU owner. */
int asphalt_display_extend_options(uint32_t object,uint32_t (*allocate)(unsigned),void (*release)(uint32_t));
/* These accept only the exact, still-matching successfully extended widgets. */
uint32_t asphalt_display_options_carousel(uint32_t object);
/* Returns one when this release ends the registered extension lifetime. */
int asphalt_display_options_released(uint32_t address);
int asphalt_display_options_is_recomp(uint32_t object);
uint32_t asphalt_display_options_original_id(uint32_t object,uint32_t selected);
#endif
