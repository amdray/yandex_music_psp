#include "ui/ui_screen_track_list.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "services/locale.h"
#include "services/cover_manager.h"
#include "services/net_client.h"
#include "services/token_loader.h"
#include "services/playback_controller.h"
#include "services/playback_queue.h"
#include "core/logger.h"
#include "app/app_state.h"
#include "fonts/text.h"
#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static void format_duration(int duration_ms, char *out, size_t out_size)
{
    if (duration_ms < 0) {
        snprintf(out, out_size, "-");
        return;
    }
    int total_seconds = duration_ms / 1000;
    int minutes = total_seconds / 60;
    int seconds = total_seconds % 60;
    snprintf(out, out_size, "%d:%02d", minutes, seconds);
}

// Отслеживаем последний видимый диапазон, чтобы не запрашивать обложки каждый кадр
static int s_last_visible_start = -1;
static int s_last_visible_end = -1;

// Сброс кэша видимого диапазона при переходе на экран
void ui_screen_track_list_clear_cache(void)
{
    s_last_visible_start = -1;
    s_last_visible_end = -1;
}

/* Leaving the screen: remember where the cursor stood as an identity anchor
   (track id + ordinal hint), so re-entering this playlist relocates the same
   track even if an external edit shifted its position. */
void ui_screen_track_list_on_exit(AppState *state)
{
    if (!state) {
        return;
    }
    net_client_track_store_lock();
    {
        int sel = state->track_ui.track_selected;
        if (state->track_boot.target_uuid[0] &&
            sel >= 0 && sel < state->track_store.count) {
            app_state_anchor_save(state, state->track_boot.target_uuid,
                                  state->track_store.ids[sel], sel);
        }
    }
    net_client_track_store_unlock();
}

void ui_screen_track_list_update(AppState *state)
{
    static char s_token[256];
    static int s_token_generation = -1;
    static int s_last_hydrated = -1;

    if (!state) {
        return;
    }

    int request_count = 0;
    TrackEntry request_tracks[7];
    int generation;
    int focus;

    net_client_track_store_lock();
    int visible_count = state->track_store.count;
    generation = state->track_boot.generation;
    focus = state->track_ui.track_selected;
    if (visible_count > 0) {
        const int max_visible = TRACK_LIST_VISIBLE_ROWS;
        if (state->track_ui.track_scroll < 0) {
            state->track_ui.track_scroll = 0;
        }
        if (state->track_ui.track_scroll > visible_count - max_visible) {
            state->track_ui.track_scroll = (visible_count > max_visible) ?
                                           (visible_count - max_visible) : 0;
        }

        int start_idx = state->track_ui.track_scroll;
        int end_idx = start_idx + max_visible;
        if (end_idx > visible_count) {
            end_idx = visible_count;
        }

        /* Re-collect cover requests when the range moves OR when more rows in
           the range hydrate (their cover uris only exist after hydration). */
        int hydrated = 0;
        for (int i = start_idx; i < end_idx + 2 && i < visible_count; ++i) {
            int slot = i - state->track_store.window_start;
            if (slot >= 0 && slot < TRACK_WINDOW_CAPACITY &&
                state->track_store.window_valid[slot]) {
                hydrated++;
            }
        }

        if (start_idx != s_last_visible_start || end_idx != s_last_visible_end ||
            hydrated != s_last_hydrated) {
            s_last_visible_start = start_idx;
            s_last_visible_end = end_idx;
            s_last_hydrated = hydrated;

            for (int i = start_idx;
                 i < end_idx + 2 && i < visible_count && request_count < 7; ++i) {
                int slot = i - state->track_store.window_start;
                if (slot >= 0 && slot < TRACK_WINDOW_CAPACITY &&
                    state->track_store.window_valid[slot]) {
                    request_tracks[request_count++] = state->track_store.window[slot];
                }
            }
        }
    }
    net_client_track_store_unlock();

    /* Keep the hydration window centered on the cursor. */
    if (s_token_generation != generation) {
        s_token[0] = '\0';
        if (token_loader_read(s_token, sizeof(s_token)) == 0) {
            s_token_generation = generation;
        }
    }
    net_client_track_window_focus(state, s_token, focus);

    for (int i = 0; i < request_count; ++i) {
        const TrackEntry *track = &request_tracks[i];
        if (track->cover_uri[0] && track->album_id != 0) {
            CoverPriority priority = (i < 5) ? COVER_PRIORITY_VISIBLE : COVER_PRIORITY_NEARBY;
            cover_manager_request_cover(COVER_ENTITY_ALBUM, track->album_id, track->cover_uri, priority, NULL);
        }
    }
}

void ui_screen_track_list_handle_input(AppState *state, const InputState *input)
{
    int start_playback = 0;
    TrackEntry selected_track;
    memset(&selected_track, 0, sizeof(selected_track));

    net_client_track_store_lock();
    int visible_count = state->track_store.count;

    if (input->pressed & PSP_CTRL_UP) {
        if (state->track_ui.track_selected > 0) {
            state->track_ui.track_selected--;
            if (state->track_ui.track_selected < state->track_ui.track_scroll) {
                state->track_ui.track_scroll = state->track_ui.track_selected;
            }
        }
    }
    if (input->pressed & PSP_CTRL_DOWN) {
        if (state->track_ui.track_selected < visible_count - 1) {
            state->track_ui.track_selected++;
            const int max_visible = TRACK_LIST_VISIBLE_ROWS;
            // Scroll down if selected item goes beyond visible area
            if (state->track_ui.track_selected >= state->track_ui.track_scroll + max_visible) {
                state->track_ui.track_scroll = state->track_ui.track_selected - max_visible + 1;
            }
        }
    }
    if (input->pressed & PSP_CTRL_CROSS && visible_count > 0) {
        /* PLAY_TRACK: tell playback_controller which track to download/play,
         * then open the NOW_PLAYING screen which reads g_playback state.
         * Only a hydrated row can start playback — a "..." row has no
         * metadata yet; the press is ignored until hydration lands. */
        int selected = state->track_ui.track_selected;
        int slot = selected - state->track_store.window_start;
        if (selected >= 0 && selected < visible_count &&
            slot >= 0 && slot < TRACK_WINDOW_CAPACITY &&
            state->track_store.window_valid[slot]) {
            const PlaylistEntry *active_list = (state->playlist_tab == 0) ? state->playlists : state->liked_playlists;
            int active_count = (state->playlist_tab == 0) ? state->playlist_count : state->liked_playlist_count;
            int active_selected = (state->playlist_tab == 0) ? state->playlist_selected : state->liked_playlist_selected;
            int source_id = 0;

            if (active_selected >= 0 && active_selected < active_count) {
                source_id = active_list[active_selected].playlist_id;
            }

            if (playback_queue_set_from_ids(state->track_store.ids,
                                            visible_count,
                                            selected,
                                            PLAYBACK_QUEUE_SOURCE_PLAYLIST,
                                            source_id,
                                            state->track_boot.generation) != 0) {
                logLine("ui: queue set failed selected=%d count=%d\n",
                        selected, visible_count);
            } else {
                logLine("ui: play selected=%d count=%d source_id=%d generation=%d track_id='%s'\n",
                        selected,
                        visible_count,
                        source_id,
                        state->track_boot.generation,
                        state->track_store.window[slot].id);
                memcpy(&selected_track, &state->track_store.window[slot], sizeof(TrackEntry));
                start_playback = 1;
            }
        }
    }
    net_client_track_store_unlock();

    if (start_playback) {
        memcpy(&state->now_playing_track, &selected_track, sizeof(TrackEntry));
        app_state_push(state, SCREEN_NOW_PLAYING);
        playback_controller_request_play_current();
    }
}

void ui_screen_track_list_render(const AppState *state)
{
    ui_draw_clear(0xFF1A1A1A);
    
    // Формируем заголовок с названием плейлиста через локализацию
    char header_text[256];
    {
        const PlaylistEntry *active_list = (state->playlist_tab == 0) ? state->playlists : state->liked_playlists;
        int active_count    = (state->playlist_tab == 0) ? state->playlist_count : state->liked_playlist_count;
        int active_selected = (state->playlist_tab == 0) ? state->playlist_selected : state->liked_playlist_selected;
        if (active_selected >= 0 && active_selected < active_count && active_list[active_selected].title[0]) {
            snprintf(header_text, sizeof(header_text), locale_get(LOCALE_SCREEN_TRACK_LIST_OF_PLAYLIST), active_list[active_selected].title);
        } else {
            strncpy(header_text, locale_get(LOCALE_SCREEN_TRACK_LIST), sizeof(header_text) - 1);
            header_text[sizeof(header_text) - 1] = '\0';
        }
    }
    ui_common_draw_header(header_text);
    
    TrackEntry visible_tracks[TRACK_LIST_VISIBLE_ROWS];
    unsigned char visible_valid[TRACK_LIST_VISIBLE_ROWS];
    int visible_tracks_count = 0;
    int selected_index = 0;
    int visible_count = 0;

    net_client_track_store_lock();
    visible_count = state->track_store.count;
    selected_index = state->track_ui.track_selected;
    int start_idx = state->track_ui.track_scroll;
    if (start_idx < 0) {
        start_idx = 0;
    }
    if (start_idx > visible_count) {
        start_idx = visible_count;
    }

    int end_idx = start_idx + TRACK_LIST_VISIBLE_ROWS;
    if (end_idx > visible_count) {
        end_idx = visible_count;
    }
    for (int i = start_idx; i < end_idx; ++i) {
        int slot = i - state->track_store.window_start;
        if (slot >= 0 && slot < TRACK_WINDOW_CAPACITY &&
            state->track_store.window_valid[slot]) {
            visible_tracks[visible_tracks_count] = state->track_store.window[slot];
            visible_valid[visible_tracks_count] = 1;
        } else {
            /* Row not hydrated yet (scrolled outside the window): placeholder
               until the hydrator catches up — background load, not a screen
               transition. */
            memset(&visible_tracks[visible_tracks_count], 0, sizeof(TrackEntry));
            visible_valid[visible_tracks_count] = 0;
        }
        visible_tracks_count++;
    }
    net_client_track_store_unlock();

    /* The transition gate guarantees visible rows exist on entry, so the only
       zero-row state left is a genuinely empty playlist. */
    if (visible_count == 0) {
        ui_draw_text(16.0f, 48.0f, locale_get(LOCALE_SCREEN_EMPTY), 0xFFBBBBBB);
    } else {
        // Draw track list with thumbnails
        // Layout: header line (X=16) -> 6px selection indicator -> 5px gap -> cover (30x30) -> 5px gap -> text
        const float item_height = 40.0f;
        const float thumb_size = 30.0f;  // Match download size 30x30 (stride alignment handled in image_loader)
        const float header_x = 16.0f;  // X position of header text and line
        const float selection_width = 6.0f;  // Width of selection indicator
        const float gap_after_selection = 5.0f;  // Gap after selection indicator
        const float thumb_x = header_x + selection_width + gap_after_selection;  // Cover starts after header line + selection + gap
        const float gap_after_cover = 5.0f;  // Gap after cover
        const float text_x = thumb_x + thumb_size + gap_after_cover;  // Text starts 5px after cover
        const float start_y = 48.0f;
        const float text_max_w = 480.0f - text_x - 8.0f;
        
        for (int row = 0; row < visible_tracks_count; row++) {
            int i = start_idx + row;
            float y = start_y + (float)row * item_height;
            const TrackEntry *track = &visible_tracks[row];
            
            // Draw selection indicator (aligned with header line, centered vertically relative to cover)
            if (i == selected_index) {
                ui_draw_rect(header_x, y - 3.0f, selection_width, item_height - 4.0f, 0xFF00D5FF);
            }

            if (!visible_valid[row]) {
                ui_draw_text(text_x, y, "...", 0xFF777777);
                continue;
            }

            if (!cover_manager_draw_cover(COVER_ENTITY_ALBUM, track->album_id, NULL,
                                          (int)thumb_x, (int)y, (int)thumb_size, (int)thumb_size)) {
                if (cover_manager_is_loading(COVER_ENTITY_ALBUM, track->album_id, NULL)) {
                    ui_draw_rect(thumb_x, y, thumb_size, thumb_size, 0xFF444444);
                }
            }
            
            // First line: artist - title, with the edition/version de-emphasized.
            u32 line1_color = i == selected_index ? 0xFFFFFFFF : 0xFFBBBBBB;
            u32 version_color = i == selected_index ? 0xFFBBBBBB : 0xFF777777;
            char line1[512];  // 96 + 160 + 64 + separators
            if (track->artist[0]) {
                snprintf(line1, sizeof(line1), "%s - %s", track->artist, track->title);
            } else {
                strncpy(line1, track->title, sizeof(line1) - 1);
                line1[sizeof(line1) - 1] = '\0';
            }
            // logLine("ui: render track %d: artist='%s' title='%s' line1='%s'\n", i, track->artist, track->title, line1);
            text_render_clipped(text_x, y, line1, line1_color, text_max_w);
            if (track->version[0]) {
                float line1_w = text_measure_width(line1);
                float ver_x = text_x + line1_w + text_measure_width(" ");
                float ver_w = text_max_w - (line1_w + text_measure_width(" "));
                if (ver_w > 0.0f) {
                    text_render_clipped(ver_x, y, track->version, version_color, ver_w);
                }
            }
            
            // Second line: year %space% duration %space% genre %space% [E] explicit
            char line2[256];
            int parts = 0;
            
            // Year
            if (track->year > 0) {
                snprintf(line2, sizeof(line2), "%d", track->year);
                parts++;
            } else {
                line2[0] = '\0';
            }
            
            // Duration
            char duration_str[16];
            format_duration(track->duration_ms, duration_str, sizeof(duration_str));
            if (parts > 0) {
                strncat(line2, " ", sizeof(line2) - strlen(line2) - 1);
            }
            strncat(line2, duration_str, sizeof(line2) - strlen(line2) - 1);
            parts++;
            
            // Genre
            if (track->genre[0]) {
                if (parts > 0) {
                    strncat(line2, " ", sizeof(line2) - strlen(line2) - 1);
                }
                strncat(line2, track->genre, sizeof(line2) - strlen(line2) - 1);
                parts++;
            }
            
            // Explicit
            if (track->explicit_content) {
                if (parts > 0) {
                    strncat(line2, " ", sizeof(line2) - strlen(line2) - 1);
                }
                strncat(line2, locale_get(LOCALE_TRACK_EXPLICIT), sizeof(line2) - strlen(line2) - 1);
            }
            
            ui_draw_text(text_x, y + 16.0f, line2, 0xFF888888);
        }
    }
    
    ui_common_draw_prompts(LOCALE_TRACK_PLAY_PROMPT, LOCALE_TRACK_BACK_PROMPT);
}
