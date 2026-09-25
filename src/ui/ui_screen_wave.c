#include "ui/ui_screen_wave.h"

#include <stdio.h>
#include <string.h>

#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "services/locale.h"
#include "core/logger.h"
#include "services/wave.h"
#include "services/token_loader.h"
#include "services/playback_controller.h"
#include "ui/ui_screens.h"

static int s_error = 0;

void ui_screen_wave_on_enter(AppState *state)
{
    char token[256];

    (void)state;
    s_error = 0;
    if (token_loader_read(token, sizeof(token)) == 0) {
        wave_start_first(token);
        memset(token, 0, sizeof(token));
    } else {
        s_error = 1;
    }
}

void ui_screen_wave_on_exit(AppState *state)
{
    (void)state;
    /* Только подобрать воркер: сессия живёт дальше (плеер+рефилл+репорт). */
    wave_dismiss_screen();
}

void ui_screen_wave_update(AppState *state)
{
    char first_id[40];
    int rc = wave_poll_first(first_id, sizeof(first_id));

    if (rc == 1) {
        memset(&state->now_playing_track, 0, sizeof(state->now_playing_track));
        snprintf(state->now_playing_track.id, sizeof(state->now_playing_track.id),
                 "%s", first_id);
        ui_screens_navigate(state, SCREEN_NOW_PLAYING);
        playback_controller_request_play_current();
    } else if (rc == -1) {
        s_error = 1;
    }
}

void ui_screen_wave_render(const AppState *state)
{
    (void)state;
    ui_draw_clear(UI_COLOR_BACKGROUND);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_WAVE));
    if (s_error) {
        ui_draw_text(16.0f, 48.0f, locale_get(LOCALE_WAVE_ERROR), UI_COLOR_ERROR);
    } else {
        ui_draw_text(16.0f, 48.0f, locale_get(LOCALE_WAVE_LOADING), UI_COLOR_ACTIVE);
    }
}
