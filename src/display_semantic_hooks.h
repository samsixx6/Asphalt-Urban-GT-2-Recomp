#ifndef ASPHALT_DISPLAY_SEMANTIC_HOOKS_H
#define ASPHALT_DISPLAY_SEMANTIC_HOOKS_H
#include <stdint.h>
void asphalt_semantic_hooks_begin_session(int enabled);
void asphalt_semantic_hook_enter(uint32_t pc,uint32_t object);
void asphalt_semantic_hook_leave(uint32_t pc,uint32_t object);
void asphalt_semantic_component_begin(uint32_t component);
void asphalt_semantic_component_end(void);
void asphalt_semantic_prim(uint32_t address,uint32_t word);
#if ASPHALT_HUD_DIAGNOSTICS
void asphalt_semantic_diagnostics_layout(int layout,int racing);
#endif
#endif
