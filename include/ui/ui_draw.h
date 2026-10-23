#ifndef YM_UI_DRAW_H
#define YM_UI_DRAW_H

#include <psptypes.h>
#include "ui/ui_palette.h"

typedef struct UiDrawColoredRect {
    short x;
    short y;
    short w;
    short h;
    u32 color;
} UiDrawColoredRect;

int ui_draw_init(void);
void ui_draw_shutdown(void);
void ui_draw_begin_frame(void);
void ui_draw_end_frame(void);
void ui_draw_clear(u32 color_abgr);
void ui_draw_rect(float x, float y, float w, float h, u32 color_abgr);
void ui_draw_colored_rects(const UiDrawColoredRect *rects, int count);
void ui_draw_image_rgba_crop(const void *pixels, int tex_w, int tex_h,
                             int src_x, int src_y, int src_w, int src_h,
                             float x, float y);
void ui_draw_image_rgba_nearest(const void *pixels, int tex_w, int tex_h,
                                int draw_w, int draw_h, float x, float y);
void ui_draw_text(float x, float y, const char *text, u32 color_abgr);

/* Глобальная прозрачность интерфейса (0..255): умножает альфу прямоугольников,
 * текста и значков. 255 — как обычно. Для режима бездействия. */
void ui_draw_set_alpha(int alpha);
int ui_draw_get_alpha(void);
u32 ui_draw_apply_alpha(u32 color_abgr);

/* Текстурный прямоугольник из RGBA-буфера (обложка): src в текселях,
 * dst в пикселях экрана, color_abgr модулирует (альфа = прозрачность). */
void ui_draw_cover_quad(const void *pixels, int stride_px, int tex_h,
                        float u0, float v0, float u1, float v1,
                        float x0, float y0, float x1, float y1, u32 color_abgr);

#endif
