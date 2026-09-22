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

// Render a horizontal window of the text: skip skip_px pixels, including a
// partial leading glyph, and draw enough content for max_width pixels. The
// caller's active scissor performs exact edge clipping. skip_px<=0 behaves
// like text_render_clipped; max_width<=0 draws nothing.
void text_render_window(float x, float y, const char *text, u32 color_abgr, float skip_px, float max_width);

// Measure text width in pixels
float text_measure_width(const char *text);

#endif
