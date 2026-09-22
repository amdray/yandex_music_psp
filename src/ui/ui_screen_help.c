#include "ui/ui_screen_help.h"

#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_layout.h"
#include "services/locale.h"

#include <string.h>

static const LocaleKey s_help_rows[] = {
    LOCALE_MENU_SELECT_PROMPT,
    LOCALE_MENU_BACK_PROMPT,
    LOCALE_NOW_PLAYING_TOGGLE_PROMPT,
    LOCALE_NOW_PLAYING_STOP_PROMPT,
    LOCALE_NOW_PLAYING_LIKE_PROMPT,
    LOCALE_HELP_EQ,
    LOCALE_HELP_SEEK,
    LOCALE_SELECT_HOLD_HINT,
};

static void help_layout_slot(const UiLayoutWidget *widget, void *user_data)
{
    float step = widget->step;
    unsigned i;
    (void)user_data;
    if (strcmp(widget->binding, "help.rows") == 0) {
        for (i = 0; i < sizeof(s_help_rows) / sizeof(s_help_rows[0]); i++) {
            ui_draw_text(widget->x, widget->y + (float)i * step,
                         locale_get(s_help_rows[i]), widget->color);
        }
    }
}

void ui_screen_help_render(const AppState *state)
{
    (void)state;
    (void)ui_layout_render("help", NULL, help_layout_slot, NULL);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_HELP));
}
