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
#include "services/download_settings.h"
#include "services/screen_idle.h"
#include "services/cpu_clock.h"
#include "services/last_play.h"
#include "services/audio_cache.h"
#include "services/playback_controller.h"
#include "services/wave.h"
#include "services/speed_test.h"
#include "app/app_state.h"
#include "core/logger.h"
#include "fonts/text.h"
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

// Единственный источник правды для главного меню: порядок = порядок на экране,
// метка, действие и экран назначения в одной строке. Добавил строку — пункт сам появился
// в отрисовке, стал навигируемым и кликабельным. Рассинхрон невозможен by design.
static const MenuItem kMainMenu[] = {
    { LOCALE_MENU_NOW_PLAYING, SCREEN_NOW_PLAYING,   0, UI_NAV_SCREEN },
    { LOCALE_MENU_ALBUMS,      SCREEN_ALBUM_LIST,    0, UI_NAV_SCREEN },
    { LOCALE_MENU_PLAYLISTS,   SCREEN_PLAYLIST_LIST, 1, UI_NAV_SCREEN },
    { LOCALE_MENU_ARTISTS,     SCREEN_ARTIST,        0, UI_NAV_SCREEN },
    { LOCALE_MENU_WAVE,        SCREEN_NOW_PLAYING,   0, UI_NAV_WAVE },
    { LOCALE_MENU_SEARCH,      SCREEN_SEARCH,        0, UI_NAV_SCREEN },
    { LOCALE_MENU_SETTINGS,    SCREEN_SETTINGS,      0, UI_NAV_SCREEN },
};

static const MenuItem kSettingsMenu[] = {
    { LOCALE_SETTINGS_PLAYBACK, SCREEN_SETTINGS_PLAYBACK, 0, UI_NAV_SCREEN },
    { LOCALE_SETTINGS_DISPLAY, SCREEN_SETTINGS_DISPLAY, 0, UI_NAV_SCREEN },
    { LOCALE_MENU_CPU_CLOCK,   SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_ACCOUNT,     SCREEN_ACCOUNT,       0, UI_NAV_SCREEN },
    { LOCALE_SETTINGS_DIAGNOSTICS, SCREEN_SETTINGS_DIAGNOSTICS, 0, UI_NAV_SCREEN },
    { LOCALE_MENU_HELP,        SCREEN_HELP,          0, UI_NAV_SCREEN },
};

static const MenuItem kPlaybackSettingsMenu[] = {
    { LOCALE_MENU_AUDIO_CODEC, SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_AUDIO_QUALITY, SCREEN_COUNT,       0, UI_NAV_SCREEN },
    { LOCALE_MENU_EQ,          SCREEN_EQ,            0, UI_NAV_SCREEN },
    { LOCALE_MENU_AUTOPLAY,    SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_RETAIN_DOWNLOADS, SCREEN_COUNT,    0, UI_NAV_SCREEN },
};

static const MenuItem kDisplaySettingsMenu[] = {
    { LOCALE_MENU_GLOW_STYLE,  SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_THEME,       SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_IDLE,        SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_IDLE_DELAY,  SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_SCREEN_OFF,  SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_SCREEN_OFF_ON_POWER, SCREEN_COUNT, 0, UI_NAV_SCREEN },
};

static const MenuItem kDiagnosticsSettingsMenu[] = {
    { LOCALE_MENU_NET_INFO,    SCREEN_NET_INFO,      0, UI_NAV_SCREEN },
    { LOCALE_MENU_SPEED_TEST,  SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_LOGS,        SCREEN_COUNT,         0, UI_NAV_SCREEN },
    { LOCALE_MENU_MEMORY,      SCREEN_MEMORY,        0, UI_NAV_SCREEN },
};
#define MENU_COUNT(items) ((int)(sizeof(items) / sizeof((items)[0])))

static int menu_item_enabled(const MenuItem *item, const AppState *state)
{
    if (!item || !state) return 0;
    if (item->requires_auth && state->currentUser.uid <= 0) return 0;
    if (item->action == UI_NAV_SCREEN && item->target == SCREEN_NOW_PLAYING &&
        !playback_controller_has_current()) return 0;
    return 1;
}

typedef struct MenuLayoutContext {
    const AppState *state;
    const MenuItem *items;
    int item_count;
    int selected_index;
    u32 pending_color;
} MenuLayoutContext;

static void menu_layout_slot(const UiLayoutWidget *widget, void *user_data)
{
    MenuLayoutContext *ctx = (MenuLayoutContext *)user_data;
    float row_step = widget->step;
    int i;

    if (widget == &ui_layout_rows_menu__items) {
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
            if (enabled && ui_screens_nav_matches(ctx->items[i].action,
                                                  ctx->items[i].target)) {
                color = ctx->pending_color;
            }
            if (ctx->items[i].label == LOCALE_MENU_AUDIO_CODEC ||
                ctx->items[i].label == LOCALE_MENU_AUDIO_QUALITY) {
                DownloadSettings settings = download_settings_get();
                const char *value = ctx->items[i].label == LOCALE_MENU_AUDIO_CODEC
                    ? download_codec_name(settings.codec)
                    : settings.codec == DOWNLOAD_CODEC_FLAC
                        ? "lossless" : download_quality_name(settings.quality);
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label,
                         value);
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_CPU_CLOCK) {
                snprintf(quality_row, sizeof(quality_row), "%s: %d %s", label,
                         cpu_clock_mhz(), locale_get(LOCALE_UNIT_MHZ));
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
            } else if (ctx->items[i].label == LOCALE_MENU_SCREEN_OFF) {
                int delay = screen_idle_delay_s();
                if (delay == 0) {
                    snprintf(quality_row, sizeof(quality_row), "%s: %s", label,
                             locale_get(LOCALE_NEVER));
                } else {
                    snprintf(quality_row, sizeof(quality_row), "%s: %d %s", label,
                             delay >= 60 ? delay / 60 : delay,
                             locale_get(delay >= 60 ? LOCALE_UNIT_MIN
                                                    : LOCALE_UNIT_SEC));
                }
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_SCREEN_OFF_ON_POWER) {
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label,
                         locale_get(screen_idle_off_on_power()
                             ? LOCALE_YES : LOCALE_NO));
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
            } else if (ctx->items[i].label == LOCALE_MENU_RETAIN_DOWNLOADS) {
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label,
                         locale_get(audio_cache_retain_downloads()
                             ? LOCALE_SETTING_ON : LOCALE_SETTING_OFF));
                label = quality_row;
            } else if (ctx->items[i].label == LOCALE_MENU_GLOW_STYLE) {
                const char *value = locale_get(
                    glow_bg_style_name_key(glow_bg_get_style()));
                snprintf(quality_row, sizeof(quality_row), "%s: %s", label, value);
                label = quality_row;
            }
            text_render_font(TEXT_FONT_UI16, widget->x, y, label, color);
        }
    } else if (widget == &ui_layout_slot_menu__selection) {
        UiLayoutWidget items;
        const MenuItem *selected = &ctx->items[ctx->selected_index];
        u32 color;

        items = ui_layout_rows_menu__items;
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
        if (item->action == UI_NAV_WAVE) {
            WaveRequestParams params = {0};
            ui_screens_start_wave(state, &params);
        } else {
            ui_screens_navigate(state, item->target);
        }
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
    context.pending_color = ui_screens_nav_action() == UI_NAV_NONE
        ? UI_COLOR_PRIMARY : ui_common_pulse_color();
    (void)ui_layout_render(&ui_layout_screen_menu, menu_layout_slot, &context);
    ui_common_draw_header(locale_get(title));
}

void ui_screen_menu_handle_input(AppState *state, const InputState *input)
{
    menu_handle_input(state, input, kMainMenu, MENU_COUNT(kMainMenu),
                      &state->menu_index, "menu");
}

void ui_screen_menu_render(const AppState *state)
{
    menu_render(state, kMainMenu, MENU_COUNT(kMainMenu), state->menu_index,
                LOCALE_MENU_TITLE);
}

static void settings_item_input(LocaleKey label, const InputState *input)
{
    if (label == LOCALE_MENU_SPEED_TEST &&
        (input->pressed & PSP_CTRL_CROSS)) {
        speed_test_request();
        return;
    }
    if (!(input->pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) ||
        (input->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS))) return;

    if (label == LOCALE_MENU_AUDIO_CODEC || label == LOCALE_MENU_AUDIO_QUALITY) {
        DownloadSettings settings = download_settings_get();
        int forward = (input->pressed & PSP_CTRL_RIGHT) != 0;
        if (label == LOCALE_MENU_AUDIO_CODEC) {
            int codec = (settings.codec + (forward ? 1 : DOWNLOAD_CODEC_COUNT - 1))
                        % DOWNLOAD_CODEC_COUNT;
            download_settings_set_codec((DownloadCodec)codec);
        } else if (settings.codec != DOWNLOAD_CODEC_FLAC) {
            int quality = (settings.quality + (forward ? 1 : DOWNLOAD_QUALITY_COUNT - 1))
                          % DOWNLOAD_QUALITY_COUNT;
            download_settings_set_quality((DownloadQuality)quality);
        }
    } else if (label == LOCALE_MENU_CPU_CLOCK) {
        cpu_clock_cycle((input->pressed & PSP_CTRL_RIGHT) ? 1 : -1);
    } else if (label == LOCALE_MENU_AUTOPLAY) {
        last_play_set_autostart((input->pressed & PSP_CTRL_RIGHT) != 0);
    } else if (label == LOCALE_MENU_RETAIN_DOWNLOADS) {
        audio_cache_set_retain_downloads((input->pressed & PSP_CTRL_RIGHT) != 0);
    } else if (label == LOCALE_MENU_THEME) {
        ui_theme_cycle((input->pressed & PSP_CTRL_RIGHT) ? 1 : -1);
    } else if (label == LOCALE_MENU_IDLE) {
        idle_cycle_mode((input->pressed & PSP_CTRL_RIGHT) ? 1 : -1);
    } else if (label == LOCALE_MENU_IDLE_DELAY) {
        idle_cycle_delay((input->pressed & PSP_CTRL_RIGHT) ? 1 : -1);
    } else if (label == LOCALE_MENU_SCREEN_OFF) {
        screen_idle_cycle_delay((input->pressed & PSP_CTRL_RIGHT) ? 1 : -1);
    } else if (label == LOCALE_MENU_SCREEN_OFF_ON_POWER) {
        screen_idle_set_off_on_power((input->pressed & PSP_CTRL_RIGHT) != 0);
    } else if (label == LOCALE_MENU_LOGS) {
        logger_set_enabled((input->pressed & PSP_CTRL_RIGHT) != 0);
    } else if (label == LOCALE_MENU_GLOW_STYLE) {
        int style = glow_bg_get_style();
        if (input->pressed & PSP_CTRL_RIGHT) {
            style = (style + 1) % GLOW_STYLE_COUNT;
        } else {
            style = (style + GLOW_STYLE_COUNT - 1) % GLOW_STYLE_COUNT;
        }
        glow_bg_set_style(style);
    }
}

static void settings_handle_input(AppState *state, const InputState *input,
                                  const MenuItem *items, int item_count,
                                  int *selected_index)
{
    if (*selected_index >= 0 && *selected_index < item_count) {
        settings_item_input(items[*selected_index].label, input);
    }
    menu_handle_input(state, input, items, item_count, selected_index, "settings");
}

void ui_screen_settings_handle_input(AppState *state, const InputState *input)
{
    settings_handle_input(state, input, kSettingsMenu, MENU_COUNT(kSettingsMenu),
                          &state->settings_index);
}

void ui_screen_settings_render(const AppState *state)
{
    menu_render(state, kSettingsMenu, MENU_COUNT(kSettingsMenu),
                state->settings_index, LOCALE_MENU_SETTINGS);
}

void ui_screen_playback_settings_handle_input(AppState *state, const InputState *input)
{
    settings_handle_input(state, input, kPlaybackSettingsMenu,
                          MENU_COUNT(kPlaybackSettingsMenu),
                          &state->playback_settings_index);
}

void ui_screen_playback_settings_render(const AppState *state)
{
    menu_render(state, kPlaybackSettingsMenu, MENU_COUNT(kPlaybackSettingsMenu),
                state->playback_settings_index, LOCALE_SETTINGS_PLAYBACK);
}

void ui_screen_display_settings_handle_input(AppState *state, const InputState *input)
{
    settings_handle_input(state, input, kDisplaySettingsMenu,
                          MENU_COUNT(kDisplaySettingsMenu),
                          &state->display_settings_index);
}

void ui_screen_display_settings_render(const AppState *state)
{
    menu_render(state, kDisplaySettingsMenu, MENU_COUNT(kDisplaySettingsMenu),
                state->display_settings_index, LOCALE_SETTINGS_DISPLAY);
}

void ui_screen_diagnostics_settings_handle_input(AppState *state, const InputState *input)
{
    settings_handle_input(state, input, kDiagnosticsSettingsMenu,
                          MENU_COUNT(kDiagnosticsSettingsMenu),
                          &state->diagnostics_settings_index);
}

void ui_screen_diagnostics_settings_render(const AppState *state)
{
    menu_render(state, kDiagnosticsSettingsMenu, MENU_COUNT(kDiagnosticsSettingsMenu),
                state->diagnostics_settings_index, LOCALE_SETTINGS_DIAGNOSTICS);
}
