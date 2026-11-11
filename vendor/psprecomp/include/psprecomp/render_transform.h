#ifndef PSPRECOMP_RENDER_TRANSFORM_H
#define PSPRECOMP_RENDER_TRANSFORM_H
#include <stdint.h>
/* Immutable GE pose provenance for visual-only interpolation. No guest
 * pointers: the render worker consumes values copied at PRIM decoding. */
typedef struct {
 int valid;
 uint32_t mesh,indices,vertex_format,count,primitive;
 float world[12],view[12],proj[16];
 /* xscale, xpos-offx, yscale, ypos-offy, zscale, zpos */
 float viewport[6];
 uint64_t instance; /* validated original object matrix + allocation lifetime */
} psp_render_transform;
#endif
