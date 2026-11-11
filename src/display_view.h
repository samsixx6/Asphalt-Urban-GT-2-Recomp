#ifndef ASPHALT_DISPLAY_VIEW_H
#define ASPHALT_DISPLAY_VIEW_H
#include "display_settings.h"
typedef struct {
    float x,y,width,height;
    float world_x,ui_x_scale,ui_y_scale,ui_x_offset,ui_y_offset;
} asphalt_display_view;
int asphalt_display_view_make(const asphalt_display_config *config,int width,int height,const int insets[4],asphalt_display_view *out);
#endif
