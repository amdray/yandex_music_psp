#ifndef CBMF_PSP_H
#define CBMF_PSP_H

#include <stdint.h>
#include "cbmf.h"

#define CBMF_PSP_OK                    0
#define CBMF_PSP_ERR_NULL             -1
#define CBMF_PSP_ERR_BAD_DESC         -2
#define CBMF_PSP_ERR_ATLAS_TOO_SMALL  -3
#define CBMF_PSP_ERR_GLYPH_TOO_LARGE  -4
#define CBMF_PSP_ERR_BAD_UTF8         -5
#define CBMF_PSP_ERR_NOT_FOUND        -6

/* Caller owns all storage. Init builds the complete T4 atlas; drawing never
 * modifies it or the CLUT. Storage must live until the last GU list completes.
 * CBMF v1 uses binary ink; v2 uses 4-bit coverage.
 * Texture and CLUT must be 16-byte aligned; pitch is in pixels.
 */
typedef struct {
    uint32_t codepoint;
    uint16_t u, v;
} CbmfPspGlyph;

typedef struct {
    uint8_t *texture_pixels;
    uint16_t texture_width, texture_height, texture_pitch;
    CbmfPspGlyph *glyphs;
    uint16_t glyph_capacity;
    uint32_t *clut; /* 16 GU_PSM_8888 entries */
} CbmfPspRendererDesc;

typedef struct {
    const CbmfFont *font;
    const uint8_t *texture_pixels;
    uint16_t texture_width, texture_height, texture_pitch;
    uint16_t glyph_count;
    const CbmfPspGlyph *glyphs;
    const uint32_t *clut;
} CbmfPspRenderer;

int cbmf_psp_init(CbmfPspRenderer *renderer, const CbmfFont *font,
                  const CbmfPspRendererDesc *desc);
int cbmf_psp_draw_utf8(CbmfPspRenderer *renderer, int x, int y,
                      const char *text, uint32_t color);

#endif
