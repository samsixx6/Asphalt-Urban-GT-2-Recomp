#include "host_input.h"
#include <string.h>
void asphalt_key_event(asphalt_host_keys *keys,unsigned key,int down,uint64_t now) {
    if(key>=256)return;
    if(down && !keys->held[key])keys->tap_until[key]=now+80;
    keys->held[key]=(uint8_t)!!down;
}
void asphalt_key_sample(const asphalt_host_keys *keys,uint8_t out[256],uint64_t now) {
    for(unsigned key=0;key<256;key++)out[key]=(uint8_t)(keys->held[key] || now<keys->tap_until[key]);
}
void asphalt_key_clear(asphalt_host_keys *keys) {memset(keys,0,sizeof *keys);}
uint32_t asphalt_windows_keyboard(asphalt_windows_keymap *map,const uint8_t k[256],int racing){
    racing=racing!=0;
    int up_held=k[38]||k['W'],down_held=k[40]||k['S'];
    uint8_t held=(uint8_t)((up_held?1:0)|(down_held?2:0)|(k[32]?4:0));
    if(map->initialized&&map->racing!=racing)
        map->blocked=(uint8_t)(held&map->held);
    map->initialized=1;map->racing=racing;map->held=held;
    if(!up_held)map->blocked&=(uint8_t)~1u;
    if(!down_held)map->blocked&=(uint8_t)~2u;
    if(!k[32])map->blocked&=(uint8_t)~4u;
    int up=up_held&&!(map->blocked&1),down=down_held&&!(map->blocked&2),space=k[32]&&!(map->blocked&4);
    return (up?(racing?0x4000u:0x10u):0)|(down?(racing?0x8000u:0x40u):0)|
        (space?(racing?0x2000u:0x4000u):0)|((k[39]||k['D'])?0x20u:0)|((k[37]||k['A'])?0x80u:0)|
        (k['Q']?0x100u:0)|(k['E']?0x200u:0)|(k['X']?0x4000u:0)|
        (k['Z']?0x8000u:0)|(k['C']?0x2000u:0)|(k['V']?0x1000u:0)|(k[13]?8u:0)|(k[8]?1u:0);
}
