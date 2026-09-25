#include "fonts/cbmf_fonts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_SIZE 512
#define GLYPH_CAPACITY 2048

typedef struct CbmfFace {
    void *data;
    CbmfFont font;
    CbmfPspRenderer renderer;
} CbmfFace;

static const char *const s_paths[CBMF_FONT_COUNT] = {
    "fonts/ui.cbmf",
    "fonts/ui16.cbmf",
};
static uint8_t s_textures[CBMF_FONT_COUNT][ATLAS_SIZE * ATLAS_SIZE / 2]
    __attribute__((aligned(16)));
static uint32_t s_cluts[CBMF_FONT_COUNT][16] __attribute__((aligned(16)));
static CbmfPspGlyph s_glyphs[CBMF_FONT_COUNT][GLYPH_CAPACITY];
static CbmfFace s_faces[CBMF_FONT_COUNT];

static int load_face(CbmfFontId id)
{
    CbmfFace *face = &s_faces[id];
    FILE *file = fopen(s_paths[id], "rb");
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
        rc = cbmf_mount(&face->font, data, (size_t)size);
    if (rc == CBMF_OK) {
        const CbmfPspRendererDesc desc = {
            .texture_pixels = s_textures[id],
            .texture_width = ATLAS_SIZE,
            .texture_height = ATLAS_SIZE,
            .texture_pitch = ATLAS_SIZE,
            .glyphs = s_glyphs[id],
            .glyph_capacity = GLYPH_CAPACITY,
            .clut = s_cluts[id],
        };
        rc = cbmf_psp_init(&face->renderer, &face->font, &desc);
    }
    if (rc != 0) {
        free(data);
        memset(face, 0, sizeof(*face));
        return rc;
    }
    face->data = data;
    return 0;
}

int cbmf_fonts_init(void)
{
    int i;
    if (s_faces[CBMF_FONT_UI].data) return 0;
    for (i = 0; i < CBMF_FONT_COUNT; ++i) {
        int rc = load_face((CbmfFontId)i);
        if (rc != 0) {
            cbmf_fonts_shutdown();
            return rc;
        }
    }
    return 0;
}

void cbmf_fonts_shutdown(void)
{
    int i;
    /* Caller must finish drawing before releasing the font. */
    for (i = 0; i < CBMF_FONT_COUNT; ++i) {
        free(s_faces[i].data);
        memset(&s_faces[i], 0, sizeof(s_faces[i]));
    }
}

CbmfPspRenderer *cbmf_fonts_get_renderer(CbmfFontId id)
{
    if (id < 0 || id >= CBMF_FONT_COUNT || !s_faces[id].data) return NULL;
    return &s_faces[id].renderer;
}

const CbmfFont *cbmf_fonts_get_font(CbmfFontId id)
{
    if (id < 0 || id >= CBMF_FONT_COUNT || !s_faces[id].data) return NULL;
    return &s_faces[id].font;
}
