#include "ui/ui_draw.h"

#include <pspgu.h>

#include <stddef.h>

#include "hal/hal_gpu.h"
#include "core/logger.h"
#include "fonts/text.h"

typedef struct Vertex2D {
    short x;
    short y;
    short z;
} Vertex2D;

typedef struct ColoredVertex2D {
    u32 color;
    short x;
    short y;
    short z;
} ColoredVertex2D;

int ui_draw_init(void)
{
    // No initialization needed currently
    return 0;
}

void ui_draw_shutdown(void)
{
}

void ui_draw_begin_frame(void)
{
    hal_gpu_begin_frame();
}

void ui_draw_end_frame(void)
{
    hal_gpu_end_frame();
}

void ui_draw_clear(u32 color_abgr)
{
    hal_gpu_clear(color_abgr);
}

void ui_draw_rect(float x, float y, float w, float h, u32 color_abgr)
{
    if (!hal_gpu_in_frame()) {
        return;
    }
    hal_gpu_set_texturing(0);
    Vertex2D *v = (Vertex2D *)sceGuGetMemory(2 * sizeof(Vertex2D));
    v[0].x = (short)x;
    v[0].y = (short)y;
    v[0].z = 0;
    v[1].x = (short)(x + w);
    v[1].y = (short)(y + h);
    v[1].z = 0;

    sceGuColor(color_abgr);
    sceGuDrawArray(GU_SPRITES, GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

void ui_draw_colored_rects(const UiDrawColoredRect *rects, int count)
{
    ColoredVertex2D *vertices;
    int i;

    if (!hal_gpu_in_frame() || !rects || count <= 0) {
        return;
    }
    hal_gpu_set_texturing(0);
    vertices = (ColoredVertex2D *)sceGuGetMemory(
        (unsigned int)(count * 2) * sizeof(*vertices));
    for (i = 0; i < count; i++) {
        const UiDrawColoredRect *r = &rects[i];
        ColoredVertex2D *v = &vertices[i * 2];
        v[0].color = r->color;
        v[0].x = r->x;
        v[0].y = r->y;
        v[0].z = 0;
        v[1].color = r->color;
        v[1].x = (short)(r->x + r->w);
        v[1].y = (short)(r->y + r->h);
        v[1].z = 0;
    }
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   count * 2, 0, vertices);
}

typedef struct TexVertex2D {
    short u, v;
    short x, y, z;
} TexVertex2D;

/* Copy a source rectangle from an ME-decoded texture pixel-for-pixel. */
void ui_draw_image_rgba_crop(const void *pixels, int tex_w, int tex_h,
                             int src_x, int src_y, int src_w, int src_h,
                             float x, float y)
{
    if (!hal_gpu_in_frame() || !pixels) {
        return;
    }
    /* No cache flush here: `pixels` is ME-decoded (DMA'd straight to RAM). Writing
     * back the CPU's stale cache would corrupt the frame; the player invalidates
     * instead. The GE reads the texture from RAM. */
    hal_gpu_set_texturing(1);
    /* Invalidate the GE texture cache: the previous draw was a T4/CLUT font
     * glyph, and without a flush the GE can keep that texture descriptor and
     * misread this 8888 buffer. */
    sceGuTexFlush();
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, tex_w, tex_h, tex_w, pixels);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);   /* opaque video: ignore alpha */
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);

    TexVertex2D *v = (TexVertex2D *)sceGuGetMemory(2 * sizeof(TexVertex2D));
    v[0].u = (short)src_x;      v[0].v = (short)src_y;
    v[0].x = (short)x;          v[0].y = (short)y;          v[0].z = 0;
    v[1].u = (short)(src_x + src_w); v[1].v = (short)(src_y + src_h);
    v[1].x = (short)(x + src_w); v[1].y = (short)(y + src_h); v[1].z = 0;

    sceGuDrawArray(GU_SPRITES,
        GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
        2, 0, v);
}

void ui_draw_image_rgba_nearest(const void *pixels, int tex_w, int tex_h,
                                int draw_w, int draw_h, float x, float y)
{
    TexVertex2D *v;

    if (!hal_gpu_in_frame() || !pixels || tex_w <= 0 || tex_h <= 0 ||
        draw_w <= 0 || draw_h <= 0 || draw_w > tex_w || draw_h > tex_h) {
        return;
    }
    hal_gpu_flush_cache_range(pixels,
                              (unsigned int)(tex_w * tex_h * (int)sizeof(u32)));
    hal_gpu_set_texturing(1);
    sceGuTexFlush();
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, tex_w, tex_h, tex_w, pixels);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);

    v = (TexVertex2D *)sceGuGetMemory(2 * sizeof(*v));
    v[0].u = 0;
    v[0].v = 0;
    v[0].x = (short)x;
    v[0].y = (short)y;
    v[0].z = 0;
    v[1].u = (short)draw_w;
    v[1].v = (short)draw_h;
    v[1].x = (short)(x + draw_w);
    v[1].y = (short)(y + draw_h);
    v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
}

void ui_draw_text(float x, float y, const char *text, u32 color_abgr)
{
    text_render(x, y, text, color_abgr);
}
