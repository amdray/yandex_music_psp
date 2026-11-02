/* Проверен и зафиксирован: отрисовка фона, прямоугольников и вызов
 * обработчиков слотов. Проверки раскладки и сборка пройдены. */
#define UI_LAYOUT_IMPLEMENTATION
#include "ui/ui_layout.h"
#include "ui/ui_draw.h"

void ui_layout_render(const UiLayoutScreen *screen,
                      UiLayoutSlotRenderer slot_renderer,
                      void *user_data)
{
    size_t i;
    ui_draw_clear(screen->background);
    for (i = 0; i < screen->widget_count; i++) {
        const UiLayoutWidget *widget = screen->widgets[i];
        if (widget->type == UI_LAYOUT_WIDGET_RECT) {
            ui_draw_rect(widget->x, widget->y, widget->w, widget->h,
                         widget->color);
        } else if (widget->type == UI_LAYOUT_WIDGET_SLOT && slot_renderer) {
            slot_renderer(widget, user_data);
        }
    }
}
