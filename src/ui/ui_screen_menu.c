#include "ui/ui_screen_menu.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_screens.h"
#include "ui/ui_layout.h"
#include "services/locale.h"
#include "services/eq.h"
#include "services/ym_api.h"
#include "services/last_play.h"
#include "services/playback_controller.h"
#include "app/app_state.h"
#include "core/logger.h"
#include "fonts/text.h"
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

// Единственный источник правды для главного меню: порядок = порядок на экране,
// метка и экран перехода в одной строке. Добавил строку — пункт сам появился
// в отрисовке, стал навигируемым и кликабельным. Рассинхрон невозможен by design.
static const MenuItem kMainMenu[] = {
    { LOCALE_MENU_NOW_PLAYING, SCREEN_NOW_PLAYING,   0 },
    { LOCALE_MENU_ALBUMS,      SCREEN_ALBUM_LIST,    0 },
    { LOCALE_MENU_PLAYLISTS,   SCREEN_PLAYLIST_LIST, 1 },
    { LOCALE_MENU_ARTISTS,     SCREEN_ARTIST,        0 },
    { LOCALE_MENU_WAVE,        SCREEN_WAVE,          0 },
    { LOCALE_MENU_SEARCH,      SCREEN_SEARCH,        0 },
    { LOCALE_MENU_SETTINGS,    SCREEN_SETTINGS,      0 },
};

static const MenuItem kSettingsMenu[] = {
    { LOCALE_MENU_ACCOUNT,     SCREEN_ACCOUNT,       0 },
    { LOCALE_MENU_NET_INFO,    SCREEN_NET_INFO,      0 },
    { LOCALE_MENU_HELP,        SCREEN_HELP,          0 },
    { LOCALE_MENU_EQ,          SCREEN_EQ,            0 },
    { LOCALE_MENU_AUDIO_QUALITY, SCREEN_COUNT,       0 },
    { LOCALE_MENU_AUTOPLAY,    SCREEN_COUNT,         0 },
    { LOCALE_MENU_MEMORY,      SCREEN_MEMORY,        0 },
};
#define MAIN_MENU_COUNT ((int)(sizeof(kMainMenu) / sizeof(kMainMenu[0])))
#define SETTINGS_MENU_COUNT ((int)(sizeof(kSettingsMenu) / sizeof(kSettingsMenu[0])))

static int menu_item_enabled(const MenuItem *item, const AppState *state)
{
    if (!item || !state) return 0;
    if (item->requires_auth && state->currentUser.uid <= 0) return 0;
    if (item->target == SCREEN_NOW_PLAYING &&
        !playback_controller_has_current()) return 0;
    return 1;
}

typedef struct MenuLayoutContext {
    const AppState *state;
    const MenuItem *items;
    int item_count;
    int selected_index;
    ScreenId pending;
    u32 pending_color;
} MenuLayoutContext;

static void menu_layout_slot(const UiLayoutWidget *widget, void *user_data)
{
    MenuLayoutContext *ctx = (MenuLayoutContext *)user_data;
    float row_step = widget->step;
    int i;

    if (strcmp(widget->binding, "menu.items") == 0) {
        for (i = 0; i < ctx->item_count; i++) {
            char quality_row[80];
            const char *label = locale_get(ctx->items[i].label);
            float y = widget->y + (float)i * row_step;
            int enabled = menu_item_enabled(&ctx->items[i], ctx->state);
            u32 color = !enabled ? UI_COLOR_INACTIVE
                                 : (i == ctx->selected_index
                                        ? widget->color : UI_COLOR_ACTIVE);
            if (enabled && ctx->pending != SCREEN_COUNT &&
                ctx->items[i].target == ctx->pending) {
                color = ctx->pending_color;
            }
            if (ctx->items[i].label == LOCALE_MENU_AUDIO_QUALITY) {
                const char *value = locale_get(
                    strcmp(ym_api_download_quality(), "hq") == 0
                        ? LOCALE_QUALITY_HIGH : LOCALE_QUALITY_NORMAL);
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label, value);
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_AUTOPLAY) {
                const char *value = locale_get(
                    last_play_autostart_enabled()
                        ? LOCALE_SETTING_ON : LOCALE_SETTING_OFF);
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label, value);
                label = quality_row;
            }
            text_render_font(TEXT_FONT_UI16, widget->x, y, label, color);
        }
    } else if (strcmp(widget->binding, "menu.selection") == 0) {
        UiLayoutWidget items;
        const MenuItem *selected = &ctx->items[ctx->selected_index];
        u32 color;

        if (ui_layout_get_widget("menu", "items", &items) != 0) return;
        row_step = items.step;
        color = menu_item_enabled(selected, ctx->state)
                    ? widget->color : UI_COLOR_INACTIVE;
        ui_draw_rect(items.x + widget->x,
                     items.y + (float)ctx->selected_index * row_step +
                         widget->y,
                     widget->w, widget->h, color);
    }
}

static void menu_handle_input(AppState *state, const InputState *input,
                              const MenuItem *items, int item_count,
                              int *selected_index, const char *log_name)
{
    if (input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        if (input->pressed & PSP_CTRL_UP) {
            *selected_index = (*selected_index + item_count - 1) % item_count;
        }
        if (input->pressed & PSP_CTRL_DOWN) {
            *selected_index = (*selected_index + 1) % item_count;
        }
        logLine("ui: %s nav index=%d/%d\n", log_name, *selected_index, item_count);
    }
    if (input->pressed & PSP_CTRL_CROSS) {
        const MenuItem *item = &items[*selected_index];
        if (item->target == SCREEN_COUNT) {
            return;
        }
        if (!menu_item_enabled(item, state)) {
            logLine("ui: %s select blocked index=%d target=%d reason=unavailable\n",
                    log_name, *selected_index, (int)item->target);
            logger_flush();
            return;
        }
        logLine("ui: %s select index=%d/%d target=%d\n",
                log_name, *selected_index, item_count, (int)item->target);
        ui_screens_navigate(state, item->target);
    }
}

static void menu_render(const AppState *state, const MenuItem *items,
                        int item_count, int selected_index, LocaleKey title)
{
    MenuLayoutContext context;
    context.state = state;
    context.items = items;
    context.item_count = item_count;
    context.selected_index = selected_index;
    context.pending = ui_screens_nav_pending();
    context.pending_color = context.pending == SCREEN_COUNT
        ? UI_COLOR_PRIMARY : ui_common_pulse_color();
    (void)ui_layout_render("menu", NULL, menu_layout_slot, &context);
    ui_common_draw_header(locale_get(title));
}

void ui_screen_menu_handle_input(AppState *state, const InputState *input)
{
    menu_handle_input(state, input, kMainMenu, MAIN_MENU_COUNT,
                      &state->menu_index, "menu");
}

void ui_screen_menu_render(const AppState *state)
{
    menu_render(state, kMainMenu, MAIN_MENU_COUNT, state->menu_index,
                LOCALE_MENU_TITLE);
}

void ui_screen_settings_handle_input(AppState *state, const InputState *input)
{
    if (state->settings_index >= 0 && state->settings_index < SETTINGS_MENU_COUNT &&
        kSettingsMenu[state->settings_index].label == LOCALE_MENU_AUDIO_QUALITY &&
        (input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) &&
        !(input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) {
        const char *quality = (input->pressed & PSP_CTRL_RIGHT) ? "hq" : "nq";
        if (strcmp(ym_api_download_quality(), quality) != 0) {
            ym_api_download_set_quality(quality);
            eq_save();
        }
    } else if (state->settings_index >= 0 &&
               state->settings_index < SETTINGS_MENU_COUNT &&
               kSettingsMenu[state->settings_index].label == LOCALE_MENU_AUTOPLAY &&
               (input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) &&
               !(input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) {
        last_play_set_autostart((input->pressed & PSP_CTRL_RIGHT) != 0);
    }
    menu_handle_input(state, input, kSettingsMenu, SETTINGS_MENU_COUNT,
                      &state->settings_index, "settings");
}

void ui_screen_settings_render(const AppState *state)
{
    menu_render(state, kSettingsMenu, SETTINGS_MENU_COUNT,
                state->settings_index, LOCALE_MENU_SETTINGS);
}
