#include "ui/ui_screen_menu.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_screens.h"
#include "services/locale.h"
#include "app/app_state.h"
#include "core/logger.h"
#include <pspctrl.h>

// Единственный источник правды для главного меню: порядок = порядок на экране,
// метка и экран перехода в одной строке. Добавил строку — пункт сам появился
// в отрисовке, стал навигируемым и кликабельным. Рассинхрон невозможен by design.
static const MenuItem kMainMenu[] = {
    { LOCALE_MENU_NOW_PLAYING, SCREEN_NOW_PLAYING   },
    { LOCALE_MENU_ALBUMS,      SCREEN_ALBUM_LIST    },
    { LOCALE_MENU_PLAYLISTS,   SCREEN_PLAYLIST_LIST },
    { LOCALE_MENU_ACCOUNT,     SCREEN_ACCOUNT       },
    { LOCALE_MENU_ARTISTS,     SCREEN_ARTIST        },
    { LOCALE_MENU_NET_INFO,    SCREEN_NET_INFO      },
    { LOCALE_MENU_EQ,          SCREEN_EQ            },
    { LOCALE_MENU_WAVE,        SCREEN_WAVE          },
};
#define MAIN_MENU_COUNT ((int)(sizeof(kMainMenu) / sizeof(kMainMenu[0])))

void ui_screen_menu_handle_input(AppState *state, const InputState *input)
{
    if (input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        if (input->pressed & PSP_CTRL_UP) {
            state->menu_index = (state->menu_index + MAIN_MENU_COUNT - 1) % MAIN_MENU_COUNT;
        }
        if (input->pressed & PSP_CTRL_DOWN) {
            state->menu_index = (state->menu_index + 1) % MAIN_MENU_COUNT;
        }
        logLine("ui: menu nav index=%d/%d\n", state->menu_index, MAIN_MENU_COUNT);
    }
    if (input->pressed & PSP_CTRL_CROSS) {
        logLine("ui: menu select index=%d/%d target=%d\n",
                state->menu_index, MAIN_MENU_COUNT, (int)kMainMenu[state->menu_index].target);
        /* Gated: content screens are entered only when their visible content
           is loaded; until then we wait here with the "..." indicator. */
        ui_screens_navigate(state, kMainMenu[state->menu_index].target);
    }
}

void ui_screen_menu_render(const AppState *state)
{
    ui_draw_clear(0xFF1A1A1A);
    ui_common_draw_header(locale_get(LOCALE_MENU_TITLE));
    ui_common_draw_menu(kMainMenu, MAIN_MENU_COUNT, state->menu_index);

    /* Pending gated navigation: mark the item being prepared. */
    if (ui_screens_nav_pending() != SCREEN_COUNT) {
        int i;
        for (i = 0; i < MAIN_MENU_COUNT; i++) {
            if (kMainMenu[i].target == ui_screens_nav_pending()) {
                ui_draw_text(200.0f, 48.0f + (float)i * 16.0f, "...", 0xFF00D5FF);
                break;
            }
        }
    }

    ui_common_draw_prompts(LOCALE_MENU_SELECT_PROMPT, LOCALE_MENU_BACK_PROMPT, LOCALE_MENU_EXIT_PROMPT);
}
