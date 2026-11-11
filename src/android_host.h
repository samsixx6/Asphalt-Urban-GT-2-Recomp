#ifndef ASPHALT_ANDROID_HOST_H
#define ASPHALT_ANDROID_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* All guest/runtime calls belong to the current PSP CPU owner. JNI, the
 * render thread and the audio worker exchange only host-owned data. */
uint64_t asphalt_android_now_ms(void);
int asphalt_android_host_start(void);
void asphalt_android_host_stop(void);
int asphalt_android_host_tick(uint32_t *buttons, uint8_t *ax, uint8_t *ay,
                              char *error, size_t capacity);
void asphalt_android_host_report(FILE *out);
void asphalt_android_install_persistence(void);

/* Called by the boot pthread; any later fatal stop may belong to a guest
 * worker pthread instead. This process supports exactly one game session. */
void asphalt_android_game_run(const char *image_path, const char *iso_path,
                              const char *runtime_root);
_Noreturn void asphalt_android_finish(const char *reason);

#endif
