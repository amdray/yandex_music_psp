#include "fonts/cbmf_fonts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>

#include "core/fs.h"
#include "core/logger.h"
#include "ui/ui_theme.h"

#define ATLAS_SIZE 512
#define GLYPH_CAPACITY 2048

typedef struct CbmfFace {
    void *data;
    CbmfFont font;
    CbmfPspRenderer renderer;
} CbmfFace;

#define FONT_VARIANTS 4
static const char *const s_paths[FONT_VARIANTS][CBMF_FONT_COUNT] = {
    { "fonts/ui.cbmf",     "fonts/ui16.cbmf" },
    { "fonts/ui_gri.cbmf", "fonts/ui16_gri.cbmf" },
    { "fonts/ui_pdp.cbmf", "fonts/ui16_pdp.cbmf" },
    { "fonts/ui_cov.cbmf", "fonts/ui16_cov.cbmf" },
};
static int s_variant = 0;
static uint8_t s_textures[CBMF_FONT_COUNT][ATLAS_SIZE * ATLAS_SIZE / 2]
    __attribute__((aligned(16)));
static uint32_t s_cluts[CBMF_FONT_COUNT][16] __attribute__((aligned(16)));
static CbmfPspGlyph s_glyphs[CBMF_FONT_COUNT][GLYPH_CAPACITY];
static CbmfFace s_faces[CBMF_FONT_COUNT];

/* Весь файл одним fs_read (раньше fopen/fread через newlib читал мелкими
 * кусками: на медленной карте это ~9 с на старте). */
static void *read_whole_file(const char *path, size_t *out_size)
{
    SceIoStat st;
    SceUID fd;
    void *data;
    size_t size, done = 0;

    if (fs_getstat(path, &st) != 0 || st.st_size <= 0) return NULL;
    size = (size_t)st.st_size;
    fd = fs_open(path, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    data = malloc(size);
    if (!data) {
        fs_close(fd);
        return NULL;
    }
    while (done < size) {
        int n = fs_read(fd, (uint8_t *)data + done, size - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    fs_close(fd);
    if (done != size) {
        free(data);
        return NULL;
    }
    *out_size = size;
    return data;
}

static int load_face(CbmfFontId id)
{
    CbmfFace *face = &s_faces[id];
    size_t size = 0;
    u64 t0 = sceKernelGetSystemTimeWide();
    void *data = read_whole_file(s_paths[s_variant][id], &size);
    if (!data) return -1;
    logLine("text: font %d read %u bytes in %u ms\n", (int)id, (unsigned)size,
            (unsigned)((sceKernelGetSystemTimeWide() - t0) / 1000));
    int rc = cbmf_mount(&face->font, data, size);
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
    logLine("text: font %d ready rc=%d total %u ms\n", (int)id, rc,
            (unsigned)((sceKernelGetSystemTimeWide() - t0) / 1000));
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
    s_variant = ui_theme_get();
    if (s_variant < 0 || s_variant >= FONT_VARIANTS) s_variant = 0;
    if (s_variant != 0) {
        /* Гарнитура темы: нет файлов — стартуем с обычной. */
        int ok = 1;
        for (i = 0; i < CBMF_FONT_COUNT; ++i) {
            if (load_face((CbmfFontId)i) != 0) { ok = 0; break; }
        }
        if (ok) return 0;
        cbmf_fonts_shutdown();
        logLine("text: theme font variant %d missing, default font\n", s_variant);
        s_variant = 0;
    }
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

int cbmf_fonts_set_variant(int variant)
{
    int i, ok = 1;
    if (variant < 0 || variant >= FONT_VARIANTS) variant = 0;
    if (variant == s_variant && s_faces[CBMF_FONT_UI].data) return 0;
    cbmf_fonts_shutdown();
    s_variant = variant;
    for (i = 0; i < CBMF_FONT_COUNT; ++i) {
        if (load_face((CbmfFontId)i) != 0) { ok = 0; break; }
    }
    if (ok) return 0;
    cbmf_fonts_shutdown();
    s_variant = 0;
    for (i = 0; i < CBMF_FONT_COUNT; ++i) {
        (void)load_face((CbmfFontId)i);
    }
    return -1;
}
