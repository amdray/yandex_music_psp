#ifndef CBMF_H
#define CBMF_H

#include <stdint.h>
#include <stddef.h>

#define CBMF_OK                 0
#define CBMF_ERR_NULL          -1
#define CBMF_ERR_TOO_SMALL     -2
#define CBMF_ERR_BAD_MAGIC     -3
#define CBMF_ERR_BAD_VERSION   -4
#define CBMF_ERR_BAD_HEADER    -5
#define CBMF_ERR_BAD_BOUNDS    -6
#define CBMF_ERR_BAD_GLYPHS    -7
#define CBMF_ERR_BAD_UTF8      -8
#define CBMF_ERR_NOT_FOUND     -9

typedef struct {
    const uint8_t *base;
    size_t size;

    const void *header;
    const void *glyphs;
    const uint8_t *bitmap_blob;

    uint32_t glyph_count;
    uint32_t fallback_codepoint;

    uint16_t version; /* 1: binary ink; 2: coverage 0..15 */
    uint16_t line_height;
    int16_t baseline;
} CbmfFont;

typedef struct {
    uint32_t codepoint;

    const uint8_t *bitmap;

    uint16_t width;
    uint16_t height;
    uint16_t row_bytes;
    uint16_t advance_x;

    int8_t offset_x;
    int8_t offset_y;
} CbmfGlyphView;

int cbmf_mount(
    CbmfFont *font,
    const void *data,
    size_t size
);

int cbmf_get_glyph(
    const CbmfFont *font,
    uint32_t codepoint,
    CbmfGlyphView *out
);

/* Enumerate the validated, codepoint-sorted glyph table. */
int cbmf_get_glyph_at(const CbmfFont *font, uint32_t index, CbmfGlyphView *out);

int cbmf_text_width_utf8(
    const CbmfFont *font,
    const char *text,
    int32_t *out_width
);

#endif
