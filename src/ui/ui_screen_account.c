#include "ui/ui_screen_account.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_screens.h"
#include "services/locale.h"
#include "services/token_loader.h"
#include "core/logger.h"
#include "core/fs.h"
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

void ui_screen_account_handle_input(AppState *state, const InputState *input)
{
    // Без профиля X ведёт на вход по коду (ya_auth).
    if ((input->pressed & PSP_CTRL_CROSS) && state->currentUser.uid <= 0) {
        logLine("account: login requested\n");
        ui_screens_navigate(state, SCREEN_DEVICE_LOGIN);
    }
    // С профилем квадрат — выход: чистим токен (файл + кэш) и профиль.
    if ((input->pressed & PSP_CTRL_SQUARE) && state->currentUser.uid > 0) {
        logLine("account: logout\n");
        logger_flush();
        fs_remove("config/token.txt");
        token_loader_clear();
        memset(&state->currentUser, 0, sizeof(state->currentUser));
    }
}

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
