#ifndef ASPHALT_SAVEDATA_H
#define ASPHALT_SAVEDATA_H
#include "startup_services.h"
typedef int (*asphalt_savedata_persist)(void *user);
void asphalt_savedata_init(const char *runtime_root, asphalt_service_stop stop);
/* Optional host persistence boundary. Set these after init; 0 means success. */
void asphalt_savedata_set_persistence(asphalt_savedata_persist before_write,
                                     asphalt_savedata_persist after_write, void *user);
/* Repair only validated own-format interrupted writes; no guest state/callbacks. */
int asphalt_savedata_recover(const char *runtime_root);
/* Read-only validation of committed own saves (or an empty runtime). */
int asphalt_savedata_validate(const char *runtime_root);
#endif
