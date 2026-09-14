#ifndef CBMF_FONTS_H
#define CBMF_FONTS_H

#include "cbmf.h"
#include "cbmf_psp.h"

int  cbmf_fonts_init(void);
void cbmf_fonts_shutdown(void);

/* The selected UI font; NULL before successful init. */
CbmfPspRenderer *cbmf_fonts_get_renderer(void);
const CbmfFont  *cbmf_fonts_get_font(void);

#endif
