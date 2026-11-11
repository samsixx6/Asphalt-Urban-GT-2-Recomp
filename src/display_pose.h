#ifndef ASPHALT_DISPLAY_POSE_H
#define ASPHALT_DISPLAY_POSE_H
#include <stdint.h>
void asphalt_pose_begin_session(void);
void asphalt_pose_allocated(uint32_t address,uint32_t size);
void asphalt_pose_released(uint32_t address);
uint64_t asphalt_pose_instance(uint32_t matrix);
void asphalt_pose_record(uint32_t command,uint64_t instance);
uint64_t asphalt_pose_command_owner(uint32_t command);
void asphalt_pose_hook_enter(uint32_t pc,uint32_t object);
void asphalt_pose_hook_leave(uint32_t pc,uint32_t object);
#endif
