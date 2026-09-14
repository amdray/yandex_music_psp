#ifndef YM_SERVICES_IMAGE_LOADER_H
#define YM_SERVICES_IMAGE_LOADER_H

#include <psptypes.h>

int image_load_rgba8888(const char *path, void **out_data,
                        int *out_w, int *out_h, int *out_stride_bytes);
void image_free_rgba8888(void *data);

#endif
