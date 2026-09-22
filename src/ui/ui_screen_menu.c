#include "ui/ui_screen_menu.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_screens.h"
#include "ui/ui_layout.h"
#include "services/locale.h"
#include "app/app_state.h"
#include "core/logger.h"
#include <pspctrl.h>
#include <string.h>

// Единственный источник правды для главного меню: порядок = порядок на экране,
// метка и экран перехода в одной строке. Добавил строку — пункт сам появился
// в отрисовке, стал навигируемым и кликабельным. Рассинхрон невозможен by design.
static const MenuItem kMainMenu[] = {
    { LOCALE_MENU_NOW_PLAYING, SCREEN_NOW_PLAYING,   0 },
    { LOCALE_MENU_ALBUMS,      SCREEN_ALBUM_LIST,    0 },
    { LOCALE_MENU_PLAYLISTS,   SCREEN_PLAYLIST_LIST, 1 },
    { LOCALE_MENU_ACCOUNT,     SCREEN_ACCOUNT,       0 },
    { LOCALE_MENU_ARTISTS,     SCREEN_ARTIST,        0 },
    { LOCALE_MENU_NET_INFO,    SCREEN_NET_INFO,      0 },
    { LOCALE_MENU_EQ,          SCREEN_EQ,            0 },
    { LOCALE_MENU_WAVE,        SCREEN_WAVE,          0 },
    { LOCALE_MENU_SEARCH,      SCREEN_SEARCH,        0 },
    { LOCALE_MENU_HELP,        SCREEN_HELP,          0 },
};
#define MAIN_MENU_COUNT ((int)(sizeof(kMainMenu) / sizeof(kMainMenu[0])))

static int menu_item_enabled(const MenuItem *item, const AppState *state)
{
    return item && state &&
           (!item->requires_auth || state->currentUser.uid > 0);
}

typedef struct MenuLayoutContext {
    const AppState *state;
    ScreenId pending;
    u32 pending_color;
} MenuLayoutContext;

static void menu_layout_slot(const UiLayoutWidget *widget, void *user_data)
{
    MenuLayoutContext *ctx = (MenuLayoutContext *)user_data;
    float row_step = widget->step;
    int i;

    if (strcmp(widget->binding, "menu.items") == 0) {
        for (i = 0; i < MAIN_MENU_COUNT; i++) {
            float y = widget->y + (float)i * row_step;
            int enabled = menu_item_enabled(&kMainMenu[i], ctx->state);
            u32 color = !enabled ? 0xFF666666
                                 : (i == ctx->state->menu_index
                                        ? widget->color : 0xFFBBBBBB);
            if (enabled && kMainMenu[i].target == ctx->pending) {
                color = ctx->pending_color;
            }
            ui_draw_text(widget->x, y,
                         locale_get(kMainMenu[i].label), color);
        }
    } else if (strcmp(widget->binding, "menu.selection") == 0) {
        UiLayoutWidget items;
        const MenuItem *selected = &kMainMenu[ctx->state->menu_index];
        u32 color;

        if (ui_layout_get_widget("menu", "items", &items) != 0) return;
        row_step = items.step;
        color = menu_item_enabled(selected, ctx->state)
                    ? widget->color : 0xFF555555;
        ui_draw_rect(items.x + widget->x,
                     items.y + (float)ctx->state->menu_index * row_step +
                         widget->y,
                     widget->w, widget->h, color);
    }
}

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
        const MenuItem *item = &kMainMenu[state->menu_index];
        if (!menu_item_enabled(item, state)) {
            logLine("ui: menu select blocked index=%d target=%d reason=auth_required\n",
                    state->menu_index, (int)item->target);
            logger_flush();
            return;
        }
        logLine("ui: menu select index=%d/%d target=%d\n",
                state->menu_index, MAIN_MENU_COUNT, (int)item->target);
        /* Gated: content screens are entered only when their visible content
           is loaded; until then the selected label pulses. */
        ui_screens_navigate(state, item->target);
    }
}

void ui_screen_menu_render(const AppState *state)
{
    MenuLayoutContext context;
    context.state = state;
    context.pending = ui_screens_nav_pending();
    context.pending_color = context.pending == SCREEN_COUNT
        ? 0xFFFFFFFF : ui_common_pulse_color();
    (void)ui_layout_render("menu", NULL, menu_layout_slot, &context);
    ui_common_draw_header(locale_get(LOCALE_MENU_TITLE));
}
