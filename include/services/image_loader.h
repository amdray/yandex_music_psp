#ifndef YM_SERVICES_IMAGE_LOADER_H
#define YM_SERVICES_IMAGE_LOADER_H

#include <psptypes.h>

typedef enum {
    IMAGE_OK = 0,
    IMAGE_ERR_ARGUMENT = -1,
    IMAGE_ERR_IO = -2,
    IMAGE_ERR_NO_MEMORY = -3,
    IMAGE_ERR_UNSUPPORTED = -4,
    IMAGE_ERR_INVALID_DATA = -5,
    IMAGE_ERR_DECODE = -6
} ImageResult;

/* On success the caller owns pixels; on failure outputs are empty.
 * INVALID_DATA identifies rejected file data; DECODE is an unclassified
 * decoder failure and does not establish that the file is corrupt. */
int image_load_rgba8888(const char *path, void **out_data,
                        int *out_w, int *out_h, int *out_stride_bytes);
void image_free_rgba8888(void *data);

#endif
