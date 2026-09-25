#ifndef YM_HAL_GPU_H
#define YM_HAL_GPU_H

#include <psptypes.h>

int hal_gpu_init(void);
void hal_gpu_shutdown(void);
void hal_gpu_begin_frame(void);
void hal_gpu_end_frame(void);
void hal_gpu_clear(u32 color_abgr);
void hal_gpu_set_texturing(int enabled);
void hal_gpu_set_tex_mode(int format);
void hal_gpu_set_tex_func(int tfx, int tcc);
void hal_gpu_set_tex_filter(int min_filter, int mag_filter);
void hal_gpu_set_blend(int enabled, int op, int src, int dst);
void hal_gpu_flush_cache_range(const void *ptr, unsigned int size);
void hal_gpu_set_scissor(int x, int y, int w, int h);
void hal_gpu_set_viewport(int width, int height);
void hal_gpu_invalidate_tex_state(void);
void hal_gpu_invalidate_scissor(void);
int hal_gpu_in_frame(void);
void *hal_gpu_get_draw_buffer_cpu(void);
void hal_gpu_copy_image(int psm, int sx, int sy, int width, int height, int srcw, const void *src,
                        int dx, int dy, int destw, void *dest);

#endif
