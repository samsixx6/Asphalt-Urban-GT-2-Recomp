#ifndef ASPHALT_DISPLAY_SESSION_H
#define ASPHALT_DISPLAY_SESSION_H
#include "display_settings.h"
int asphalt_display_session_start(const char *root);
asphalt_display_config asphalt_display_session_config(void);
int asphalt_display_session_geometry(int width,int height,const int insets[4]);
void asphalt_display_session_menu_seen(void);
void asphalt_display_session_screen(const char *name);
void asphalt_display_session_options(int active);
void asphalt_display_session_race_observed(int present); /* CPU owner, validated race graph */
void asphalt_display_session_frame(void);
void asphalt_display_session_post_present(void); /* CPU owner; previous frame submitted */
int asphalt_display_session_restart_requested(void); /* atomic, any thread */
int asphalt_display_session_recovered(void); /* atomic, any thread */
int asphalt_display_session_fps_counter(void); /* atomic, any thread */
int asphalt_display_session_controls_layout(void); /* atomic: bit0 New, bit1 PSP aspect */
#endif
