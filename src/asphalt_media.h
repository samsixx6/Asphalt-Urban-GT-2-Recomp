#ifndef ASPHALT_MEDIA_H
#define ASPHALT_MEDIA_H
#include <stdint.h>
typedef void (*asphalt_media_stop_fn)(const char *reason);
void asphalt_mpeg_register(asphalt_media_stop_fn stop);
uint64_t asphalt_mpeg_video_frames(void);
uint64_t asphalt_mpeg_audio_samples(void);
int asphalt_media_available(void);
void asphalt_atrac_register(asphalt_media_stop_fn stop);
#endif
