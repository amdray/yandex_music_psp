#ifndef YM_UI_DRAW_H
#define YM_UI_DRAW_H

#include <psptypes.h>

int ui_draw_init(void);
void ui_draw_shutdown(void);
void ui_draw_begin_frame(void);
void ui_draw_end_frame(void);
void ui_draw_clear(u32 color_abgr);
void ui_draw_rect(float x, float y, float w, float h, u32 color_abgr);
void ui_draw_image_rgba_crop(const void *pixels, int tex_w, int tex_h,
                             int src_x, int src_y, int src_w, int src_h,
                             float x, float y);
void ui_draw_text(float x, float y, const char *text, u32 color_abgr);

#endif
