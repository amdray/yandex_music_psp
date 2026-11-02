#ifndef YM_UI_LAYOUT_H
#define YM_UI_LAYOUT_H

#include <stddef.h>
#include <psptypes.h>

typedef enum UiLayoutWidgetType {
    UI_LAYOUT_WIDGET_RECT = 0,
    UI_LAYOUT_WIDGET_SLOT
} UiLayoutWidgetType;

typedef struct UiLayoutWidget {
    UiLayoutWidgetType type;
    float x;
    float y;
    float w;
    float h;
    u32 color;
    u32 secondary_color;
    u32 background_color;
    float step;
} UiLayoutWidget;

typedef struct UiLayoutScreen {
    u32 background;
    size_t widget_count;
    const UiLayoutWidget *const *widgets;
} UiLayoutScreen;

#include "ui_layout_generated.h"

typedef void (*UiLayoutSlotRenderer)(const UiLayoutWidget *widget,
                                     void *user_data);

/* Draws the screen background and its widgets in declaration order. */
void ui_layout_render(const UiLayoutScreen *screen,
                     UiLayoutSlotRenderer slot_renderer,
                     void *user_data);

#endif
