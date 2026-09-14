#include "fonts/cbmf_psp.h"

#include <string.h>
#include <pspgu.h>
#include <pspkernel.h>

static int psp_decode_utf8_one(const char **cursor, uint32_t *out_cp) {
    const uint8_t *s;
    uint32_t cp;
    uint8_t b0;

    if (!cursor || !*cursor || !out_cp) {
        return CBMF_ERR_NULL;
    }

    s = (const uint8_t *)(*cursor);
    b0 = s[0];
    if (b0 == 0) {
        return CBMF_ERR_NOT_FOUND;
    }

    if (b0 < 0x80u) {
        *out_cp = (uint32_t)b0;
        *cursor += 1;
        return CBMF_OK;
    }

    if ((b0 & 0xE0u) == 0xC0u) {
        uint8_t b1 = s[1];
        if ((b1 & 0xC0u) != 0x80u) {
            return CBMF_ERR_BAD_UTF8;
        }
        cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(b1 & 0x3Fu);
        if (cp < 0x80u) {
            return CBMF_ERR_BAD_UTF8;
        }
        *out_cp = cp;
        *cursor += 2;
        return CBMF_OK;
    }

    if ((b0 & 0xF0u) == 0xE0u) {
        uint8_t b1 = s[1];
        if ((b1 & 0xC0u) != 0x80u) return CBMF_ERR_BAD_UTF8;
        uint8_t b2 = s[2];
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
            return CBMF_ERR_BAD_UTF8;
        }
        cp = ((uint32_t)(b0 & 0x0Fu) << 12) |
             ((uint32_t)(b1 & 0x3Fu) << 6) |
             (uint32_t)(b2 & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return CBMF_ERR_BAD_UTF8;
        }
        *out_cp = cp;
        *cursor += 3;
        return CBMF_OK;
    }

    if ((b0 & 0xF8u) == 0xF0u) {
        uint8_t b1 = s[1];
        if ((b1 & 0xC0u) != 0x80u) return CBMF_ERR_BAD_UTF8;
        uint8_t b2 = s[2];
        if ((b2 & 0xC0u) != 0x80u) return CBMF_ERR_BAD_UTF8;
        uint8_t b3 = s[3];
        if ((b1 & 0xC0u) != 0x80u ||
            (b2 & 0xC0u) != 0x80u ||
            (b3 & 0xC0u) != 0x80u) {
            return CBMF_ERR_BAD_UTF8;
        }
        cp = ((uint32_t)(b0 & 0x07u) << 18) |
             ((uint32_t)(b1 & 0x3Fu) << 12) |
             ((uint32_t)(b2 & 0x3Fu) << 6) |
             (uint32_t)(b3 & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu) {
            return CBMF_ERR_BAD_UTF8;
        }
        *out_cp = cp;
        *cursor += 4;
        return CBMF_OK;
    }

    return CBMF_ERR_BAD_UTF8;
}


static int find_glyph(const CbmfPspRenderer *renderer, uint32_t codepoint)
{
    unsigned int lo = 0, hi = renderer->glyph_count;
    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2;
        uint32_t cp = renderer->glyphs[mid].codepoint;
        if (cp == codepoint) return (int)mid;
        if (cp < codepoint) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

int cbmf_psp_init(CbmfPspRenderer *renderer, const CbmfFont *font,
                  const CbmfPspRendererDesc *desc)
{
    if (!renderer) return CBMF_PSP_ERR_NULL;
    memset(renderer, 0, sizeof(*renderer));
    if (!font || !desc || !desc->texture_pixels || !desc->glyphs ||
        !desc->clut) return CBMF_PSP_ERR_NULL;
    unsigned int w = desc->texture_width, h = desc->texture_height;
    if (!w || !h || w > 512 || h > 512 || (w & (w - 1)) || (h & (h - 1)) ||
        desc->texture_pitch < w || (desc->texture_pitch & 31) ||
        ((uintptr_t)desc->texture_pixels & 15) || ((uintptr_t)desc->clut & 15))
        return CBMF_PSP_ERR_BAD_DESC;
    if (!font->glyph_count || font->glyph_count > desc->glyph_capacity)
        return CBMF_PSP_ERR_ATLAS_TOO_SMALL;

    size_t pitch_bytes = desc->texture_pitch / 2;
    size_t texture_bytes = pitch_bytes * h;
    memset(desc->texture_pixels, 0, texture_bytes);
    unsigned int sx = 0, sy = 0, row_height = 0;
    for (uint32_t i = 0; i < font->glyph_count; ++i) {
        CbmfGlyphView glyph;
        if (cbmf_get_glyph_at(font, i, &glyph) != CBMF_OK)
            return CBMF_PSP_ERR_BAD_DESC;
        unsigned int cell_width = (glyph.width + 1u) & ~1u;
        if (cell_width > w || glyph.height > h)
            return CBMF_PSP_ERR_GLYPH_TOO_LARGE;
        if (sx + cell_width > w) {
            sy += row_height;
            sx = 0;
            row_height = 0;
        }
        if (sy + glyph.height > h) return CBMF_PSP_ERR_ATLAS_TOO_SMALL;
        for (unsigned int y = 0; y < glyph.height; ++y) {
            uint8_t *row = desc->texture_pixels + (sy + y) * pitch_bytes + sx / 2;
            memcpy(row, glyph.bitmap + y * glyph.row_bytes, glyph.row_bytes);
            if (glyph.width & 1) row[glyph.width / 2] &= 0x0f;
        }
        desc->glyphs[i] = (CbmfPspGlyph){glyph.codepoint, (uint16_t)sx, (uint16_t)sy};
        sx += cell_width;
        if (glyph.height > row_height) row_height = glyph.height;
    }
    /* Coverage stays in the immutable CLUT; command vertices supply color. */
    for (unsigned int i = 0; i < 16; ++i) {
        uint32_t alpha = font->version == 2 ? i * 17u : (i ? 255u : 0u);
        desc->clut[i] = (alpha << 24) | 0x00ffffffu;
    }
    sceKernelDcacheWritebackRange(desc->texture_pixels, texture_bytes);
    sceKernelDcacheWritebackRange(desc->clut, 16 * sizeof(uint32_t));

    renderer->font = font;
    renderer->texture_pixels = desc->texture_pixels;
    renderer->texture_width = desc->texture_width;
    renderer->texture_height = desc->texture_height;
    renderer->texture_pitch = desc->texture_pitch;
    renderer->glyph_count = (uint16_t)font->glyph_count;
    renderer->glyphs = desc->glyphs;
    renderer->clut = desc->clut;
    return CBMF_PSP_OK;
}

int cbmf_psp_draw_utf8(CbmfPspRenderer *renderer, int x, int y,
                      const char *text, uint32_t color)
{
    /* Field order follows GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT.
     * Vertex data belongs to the current GU list, not reusable CPU scratch. */
    typedef struct {
        uint16_t u, v;
        uint32_t color;
        int16_t x, y, z;
    } GlyphVert;
    if (!renderer || !renderer->font || !text) return CBMF_PSP_ERR_NULL;
    if (!*text) return CBMF_PSP_OK;

    sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
    sceGuClutLoad(2, renderer->clut);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexImage(0, renderer->texture_width, renderer->texture_height,
                  renderer->texture_pitch, renderer->texture_pixels);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);

    const char *cursor = text;
    int pen_x = x;
    while (*cursor) {
        uint32_t cp;
        CbmfGlyphView glyph;
        if (psp_decode_utf8_one(&cursor, &cp) != CBMF_OK)
            return CBMF_PSP_ERR_BAD_UTF8;
        if (cbmf_get_glyph(renderer->font, cp, &glyph) != CBMF_OK)
            return CBMF_PSP_ERR_NOT_FOUND;
        int slot = find_glyph(renderer, glyph.codepoint);
        if (slot < 0) return CBMF_PSP_ERR_NOT_FOUND;
        if (glyph.width && glyph.height) {
            unsigned int u = renderer->glyphs[slot].u;
            unsigned int v = renderer->glyphs[slot].v;
            GlyphVert *vertices = sceGuGetMemory(2 * sizeof(GlyphVert));
            vertices[0] = (GlyphVert){(uint16_t)u, (uint16_t)v, color,
                (int16_t)(pen_x + glyph.offset_x), (int16_t)(y + glyph.offset_y), 0};
            vertices[1] = (GlyphVert){(uint16_t)(u + glyph.width),
                (uint16_t)(v + glyph.height), color,
                (int16_t)(pen_x + glyph.offset_x + glyph.width),
                (int16_t)(y + glyph.offset_y + glyph.height), 0};
            sceGuDrawArray(GU_SPRITES,
                GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                2, NULL, vertices);
        }
        pen_x += glyph.advance_x;
    }
    return CBMF_PSP_OK;
}
