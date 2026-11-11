#ifndef ASPHALT_DISPLAY_MENU_H
#define ASPHALT_DISPLAY_MENU_H
#include <stdint.h>
#include "display_settings.h"
enum { ASPHALT_MENU_RESOLUTION,ASPHALT_MENU_ASPECT,ASPHALT_MENU_RATE,
       ASPHALT_MENU_AUTOMATIC,ASPHALT_MENU_COUNTER,ASPHALT_MENU_LAYOUT,ASPHALT_MENU_APPLY,
       ASPHALT_MENU_BACK,ASPHALT_MENU_ROWS };
typedef struct {asphalt_display_config draft;int focus,active,error,applied,applying;unsigned revision;} asphalt_display_menu_state;
/* All functions belong to the guest CPU owner. Save returns zero on success. */
void asphalt_display_menu_configure(const asphalt_display_config *active,int (*save)(const asphalt_display_config *));
void asphalt_display_menu_open(void);
void asphalt_display_menu_apply_finished(const asphalt_display_config *active,int success);
void asphalt_display_menu_input(uint32_t buttons);
void asphalt_display_menu_input_at(uint32_t buttons,uint64_t milliseconds);
int asphalt_display_menu_active(void);
int asphalt_display_menu_consuming(void);
asphalt_display_menu_state asphalt_display_menu_snapshot(void);
#endif
