#ifndef YM_FONTS_TEXT_H
#define YM_FONTS_TEXT_H

#include <psptypes.h>

// Initialize text subsystem: mount the CBMF font faces (9/12/16/23) from
// release/fonts/. Returns 0 on success, -1 if a font file is missing/invalid.
int text_init(void);

// Shutdown text subsystem (unload font)
void text_shutdown(void);

// Render text string at position (x, y) with color.
// UTF-8 decoding + on-demand glyph caching handled by the CBMF backend.
void text_render(float x, float y, const char *text, u32 color_abgr);

// Render text with clipping at max_width pixels
void text_render_clipped(float x, float y, const char *text, u32 color_abgr, float max_width);

// Measure text width in pixels
float text_measure_width(const char *text);

#endif
