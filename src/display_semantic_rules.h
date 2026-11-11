#ifndef ASPHALT_DISPLAY_SEMANTIC_RULES_H
#define ASPHALT_DISPLAY_SEMANTIC_RULES_H
#include <stdint.h>
int asphalt_semantic_component_role(uint32_t component);
int asphalt_semantic_command_role(const uint32_t *returns,unsigned count);
void asphalt_semantic_configure_bounds(void);
#endif
