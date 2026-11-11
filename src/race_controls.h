#ifndef ASPHALT_RACE_CONTROLS_H
#define ASPHALT_RACE_CONTROLS_H
#include <stdint.h>
/* Configure before the CPU owner starts. Every callback below is CPU-owner
 * only. The UI may call active(), which reads only an atomic host flag.
 * active() means verified unpaused race layout, including original countdown
 * and cinematic input locks; it does not mean the player can currently drive. */
void asphalt_race_controls_configure(int automatic);
void asphalt_race_controls_set_automatic(int automatic); /* live toggle; retain race/lifetime proof */
void asphalt_race_controls_root_enter(uint32_t owner);
void asphalt_race_controls_root_leave(uint32_t owner);
void asphalt_race_controls_released(uint32_t object);
void asphalt_race_controls_allocated(uint32_t address,uint32_t size);
void asphalt_race_controls_screen(const char *name);
void asphalt_race_controls_loading_draw(uint32_t context); /* original088DF0A4 worker draw, read-only */
void asphalt_race_controls_loading_end(uint32_t context); /* original088DEB4C context retirement */
void asphalt_race_controls_script(uint32_t interpreter,uint32_t program,uint32_t return_address);
int asphalt_race_controls_active(void);
int asphalt_race_controls_racing(void); /* verified unpaused race, independent of Auto accelerate */
int asphalt_race_controls_hidden(void); /* original loading or cinematic touch suppression, atomic */
#endif
