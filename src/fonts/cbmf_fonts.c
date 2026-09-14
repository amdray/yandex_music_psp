#include "fonts/cbmf_fonts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_SIZE 512
#define GLYPH_CAPACITY 2048

static uint8_t s_texture[ATLAS_SIZE * ATLAS_SIZE / 2] __attribute__((aligned(16)));
static uint32_t s_clut[16] __attribute__((aligned(16)));
static CbmfPspGlyph s_glyphs[GLYPH_CAPACITY];
static void *s_font_data;
static CbmfFont s_font;
static CbmfPspRenderer s_renderer;

int cbmf_fonts_init(void)
{
    if (s_font_data) return 0;
    FILE *file = fopen("fonts/ui.cbmf", "rb");
    if (!file) return -1;
    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    void *data = malloc((size_t)size);
    if (!data) {
        fclose(file);
        return -1;
    }
    size_t bytes = fread(data, 1, (size_t)size, file);
    int close_rc = fclose(file);
    int rc = -1;
    if (bytes == (size_t)size && close_rc == 0)
        rc = cbmf_mount(&s_font, data, (size_t)size);
    if (rc == CBMF_OK) {
        const CbmfPspRendererDesc desc = {
            .texture_pixels = s_texture,
            .texture_width = ATLAS_SIZE,
            .texture_height = ATLAS_SIZE,
            .texture_pitch = ATLAS_SIZE,
            .glyphs = s_glyphs,
            .glyph_capacity = GLYPH_CAPACITY,
            .clut = s_clut,
        };
        rc = cbmf_psp_init(&s_renderer, &s_font, &desc);
    }
    if (rc != 0) {
        free(data);
        memset(&s_font, 0, sizeof(s_font));
        memset(&s_renderer, 0, sizeof(s_renderer));
        return rc;
    }
    s_font_data = data;
    return 0;
}

void cbmf_fonts_shutdown(void)
{
    /* Caller must finish drawing before releasing the font. */
    free(s_font_data);
    s_font_data = NULL;
    memset(&s_font, 0, sizeof(s_font));
    memset(&s_renderer, 0, sizeof(s_renderer));
}

CbmfPspRenderer *cbmf_fonts_get_renderer(void)
{
    return s_font_data ? &s_renderer : NULL;
}

const CbmfFont *cbmf_fonts_get_font(void)
{
    return s_font_data ? &s_font : NULL;
}
