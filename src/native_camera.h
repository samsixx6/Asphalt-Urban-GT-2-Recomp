#ifndef ASPHALT_NATIVE_CAMERA_H
#define ASPHALT_NATIVE_CAMERA_H
#include <stdint.h>
#include "native_preview.h"
typedef struct {uint32_t address,size;unsigned char bytes[64];} asphalt_camera_range;
typedef struct {uint32_t descriptor;asphalt_camera_range ranges[5];
 /* Optional exact calculator pose used by projected RearRadar UI. It is not
  * a committed controller update and is consumed only inside a UI preview. */
 asphalt_camera_range rear_pose;} asphalt_camera_frame;
/* Calculate an original supported camera from current physical state, retain
 * only its render data, and roll back every calculation-side guest write. */
int asphalt_native_camera_calculate(uint32_t child,uint32_t environment,const float delta[3],asphalt_camera_frame *out);
/* Optional original preparation/capture share the camera's rollback scope. */
int asphalt_native_camera_calculate_prepared(uint32_t child,uint32_t environment,const float delta[3],asphalt_preview_capture prepare,asphalt_preview_capture capture,void *user,asphalt_camera_frame *out);
/* Rebuild projection for another depth pass from the same copied view. */
int asphalt_native_camera_project(const asphalt_camera_frame *source,uint32_t child,float near_plane,float far_plane,asphalt_camera_frame *out);
#endif
