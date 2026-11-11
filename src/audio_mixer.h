#ifndef ASPHALT_AUDIO_MIXER_H
#define ASPHALT_AUDIO_MIXER_H
#include <stdint.h>
#define ASPHALT_AUDIO_CHANNELS 8
#define ASPHALT_AUDIO_AHEAD 4096
/* Caller serializes producers/consumer. No guest pointers survive push(). */
typedef struct {
    int16_t pcm[ASPHALT_AUDIO_AHEAD * 2];
    unsigned read, count;
} asphalt_audio_channel;
typedef struct {
    asphalt_audio_channel channels[ASPHALT_AUDIO_CHANNELS];
    uint64_t input_frames, dropped_frames, mixed_frames, nonzero_frames;
    unsigned channel_mask, peak;
} asphalt_audio_mixer;
void asphalt_audio_push(asphalt_audio_mixer *, int channel, const int16_t *,
                        unsigned frames, int stereo, int volume_l, int volume_r);
void asphalt_audio_mix(asphalt_audio_mixer *, int16_t *out, unsigned frames);
#endif
