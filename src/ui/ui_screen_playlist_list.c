#include "ui/ui_screen_playlist_list.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_screen_track_list.h"  // Для ui_screen_track_list_clear_cache
#include "services/locale.h"
#include "services/cover_manager.h"
#include "services/library.h"
#include "services/ym_api.h"       // NET_LOAD_ERR_* cause codes
#include "fonts/text.h"
#include "core/logger.h"
#include "app/app_state.h"
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

// Отслеживаем последний видимый диапазон, чтобы не запрашивать обложки каждый кадр
static int s_last_visible_start = -1;
static int s_last_visible_end = -1;
static int s_last_tab = -1;  // для сброса при смене вкладки

void ui_screen_playlist_list_on_enter(AppState *state)
{
    /* Refresh stale data on entry so playlist revisions (the track-cache keys)
       are current — catches an edit made on another device. A load that just
       completed (the gated entry from the menu) is NOT redone. */
    library_playlists_refresh_on_enter(state);
}

/* Navigation-gate hook: kick the active tab's load and report whether the
   list is ready to be shown (the menu waits on this before pushing). */
int ui_screen_playlist_list_content_ready(AppState *state)
{
    PlaylistLoadStatus status;

    if (!state) {
        return -1;
    }
    status = library_playlists_service(state, state->playlist_tab);
    if (status == PLAYLIST_LOAD_READY) {
        return 1;
    }
    if (status == PLAYLIST_LOAD_ERROR) {
        return -1; /* enter anyway: the screen renders the error cause */
    }
    return 0;
}

void ui_screen_playlist_list_update(AppState *state)
{
    library_playlists_service(state, state->playlist_tab);

    // Если вкладка сменилась — сбросить видимый диапазон
    if (state->playlist_tab != s_last_tab) {
        s_last_visible_start = -1;
        s_last_visible_end = -1;
        s_last_tab = state->playlist_tab;
    }

    /* Transition gate: enter the track list only when the visible window at the
       resolved entry position is hydrated. All store/lock/anchor/hydration logic
       lives behind the library facade; here we do only view math + navigation. */
    {
        int enter_pos = 0;
        int count = 0;
        if (library_track_entry_ready(state, &enter_pos, &count)) {
            int scroll = enter_pos - TRACK_LIST_VISIBLE_ROWS / 2;
            int max_scroll = count - TRACK_LIST_VISIBLE_ROWS;
            if (max_scroll < 0) {
                max_scroll = 0;
            }
            if (scroll > max_scroll) {
                scroll = max_scroll;
            }
            if (scroll < 0) {
                scroll = 0;
            }
            state->track_ui.track_selected = enter_pos;
            state->track_ui.track_scroll = scroll;
            state->track_ui.track_screen_transition_done = 1;
            state->track_ui.track_bootstrap_indicator_visible = 0;
            ui_screen_track_list_clear_cache();
            app_state_push(state, SCREEN_TRACK_LIST);
        }
    }

    // Validate scroll position and update cover manager for the active tab
    PlaylistEntry *active_list = (state->playlist_tab == 0) ? state->playlists : state->liked_playlists;
    int active_count = (state->playlist_tab == 0) ? state->playlist_count : state->liked_playlist_count;
    int *active_scroll = (state->playlist_tab == 0) ? &state->playlist_scroll : &state->liked_playlist_scroll;

    if (active_count > 0) {
        const int max_visible = PLAYLIST_VISIBLE_SLOTS;
        if (*active_scroll < 0) {
            *active_scroll = 0;
        }
        if (*active_scroll > active_count - max_visible) {
            *active_scroll = (active_count > max_visible) ?
                             (active_count - max_visible) : 0;
        }

        // Update visible range for cover manager (updates ref_count)
        int start_idx = *active_scroll;
        int end_idx = start_idx + max_visible;
        if (end_idx > active_count) {
            end_idx = active_count;
        }

        // Update cover manager with visible range (manages ref_count internally)
        cover_manager_set_visible_range(start_idx, end_idx, active_list, active_count);

        // Запрашиваем обложки ТОЛЬКО при изменении видимого диапазона (не каждый кадр!)
        if (start_idx != s_last_visible_start || end_idx != s_last_visible_end) {
            s_last_visible_start = start_idx;
            s_last_visible_end = end_idx;

            // Request covers for visible playlists (highest priority)
            for (int i = start_idx; i < end_idx; i++) {
                const PlaylistEntry *pl = &active_list[i];
                if (pl->cover_uri[0]) {
                    cover_manager_request_cover(COVER_ENTITY_PLAYLIST, pl->playlist_id, pl->cover_uri, COVER_PRIORITY_VISIBLE, NULL);
                }
            }

            // Prefetch next 2 playlists (medium priority)
            for (int i = end_idx; i < end_idx + 2 && i < active_count; i++) {
                const PlaylistEntry *pl = &active_list[i];
                if (pl->cover_uri[0]) {
                    cover_manager_request_cover(COVER_ENTITY_PLAYLIST, pl->playlist_id, pl->cover_uri, COVER_PRIORITY_NEARBY, NULL);
                }
            }
        }
    }
}

void ui_screen_playlist_list_handle_input(AppState *state, const InputState *input)
{
    // L/R: переключение вкладок
    if (input->pressed & PSP_CTRL_LTRIGGER) {
        if (state->playlist_tab != 0) {
            state->playlist_tab = 0;
            s_last_visible_start = -1;
            s_last_visible_end = -1;
        }
    }
    if (input->pressed & PSP_CTRL_RTRIGGER) {
        if (state->playlist_tab != 1) {
            state->playlist_tab = 1;
            s_last_visible_start = -1;
            s_last_visible_end = -1;
        }
    }

    // Указатели на данные активной вкладки
    PlaylistEntry *active_list = (state->playlist_tab == 0) ? state->playlists : state->liked_playlists;
    int active_count = (state->playlist_tab == 0) ? state->playlist_count : state->liked_playlist_count;
    int *active_selected = (state->playlist_tab == 0) ? &state->playlist_selected : &state->liked_playlist_selected;
    int *active_scroll   = (state->playlist_tab == 0) ? &state->playlist_scroll   : &state->liked_playlist_scroll;

    if (input->pressed & PSP_CTRL_UP) {
        if (*active_selected > 0) {
            (*active_selected)--;
            if (*active_selected < *active_scroll) {
                *active_scroll = *active_selected;
            }
        }
    }
    if (input->pressed & PSP_CTRL_DOWN) {
        if (*active_selected < active_count - 1) {
            (*active_selected)++;
            const int max_visible = PLAYLIST_VISIBLE_SLOTS;
            if (*active_selected >= *active_scroll + max_visible) {
                *active_scroll = *active_selected - max_visible + 1;
            }
        }
    }
    if (input->pressed & PSP_CTRL_CROSS && active_count > 0) {
        library_open_playlist(state, &active_list[*active_selected],
                              state->playlist_tab, *active_selected);
    }
}

// Turn a load error_code into a human line: 429 and lost connection get a named
// reason; anything else (401/500/parse/internal) shows the raw code so the cause
// is never hidden behind a blank screen.
static void format_load_error(char *buf, size_t n, int code)
{
    if (code == 429) {
        snprintf(buf, n, "%s", locale_get(LOCALE_LOAD_ERR_RATELIMIT));
    } else if (code == NET_LOAD_ERR_TRANSPORT) {
        snprintf(buf, n, "%s", locale_get(LOCALE_LOAD_ERR_NOCONN));
    } else {
        snprintf(buf, n, "%s (%d)", locale_get(LOCALE_LOAD_ERR_GENERIC), code);
    }
}

void ui_screen_playlist_list_render(const AppState *state)
{
    ui_draw_clear(0xFF1A1A1A);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_PLAYLIST_LIST));

    // Tab labels (между заголовком y=32 и списком y=48)
    const float tab_y   = 40.0f;
    const float tab0_x  = 16.0f;
    const float tab1_x  = 80.0f;
    ui_draw_text(tab0_x, tab_y, locale_get(LOCALE_TAB_MY_PLAYLISTS),
                 state->playlist_tab == 0 ? 0xFFFFFFFF : 0xFF888888);
    ui_draw_text(tab1_x, tab_y, locale_get(LOCALE_TAB_LIKED_PLAYLISTS),
                 state->playlist_tab == 1 ? 0xFFFFFFFF : 0xFF888888);
    // Подчёркивание активной вкладки
    float active_tab_x = (state->playlist_tab == 0) ? tab0_x : tab1_x;
    ui_draw_rect(active_tab_x, tab_y + 14.0f, 50.0f, 2.0f, 0xFF00D5FF);

    // Данные активной вкладки
    const PlaylistEntry *active_list = (state->playlist_tab == 0) ? state->playlists : state->liked_playlists;
    int active_count    = (state->playlist_tab == 0) ? state->playlist_count : state->liked_playlist_count;
    int active_selected = (state->playlist_tab == 0) ? state->playlist_selected : state->liked_playlist_selected;
    int active_scroll   = (state->playlist_tab == 0) ? state->playlist_scroll   : state->liked_playlist_scroll;
    PlaylistLoadStatus active_status = (state->playlist_tab == 0)
                                           ? state->playlists_load.status
                                           : state->liked_playlists_load.status;
    int active_error_code = (state->playlist_tab == 0)
                                ? state->playlists_load.error_code
                                : state->liked_playlists_load.error_code;
    int active_loading = (active_status == PLAYLIST_LOAD_LOADING);

    /* A background refresh keeps the existing rows on screen; the bare "..."
       shows only when the tab has no data yet (first L/R switch to it). */
    if (active_loading && active_count == 0) {
        ui_draw_text(16.0f, 58.0f, "...", 0xFF777777);
    } else if (active_status == PLAYLIST_LOAD_ERROR) {
        char msg[128];
        format_load_error(msg, sizeof(msg), active_error_code);
        ui_draw_text(16.0f, 58.0f, msg, 0xFF4444FFu);
    } else if (active_count == 0) {
        ui_draw_text(16.0f, 58.0f, locale_get(LOCALE_SCREEN_EMPTY), 0xFFBBBBBB);
    } else {
        // Draw playlist list with thumbnails
        const float item_height = 40.0f;
        const float thumb_size = 30.0f;
        const float header_x = 16.0f;
        const float selection_width = 6.0f;
        const float gap_after_selection = 5.0f;
        const float thumb_x = header_x + selection_width + gap_after_selection;
        const float gap_after_cover = 5.0f;
        const float text_x = thumb_x + thumb_size + gap_after_cover;
        /* 58 + 5 rows x 40 = 258 — список заканчивается ровно у панели подсказок */
        const float start_y = 58.0f;
        const float text_max_w = 480.0f - text_x - 8.0f;
        const int max_visible = PLAYLIST_VISIBLE_SLOTS;

        int start_idx = active_scroll;
        int end_idx = start_idx + max_visible;
        if (end_idx > active_count) {
            end_idx = active_count;
        }

        for (int i = start_idx; i < end_idx; i++) {
            float y = start_y + (float)(i - start_idx) * item_height;
            const PlaylistEntry *pl = &active_list[i];

            // Draw selection indicator
            if (i == active_selected) {
                ui_draw_rect(header_x, y - 3.0f, selection_width, item_height - 4.0f, 0xFF00D5FF);
            }

            if (!cover_manager_draw_cover(COVER_ENTITY_PLAYLIST, pl->playlist_id, NULL,
                                          (int)thumb_x, (int)y, (int)thumb_size, (int)thumb_size)) {
                if (cover_manager_is_loading(COVER_ENTITY_PLAYLIST, pl->playlist_id, NULL)) {
                    ui_draw_rect(thumb_x, y, thumb_size, thumb_size, 0xFF444444);
                }
            }

            // Draw title
            text_render_clipped(text_x, y, pl->title, i == active_selected ? 0xFFFFFFFF : 0xFFBBBBBB, text_max_w);

            // Draw info (track count, etc.)
            char info[128];
            if (pl->track_count >= 0) {
                snprintf(info, sizeof(info), "%d %s", pl->track_count, locale_get(LOCALE_PLAYLIST_TRACKS));
            } else {
                snprintf(info, sizeof(info), "-");
            }
            if (state->track_ui.track_bootstrap_indicator_visible &&
                i == state->track_ui.pending_playlist_selected_index) {
                strncat(info, "  ...", sizeof(info) - strlen(info) - 1);
            }
            ui_draw_text(text_x, y + 16.0f, info, 0xFF888888);
        }
    }

    ui_common_draw_prompts(LOCALE_PLAYLIST_OPEN_PROMPT, LOCALE_PLAYLIST_TAB_PROMPT, LOCALE_PLAYLIST_BACK_PROMPT);
}
