#include "ui/ui_screen_menu.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_screens.h"
#include "ui/ui_layout.h"
#include "ui/ui_glow_bg.h"
#include "ui/ui_idle.h"
#include "ui/ui_theme.h"
#include "services/locale.h"
#include "services/eq.h"
#include "services/ym_api.h"
#include "services/last_play.h"
#include "services/playback_controller.h"
#include "services/speed_test.h"
#include "app/app_state.h"
#include "core/logger.h"
#include "services/net_speed.h"
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
    { LOCALE_MENU_SPEED_TEST,  SCREEN_COUNT,         0 },
    { LOCALE_MENU_HELP,        SCREEN_HELP,          0 },
    { LOCALE_MENU_EQ,          SCREEN_EQ,            0 },
    { LOCALE_MENU_AUDIO_QUALITY, SCREEN_COUNT,       0 },
    { LOCALE_MENU_AUTOPLAY,    SCREEN_COUNT,         0 },
    { LOCALE_MENU_GLOW_STYLE,  SCREEN_COUNT,         0 },
    { LOCALE_MENU_THEME,       SCREEN_COUNT,         0 },
    { LOCALE_MENU_IDLE,        SCREEN_COUNT,         0 },
    { LOCALE_MENU_IDLE_DELAY,  SCREEN_COUNT,         0 },
    { LOCALE_MENU_LOGS,        SCREEN_COUNT,         0 },
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
        int visible = (int)((262.0f - widget->y) / row_step);
        int first = 0;
        if (visible < 1) visible = 1;
        if (ctx->item_count > visible && ctx->selected_index >= visible) {
            first = ctx->selected_index - (visible - 1);
        }
        for (i = first; i < ctx->item_count && i < first + visible; i++) {
            char quality_row[80];
            const char *label = locale_get(ctx->items[i].label);
            float y = widget->y + (float)(i - first) * row_step;
            int enabled = menu_item_enabled(&ctx->items[i], ctx->state);
            u32 color = !enabled ? UI_COLOR_INACTIVE
                                 : (i == ctx->selected_index
                                        ? widget->color : UI_COLOR_ACTIVE);
            if (enabled && ctx->pending != SCREEN_COUNT &&
                ctx->items[i].target == ctx->pending) {
                color = ctx->pending_color;
            }
            if (ctx->items[i].label == LOCALE_MENU_AUDIO_QUALITY) {
                const char *q = ym_api_download_quality();
                LocaleKey key = LOCALE_QUALITY_NORMAL;
                if (q && strcmp(q, "hq") == 0) key = LOCALE_QUALITY_HIGH;
                else if (q && strcmp(q, "lq") == 0) key = LOCALE_QUALITY_LOW;
                else if (q && strcmp(q, "mp3") == 0) key = LOCALE_QUALITY_MP3;
                else if (q && strcmp(q, "flac") == 0) key = LOCALE_QUALITY_FLAC;
                else if (q && strcmp(q, "auto") == 0) key = LOCALE_QUALITY_AUTO;
                if (key == LOCALE_QUALITY_AUTO) {
                    /* Авто: показываем, что выбрано сейчас по скорости сети. */
                    const char *e = ym_api_download_effective_quality();
                    LocaleKey ek = LOCALE_QUALITY_NORMAL;
                    if (e && strcmp(e, "hq") == 0) ek = LOCALE_QUALITY_HIGH;
                    else if (e && strcmp(e, "lq") == 0) ek = LOCALE_QUALITY_LOW;
                    snprintf(quality_row, sizeof(quality_row), "%s: %s (%s)",
                             label, locale_get(key), locale_get(ek));
                } else {
                    snprintf(quality_row, sizeof(quality_row), "%s: %s", label,
                             locale_get(key));
                }
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_LOGS) {
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label,
                         locale_get(logger_enabled() ? LOCALE_SETTING_ON
                                                     : LOCALE_SETTING_OFF));
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_THEME) {
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label,
                         locale_get(ui_theme_name_key(ui_theme_get())));
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_IDLE) {
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label,
                         locale_get(idle_mode_name_key(idle_get_mode())));
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_IDLE_DELAY) {
                snprintf(quality_row, sizeof(quality_row), "%s: %d %s", label,
                         idle_get_delay_s(), locale_get(LOCALE_UNIT_SEC));
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_SPEED_TEST) {
                char st[40];
                speed_test_status(st, sizeof(st));
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label, st);
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_AUTOPLAY) {
                const char *value = locale_get(
                    last_play_autostart_enabled()
                        ? LOCALE_SETTING_ON : LOCALE_SETTING_OFF);
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label, value);
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_GLOW_STYLE) {
                const char *value = locale_get(
                    glow_bg_style_name_key(glow_bg_get_style()));
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
        int vis = (int)((262.0f - items.y) / row_step);
        int firstrow = 0;
        if (vis < 1) vis = 1;
        if (ctx->item_count > vis && ctx->selected_index >= vis) {
            firstrow = ctx->selected_index - (vis - 1);
        }
        ui_draw_rect(items.x + widget->x,
                     items.y + (float)(ctx->selected_index - firstrow) * row_step +
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
        kSettingsMenu[state->settings_index].label == LOCALE_MENU_SPEED_TEST &&
        (input->pressed & PSP_CTRL_CROSS)) {
        speed_test_request();
    } else if (state->settings_index >= 0 && state->settings_index < SETTINGS_MENU_COUNT &&
        kSettingsMenu[state->settings_index].label == LOCALE_MENU_AUDIO_QUALITY &&
        (input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) &&
        !(input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) {
        static const char *const kQualityOrder[] = { "lq", "nq", "hq", "auto", "mp3", "flac" };
        const int n_q = (int)(sizeof(kQualityOrder) / sizeof(kQualityOrder[0]));
        const char *cur = ym_api_download_quality();
        int idx = 1, qi; /* nq */
        for (qi = 0; qi < n_q; qi++) {
            if (cur && strcmp(cur, kQualityOrder[qi]) == 0) { idx = qi; break; }
        }
        if (input->pressed & PSP_CTRL_RIGHT) {
            idx = (idx + 1) % n_q;
        } else {
            idx = (idx + n_q - 1) % n_q;
        }
        if (strcmp(cur ? cur : "", kQualityOrder[idx]) != 0) {
            ym_api_download_set_quality(kQualityOrder[idx]);
            eq_save();
        }
    } else if (state->settings_index >= 0 &&
               state->settings_index < SETTINGS_MENU_COUNT &&
               kSettingsMenu[state->settings_index].label == LOCALE_MENU_AUTOPLAY &&
               (input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) &&
               !(input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) {
        last_play_set_autostart((input->pressed & PSP_CTRL_RIGHT) != 0);
    } else if (state->settings_index >= 0 &&
               state->settings_index < SETTINGS_MENU_COUNT &&
               kSettingsMenu[state->settings_index].label == LOCALE_MENU_THEME &&
               (input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) &&
               !(input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) {
        ui_theme_cycle((input->pressed & PSP_CTRL_RIGHT) ? 1 : -1);
    } else if (state->settings_index >= 0 &&
               state->settings_index < SETTINGS_MENU_COUNT &&
               kSettingsMenu[state->settings_index].label == LOCALE_MENU_IDLE &&
               (input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) &&
               !(input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) {
        idle_cycle_mode((input->pressed & PSP_CTRL_RIGHT) ? 1 : -1);
    } else if (state->settings_index >= 0 &&
               state->settings_index < SETTINGS_MENU_COUNT &&
               kSettingsMenu[state->settings_index].label == LOCALE_MENU_IDLE_DELAY &&
               (input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) &&
               !(input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) {
        idle_cycle_delay((input->pressed & PSP_CTRL_RIGHT) ? 1 : -1);
    } else if (state->settings_index >= 0 &&
               state->settings_index < SETTINGS_MENU_COUNT &&
               kSettingsMenu[state->settings_index].label == LOCALE_MENU_LOGS &&
               (input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) &&
               !(input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) {
        logger_set_enabled((input->pressed & PSP_CTRL_RIGHT) != 0);
    } else if (state->settings_index >= 0 &&
               state->settings_index < SETTINGS_MENU_COUNT &&
               kSettingsMenu[state->settings_index].label == LOCALE_MENU_GLOW_STYLE &&
               (input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) &&
               !(input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) {
        int style = glow_bg_get_style();
        if (input->pressed & PSP_CTRL_RIGHT) {
            style = (style + 1) % GLOW_STYLE_COUNT;
        } else {
            style = (style + GLOW_STYLE_COUNT - 1) % GLOW_STYLE_COUNT;
        }
        glow_bg_set_style(style);
    }
    menu_handle_input(state, input, kSettingsMenu, SETTINGS_MENU_COUNT,
                      &state->settings_index, "settings");
}

void ui_screen_settings_render(const AppState *state)
{
    menu_render(state, kSettingsMenu, SETTINGS_MENU_COUNT,
                state->settings_index, LOCALE_MENU_SETTINGS);
}
