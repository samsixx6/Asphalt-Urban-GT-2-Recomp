#include "display_view.h"
#include <string.h>
int asphalt_display_view_make(const asphalt_display_config *config,int width,int height,const int insets[4],asphalt_display_view *out) {
    if(asphalt_display_validate(config)||!out||width<=0||height<=0)return -1;
    int left=insets?insets[0]:0,top=insets?insets[1]:0,right=insets?insets[2]:0,bottom=insets?insets[3]:0;
    if(left<0||top<0||right<0||bottom<0||left>=width||right>=width-left||top>=height||bottom>=height-top)return -1;
    memset(out,0,sizeof *out);out->world_x=out->ui_x_scale=out->ui_y_scale=1;
    if(config->aspect==ASPHALT_ASPECT_PSP) {
        float fit=(float)width/480;if((float)height/272<fit)fit=(float)height/272;
        out->width=480*fit;out->height=272*fit;out->x=((float)width-out->width)/2;out->y=((float)height-out->height)/2;
        return 0;
    }
    out->width=(float)width;out->height=(float)height;
    out->world_x=(480.0f/272)/((float)width/(float)height);
    float aw=(float)(width-left-right),ah=(float)(height-top-bottom);
    float fit=aw/480;if(ah/272<fit)fit=ah/272;
    out->ui_x_scale=fit*480/(float)width;out->ui_y_scale=fit*272/(float)height;
    out->ui_x_offset=((float)left+(aw-480*fit)/2)*480/(float)width;
    out->ui_y_offset=((float)top+(ah-272*fit)/2)*272/(float)height;
    return 0;
}
