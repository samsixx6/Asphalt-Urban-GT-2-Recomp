#ifndef ASPHALT_HOST_INPUT_H
#define ASPHALT_HOST_INPUT_H
#include <stdint.h>
typedef struct {
    uint8_t held[256];
    uint64_t tap_until[256];
} asphalt_host_keys;
void asphalt_key_event(asphalt_host_keys *,unsigned key,int down,uint64_t now_ms);
void asphalt_key_sample(const asphalt_host_keys *,uint8_t out[256],uint64_t now_ms);
void asphalt_key_clear(asphalt_host_keys *);
/* CPU-owned Windows mapping. Rebound keys wait for release across menus. */
typedef struct {int initialized,racing;uint8_t blocked,held;} asphalt_windows_keymap;
uint32_t asphalt_windows_keyboard(asphalt_windows_keymap *,const uint8_t keys[256],int racing);
#endif
