/*
 * CBMF runtime — реализация базового API (cbmf.h).
 * On-disk типы — cbmf_file.h (не публичный контракт).
 */

#include "fonts/cbmf.h"
#include "fonts/cbmf_file.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static void glyph_load_at(
    const uint8_t *table_base,
    uint32_t index,
    CbmfFileGlyph *out
) {
    memcpy(
        out,
        table_base + (size_t)index * CBMF_FILE_GLYPH_BYTES,
        sizeof(CbmfFileGlyph)
    );
}

static int cbmf_validate_sorted_and_bitmaps(
    size_t blob_size,
    const uint8_t *glyph_table,
    uint32_t glyph_count
) {
    uint32_t prev_cp = 0;
    int first = 1;

    for (uint32_t i = 0; i < glyph_count; i++) {
        CbmfFileGlyph g;
        glyph_load_at(glyph_table, i, &g);

        if (first) {
            first = 0;
        } else {
            if (g.codepoint <= prev_cp) {
                return CBMF_ERR_BAD_GLYPHS;
            }
        }
        prev_cp = g.codepoint;

        uint16_t expected_rb = (uint16_t)((g.width + 1u) / 2u);
        if (g.row_bytes != expected_rb) {
            return CBMF_ERR_BAD_GLYPHS;
        }

        if (g.height == 0) {
            return CBMF_ERR_BAD_GLYPHS;
        }

        uint64_t pix = (uint64_t)g.row_bytes * (uint64_t)g.height;
        uint64_t end = (uint64_t)g.bitmap_offset + pix;
        if (end > (uint64_t)blob_size) {
            return CBMF_ERR_BAD_BOUNDS;
        }
    }

    return CBMF_OK;
}

int cbmf_mount(CbmfFont *font, const void *data, size_t size) {
    if (!font || !data) {
        return CBMF_ERR_NULL;
    }

    memset(font, 0, sizeof(*font));

    if (size < CBMF_FILE_HEADER_BYTES) {
        return CBMF_ERR_TOO_SMALL;
    }

    CbmfFileHeader hdr;
    memcpy(&hdr, data, sizeof(hdr));

    if (hdr.magic != CBMF_MAGIC) {
        return CBMF_ERR_BAD_MAGIC;
    }

    if (hdr.version != 1u && hdr.version != 2u) {
        return CBMF_ERR_BAD_VERSION;
    }

    if (hdr.header_size != (uint16_t)CBMF_FILE_HEADER_BYTES) {
        return CBMF_ERR_BAD_HEADER;
    }

    if (hdr.file_size > size) {
        return CBMF_ERR_BAD_BOUNDS;
    }

    if (hdr.glyph_table_size
        != (uint64_t)hdr.glyph_count * CBMF_FILE_GLYPH_BYTES) {
        return CBMF_ERR_BAD_GLYPHS;
    }

    uint64_t g_end = (uint64_t)hdr.glyph_table_offset
        + (uint64_t)hdr.glyph_table_size;
    if (g_end > (uint64_t)hdr.file_size) {
        return CBMF_ERR_BAD_BOUNDS;
    }

    uint64_t b_end = (uint64_t)hdr.bitmap_blob_offset
        + (uint64_t)hdr.bitmap_blob_size;
    if (b_end > (uint64_t)hdr.file_size) {
        return CBMF_ERR_BAD_BOUNDS;
    }

    const uint8_t *base = (const uint8_t *)data;
    const uint8_t *glyph_table = base + hdr.glyph_table_offset;
    const uint8_t *bitmap_blob = base + hdr.bitmap_blob_offset;

    int vr = cbmf_validate_sorted_and_bitmaps(
        hdr.bitmap_blob_size,
        glyph_table,
        hdr.glyph_count
    );
    if (vr != CBMF_OK) {
        return vr;
    }

    font->base = base;
    font->size = (size_t)hdr.file_size;
    font->header = data;
    font->glyphs = glyph_table;
    font->bitmap_blob = bitmap_blob;
    font->glyph_count = hdr.glyph_count;
    font->fallback_codepoint = hdr.fallback_codepoint;
    font->version = hdr.version;
    font->line_height = hdr.line_height;
    font->baseline = hdr.baseline;

    return CBMF_OK;
}

/* 1 — найден индекс, 0 — нет */
static int glyph_index_find(
    const CbmfFont *font,
    uint32_t codepoint,
    uint32_t *out_index
) {
    uint32_t lo = 0;
    uint32_t hi = font->glyph_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        CbmfFileGlyph g;
        glyph_load_at((const uint8_t *)font->glyphs, mid, &g);

        if (g.codepoint == codepoint) {
            *out_index = mid;
            return 1;
        }
        if (g.codepoint < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return 0;
}

static int cbmf_fill_view(
    const CbmfFont *font,
    uint32_t index,
    CbmfGlyphView *out
) {
    CbmfFileGlyph g;
    glyph_load_at((const uint8_t *)font->glyphs, index, &g);

    const CbmfFileHeader *hdr = (const CbmfFileHeader *)font->header;
    uint64_t pix = (uint64_t)g.row_bytes * (uint64_t)g.height;
    if ((uint64_t)g.bitmap_offset + pix > (uint64_t)hdr->bitmap_blob_size) {
        return CBMF_ERR_BAD_BOUNDS;
    }

    out->codepoint = g.codepoint;
    out->bitmap = font->bitmap_blob + g.bitmap_offset;
    out->width = g.width;
    out->height = g.height;
    out->row_bytes = g.row_bytes;
    out->advance_x = g.advance_x;
    out->offset_x = g.offset_x;
    out->offset_y = g.offset_y;
    return CBMF_OK;
}

int cbmf_get_glyph_at(const CbmfFont *font, uint32_t index, CbmfGlyphView *out)
{
    if (!font || !out || !font->glyphs || !font->bitmap_blob || !font->header)
        return CBMF_ERR_NULL;
    if (index >= font->glyph_count) return CBMF_ERR_NOT_FOUND;
    return cbmf_fill_view(font, index, out);
}

int cbmf_get_glyph(
    const CbmfFont *font,
    uint32_t codepoint,
    CbmfGlyphView *out
) {
    if (!font || !out || !font->glyphs || !font->bitmap_blob) {
        return CBMF_ERR_NULL;
    }

    uint32_t ix;
    if (glyph_index_find(font, codepoint, &ix)) {
        return cbmf_fill_view(font, ix, out);
    }

    if (font->fallback_codepoint != codepoint) {
        if (glyph_index_find(font, font->fallback_codepoint, &ix)) {
            return cbmf_fill_view(font, ix, out);
        }
    }

    return CBMF_ERR_NOT_FOUND;
}

/*
 * Декодирует один код поинт UTF-8 из [s, end).
 * Возвращает длину в байтах (>0) или 0 при ошибке (invalid/truncated).
 */
static size_t utf8_decode_one(
    const char *s,
    const char *end,
    uint32_t *out_cp
) {
    if (s >= end) {
        return 0;
    }

    const unsigned char *u = (const unsigned char *)s;
    size_t rem = (size_t)(end - s);

    if (u[0] < 0x80u) {
        *out_cp = u[0];
        return 1;
    }

    if ((u[0] & 0xE0u) == 0xC0u) {
        if (rem < 2) {
            return 0;
        }
        if ((u[1] & 0xC0u) != 0x80u) {
            return 0;
        }
        *out_cp = ((uint32_t)(u[0] & 0x1Fu) << 6)
            | (uint32_t)(u[1] & 0x3Fu);
        if (*out_cp < 0x80u) {
            return 0;
        }
        return 2;
    }

    if ((u[0] & 0xF0u) == 0xE0u) {
        if (rem < 3) {
            return 0;
        }
        if ((u[1] & 0xC0u) != 0x80u || (u[2] & 0xC0u) != 0x80u) {
            return 0;
        }
        *out_cp = ((uint32_t)(u[0] & 0x0Fu) << 12)
            | ((uint32_t)(u[1] & 0x3Fu) << 6)
            | (uint32_t)(u[2] & 0x3Fu);
        if (*out_cp < 0x800u
            || (*out_cp >= 0xD800u && *out_cp <= 0xDFFFu)) {
            return 0;
        }
        return 3;
    }

    if ((u[0] & 0xF8u) == 0xF0u) {
        if (rem < 4) {
            return 0;
        }
        if ((u[1] & 0xC0u) != 0x80u
            || (u[2] & 0xC0u) != 0x80u
            || (u[3] & 0xC0u) != 0x80u) {
            return 0;
        }
        *out_cp = ((uint32_t)(u[0] & 0x07u) << 18)
            | ((uint32_t)(u[1] & 0x3Fu) << 12)
            | ((uint32_t)(u[2] & 0x3Fu) << 6)
            | (uint32_t)(u[3] & 0x3Fu);
        if (*out_cp < 0x10000u || *out_cp > 0x10FFFFu) {
            return 0;
        }
        return 4;
    }

    return 0;
}

int cbmf_text_width_utf8(
    const CbmfFont *font,
    const char *text,
    int32_t *out_width
) {
    if (!font || !text || !out_width) {
        return CBMF_ERR_NULL;
    }

    int64_t sum = 0;
    const char *p = text;
    const char *end = text + strlen(text);

    while (p < end) {
        uint32_t cp;
        size_t step = utf8_decode_one(p, end, &cp);
        if (step == 0) {
            return CBMF_ERR_BAD_UTF8;
        }

        CbmfGlyphView gv;
        int gr = cbmf_get_glyph(font, cp, &gv);
        if (gr != CBMF_OK) {
            return gr;
        }

        sum += (int64_t)gv.advance_x;
        if (sum > (int64_t)INT32_MAX) {
            *out_width = INT32_MAX;
            return CBMF_OK;
        }

        p += step;
    }

    *out_width = (int32_t)sum;
    return CBMF_OK;
}
