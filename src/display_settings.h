#ifndef ASPHALT_DISPLAY_SETTINGS_H
#define ASPHALT_DISPLAY_SETTINGS_H
typedef enum { ASPHALT_ASPECT_PSP=0,ASPHALT_ASPECT_DEVICE=1 } asphalt_aspect_mode;
typedef enum { ASPHALT_UI_ORIGINAL=0,ASPHALT_UI_NEW=1 } asphalt_ui_layout;
typedef struct { int scale;asphalt_aspect_mode aspect;int fps;int auto_accelerate;int show_fps;asphalt_ui_layout ui_layout; } asphalt_display_config;
typedef int (*asphalt_display_persist)(void *user);
void asphalt_display_settings_set_persistence(asphalt_display_persist before_write,asphalt_display_persist after_write,void *user);
int asphalt_display_validate(const asphalt_display_config *config);
int asphalt_display_load(const char *root,asphalt_display_config *config);
int asphalt_display_load_recovery(const char *root,asphalt_display_config *config,int *recovered);
int asphalt_display_save_pending(const char *root,const asphalt_display_config *config);
int asphalt_display_begin_trial(const char *root);
/* Atomically protect a live candidate with the currently usable config. */
int asphalt_display_begin_live_trial(const char *root,const asphalt_display_config *current,const asphalt_display_config *candidate);
int asphalt_display_confirm_trial(const char *root);
#endif
