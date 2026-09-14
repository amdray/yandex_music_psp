#include "ui/ui_screen_help.h"

#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "services/locale.h"

void ui_screen_help_render(const AppState *state)
{
    static const LocaleKey rows[] = {
        LOCALE_MENU_SELECT_PROMPT,
        LOCALE_MENU_BACK_PROMPT,
        LOCALE_NOW_PLAYING_TOGGLE_PROMPT,
        LOCALE_NOW_PLAYING_STOP_PROMPT,
        LOCALE_NOW_PLAYING_LIKE_PROMPT,
        LOCALE_HELP_EQ,
        LOCALE_HELP_SEEK,
        LOCALE_SELECT_HOLD_HINT,
    };
    float y = 48.0f;
    unsigned i;

    (void)state;
    ui_draw_clear(0xFF1A1A1A);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_HELP));
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        ui_draw_text(16.0f, y, locale_get(rows[i]), 0xFFBBBBBB);
        y += 18.0f;
    }
    ui_common_draw_prompts(LOCALE_MENU_BACK_PROMPT);
}
