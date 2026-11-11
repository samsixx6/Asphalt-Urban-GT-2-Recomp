#ifndef ASPHALT_WINDOWS_BOOTSTRAP_H
#define ASPHALT_WINDOWS_BOOTSTRAP_H
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
/* Returned image is malloc-owned. Inputs and the original disc stay read-only. */
int asphalt_windows_rebase_prx(const unsigned char *input,size_t size,uint32_t base,
    unsigned char **image,size_t *image_size,char *error,size_t error_size);
int asphalt_windows_load_iso(const wchar_t *path,unsigned char **image,
    size_t *image_size,char *error,size_t error_size);
#endif
