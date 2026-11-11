#ifndef ASPHALT_STARTUP_SERVICES_H
#define ASPHALT_STARTUP_SERVICES_H
#include <stdint.h>
typedef void (*asphalt_service_stop)(const char *service, uint32_t argument);
void asphalt_startup_services_init(int iso_mounted, asphalt_service_stop stop);
#endif
