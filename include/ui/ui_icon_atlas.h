#ifndef YM_UI_ICON_ATLAS_H
#define YM_UI_ICON_ATLAS_H

#include <psptypes.h>

/* The atlas stores coverage only. color_abgr is supplied for every draw. */
int ui_icon_atlas_init(const char *path);
void ui_icon_atlas_shutdown(void);
int ui_icon_atlas_get_size(const char *name, int *out_width, int *out_height);
int ui_icon_atlas_draw(const char *name, int x, int y, u32 color_abgr);

#endif
