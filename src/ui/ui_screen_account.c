#include "ui/ui_screen_account.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "services/locale.h"
#include <stdio.h>

void ui_screen_account_render(const AppState *state)
{
    ui_draw_clear(0xFF1A1A1A);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_ACCOUNT));
    {
        char buf[256];
        float y = 48.0f;

        if (state->currentUser.uid > 0) {
            snprintf(buf, sizeof(buf), "%s: %s",
                locale_get(LOCALE_ACCOUNT_USER),
                state->currentUser.display_name);
            ui_draw_text(16.0f, y, buf, 0xFFFFFFFF);
            y += 16.0f;

            snprintf(buf, sizeof(buf), "%s: %d", 
                     locale_get(LOCALE_ACCOUNT_UID), 
                     state->currentUser.uid);
            ui_draw_text(16.0f, y, buf, 0xFFBBBBBB);
            y += 24.0f;

            if (state->currentUser.subscription_active) {
                ui_draw_text(16.0f, y, locale_get(LOCALE_ACCOUNT_SUBSCRIPTION_ACTIVE), 0xFF00FF00);
                y += 12.0f;
                snprintf(buf, sizeof(buf), "%s: %s", 
                         locale_get(LOCALE_ACCOUNT_SUBSCRIPTION_EXPIRES),
                         state->currentUser.subscription_end);
                ui_draw_text(20.0f, y, buf, 0xFFBBBBBB);

            } else {
                ui_draw_text(16.0f, y, locale_get(LOCALE_ACCOUNT_SUBSCRIPTION_INACTIVE), 0xFFFF0000);
            }
        } else {
            ui_draw_text(16.0f, 48.0f, locale_get(LOCALE_ACCOUNT_LOAD_ERROR), 0xFFFF4444);
        }
    }
}
