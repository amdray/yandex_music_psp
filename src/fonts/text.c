#include "fonts/text.h"
#include "fonts/cbmf_fonts.h"
#include "fonts/cbmf.h"
#include "hal/hal_gpu.h"
#include "core/logger.h"

#include <stdint.h>
#include <string.h>
#include <pspgu.h>


int text_init(void)
{
    int rc = cbmf_fonts_init();
    if (rc != 0) {
        logLine("text: cbmf_fonts_init failed rc=%d\n", rc);
        return -1;
    }
    logLine("text: UI font immutable atlas ready\n");
    return 0;
}

void text_shutdown(void)
{
    cbmf_fonts_shutdown();
}

/* Wrap a CBMF draw in the GU state the atlas backend expects: textured, and
   alpha-test discarding the transparent CLUT index (index 0). */
static void text_draw(int x, int y, const char *text, u32 color_abgr)
{
    CbmfPspRenderer *r = cbmf_fonts_get_renderer();
    if (!r) {
        return;
    }

    hal_gpu_set_texturing(1);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);

    cbmf_psp_draw_utf8(r, x, y, text, color_abgr);

    sceGuDisable(GU_ALPHA_TEST);
    hal_gpu_set_texturing(0);
}

void text_render(float x, float y, const char *text, u32 color_abgr)
{
    if (!text || !hal_gpu_in_frame()) {
        return;
    }
    text_draw((int)x, (int)y, text, color_abgr);
}

static int utf8_step(const unsigned char *s, uint32_t *cp)
{
    unsigned char c = s[0];
    if (c < 0x80u) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        *cp = ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
        return 2;
    }
    if ((c & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *cp = ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6)
            | (uint32_t)(s[2] & 0x3Fu);
        return 3;
    }
    *cp = 0x20;
    return 1;
}

void text_render_clipped(float x, float y, const char *text,
                         u32 color_abgr, float max_width)
{
    if (!text || !hal_gpu_in_frame()) {
        return;
    }
    if (max_width <= 0.f) {
        text_draw((int)x, (int)y, text, color_abgr);
        return;
    }

    const CbmfFont *f = cbmf_fonts_get_font();
    if (!f) {
        return;
    }

    char buf[512];
    size_t out = 0;
    float used = 0.f;
    const unsigned char *p = (const unsigned char *)text;

    while (*p && out + 4u < sizeof(buf)) {
        uint32_t cp;
        int n = utf8_step(p, &cp);
        CbmfGlyphView gv;
        float adv = (cbmf_get_glyph(f, cp, &gv) == CBMF_OK) ? (float)gv.advance_x : 0.f;
        if (used + adv > max_width) {
            break;
        }
        memcpy(buf + out, p, (size_t)n);
        out += (size_t)n;
        used += adv;
        p += n;
    }
    buf[out] = '\0';
    text_draw((int)x, (int)y, buf, color_abgr);
}

void text_render_window(float x, float y, const char *text,
                        u32 color_abgr, float skip_px, float max_width)
{
    const CbmfFont *f;
    const unsigned char *p;
    char buf[512];
    size_t out = 0;
    float used = 0.f;
    float skipped = 0.f;
    float residual;
    float draw_width;

    if (!text || !hal_gpu_in_frame()) {
        return;
    }
    if (max_width <= 0.f) {
        return;
    }
    if (skip_px <= 0.f) {
        text_render_clipped(x, y, text, color_abgr, max_width);
        return;
    }

    f = cbmf_fonts_get_font();
    if (!f) {
        return;
    }

    p = (const unsigned char *)text;

    while (*p) {
        uint32_t cp;
        int n = utf8_step(p, &cp);
        CbmfGlyphView gv;
        float adv = (cbmf_get_glyph(f, cp, &gv) == CBMF_OK) ? (float)gv.advance_x : 0.f;
        if (skipped + adv > skip_px) {
            break;
        }
        skipped += adv;
        p += n;
    }

    residual = skip_px - skipped;
    if (residual < 0.f) {
        residual = 0.f;
    }
    draw_width = max_width + residual;
    while (*p && out + 4u < sizeof(buf)) {
        uint32_t cp;
        int n = utf8_step(p, &cp);
        CbmfGlyphView gv;
        float adv = (cbmf_get_glyph(f, cp, &gv) == CBMF_OK) ? (float)gv.advance_x : 0.f;
        if (used >= draw_width) {
            break;
        }
        memcpy(buf + out, p, (size_t)n);
        out += (size_t)n;
        used += adv;
        p += n;
    }
    buf[out] = '\0';
    text_draw((int)(x - residual), (int)y, buf, color_abgr);
}

float text_measure_width(const char *text)
{
    if (!text) {
        return 0.f;
    }
    const CbmfFont *f = cbmf_fonts_get_font();
    if (!f) {
        return 0.f;
    }
    int32_t w = 0;
    cbmf_text_width_utf8(f, text, &w);
    return (float)w;
}
