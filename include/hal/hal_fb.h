#ifndef YM_HAL_FB_H
#define YM_HAL_FB_H

#include <psptypes.h>

void *hal_fb_get_draw_buffer(void);
int hal_fb_get_width(void);
int hal_fb_get_height(void);
int hal_fb_get_stride(void);

#endif
