#ifndef CBMF_FONTS_H
#define CBMF_FONTS_H

#include "cbmf.h"
#include "cbmf_psp.h"

typedef enum CbmfFontId {
    CBMF_FONT_UI = 0,
    CBMF_FONT_UI16,
    CBMF_FONT_COUNT
} CbmfFontId;

int  cbmf_fonts_init(void);
void cbmf_fonts_shutdown(void);

/* Immutable UI faces; NULL before successful init or for an invalid id. */
CbmfPspRenderer *cbmf_fonts_get_renderer(CbmfFontId id);
const CbmfFont  *cbmf_fonts_get_font(CbmfFontId id);

#endif
