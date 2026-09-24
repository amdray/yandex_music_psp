#ifndef YM_UI_LAYOUT_H
#define YM_UI_LAYOUT_H

#include <stddef.h>
#include <psptypes.h>

#define UI_LAYOUT_NAME_MAX 32
#define UI_LAYOUT_BINDING_MAX 48

typedef enum UiLayoutWidgetType {
    UI_LAYOUT_WIDGET_RECT = 0,
    UI_LAYOUT_WIDGET_TEXT,
    UI_LAYOUT_WIDGET_SLOT
} UiLayoutWidgetType;

typedef struct UiLayoutWidget {
    char id[UI_LAYOUT_NAME_MAX];
    UiLayoutWidgetType type;
    float x;
    float y;
    float w;
    float h;
    u32 color;
    u32 secondary_color;
    u32 background_color;
    char binding[UI_LAYOUT_BINDING_MAX];
    float step;
} UiLayoutWidget;

typedef const char *(*UiLayoutTextResolver)(const char *binding,
                                            char *buffer,
                                            size_t buffer_size,
                                            void *user_data);
typedef void (*UiLayoutSlotRenderer)(const UiLayoutWidget *widget,
                                     void *user_data);

/* Loads a fixed-size, allocation-free layout database from a text resource. */
int ui_layout_load(const char *path);
void ui_layout_unload(void);
int ui_layout_is_loaded(void);

/* Draws the screen background and its widgets in declaration order. */
int ui_layout_render(const char *screen_name,
                     UiLayoutTextResolver text_resolver,
                     UiLayoutSlotRenderer slot_renderer,
                     void *user_data);

/* Allows screen input/render code to use the same configured geometry. */
int ui_layout_get_widget(const char *screen_name, const char *widget_id,
                         UiLayoutWidget *out_widget);

#endif
