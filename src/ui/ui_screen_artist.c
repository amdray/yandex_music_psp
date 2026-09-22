#include "ui/ui_screen_artist.h"

#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

#include "app/artist.h"
#include "core/logger.h"
#include "services/cover_manager.h"
#include "services/locale.h"
#include "services/net_client.h"
#include "services/token_loader.h"
#include "fonts/text.h"
#include "ui/ui_common.h"
#include "ui/ui_draw.h"
#include "ui/ui_screen_artist_menu.h"
#include "ui/ui_screens.h"

static int s_fetch_done = 0;
static int s_loading = 0;
static int s_error = 0;
static int s_generation = 0;

static int s_last_visible_start = -1;
static int s_last_visible_end = -1;

static void poll_artist_result(AppState *state)
{
    ArtistEntry artists[MAX_LIKED_ARTISTS];
    int generation = 0;
    int rc = -1;
    int count = 0;

    if (!net_client_liked_artists_load_poll(&generation, &rc,
                                            artists, MAX_LIKED_ARTISTS, &count)) {
        return;
    }

    if (generation != s_generation) {
        logLine("ui: liked artists stale result ignored generation=%d current=%d\n",
                generation, s_generation);
        s_loading = 0;
        return;
    }

    if (rc == 0) {
        memcpy(state->liked_artists, artists, (size_t)count * sizeof(ArtistEntry));
        state->liked_artist_count = count;
        state->artist_selected = 0;
        state->artist_scroll = 0;
        s_error = 0;
        logLine("ui: loaded %d liked artists\n", count);
    } else {
        s_error = 1;
        logLine("ui: liked_artists fetch/parse failed rc=%d\n", rc);
    }

    s_loading = 0;
    s_fetch_done = 1;
}

static void start_artist_fetch(AppState *state)
{
    char token[256];

    if (s_fetch_done || s_loading || state->currentUser.uid <= 0) {
        return;
    }

    if (token_loader_read(token, sizeof(token)) != 0) {
        s_error = 1;
        s_fetch_done = 1;
        return;
    }

    int rc = net_client_liked_artists_load_start(token, state->currentUser.uid, s_generation);
    memset(token, 0, sizeof(token));
    if (rc == 0) {
        s_loading = 1;
        s_error = 0;
        logLine("ui: loading liked artists async uid=%d generation=%d\n",
                state->currentUser.uid, s_generation);
    } else if (rc < 0) {
        s_error = 1;
        s_fetch_done = 1;
        logLine("ui: liked artists load start failed rc=%d\n", rc);
    }
}

void ui_screen_artist_update(AppState *state)
{
    poll_artist_result(state);
    start_artist_fetch(state);
}

/* Navigation-gate hook: kick the liked-artists load and report readiness
   (the menu waits on this before pushing). */
int ui_screen_artist_content_ready(AppState *state)
{
    poll_artist_result(state);
    start_artist_fetch(state);
    if (!s_fetch_done) {
        return 0;
    }
    return s_error ? -1 : 1;
}

void ui_screen_artist_reset(void)
{
    s_generation++;
    s_fetch_done = 0;
    s_loading = 0;
    s_error = 0;
    s_last_visible_start = -1;
    s_last_visible_end = -1;
}

void ui_screen_artist_handle_input(AppState *state, const InputState *input)
{
    int count = state->liked_artist_count;
    if (count <= 0) {
        return;
    }

    if (input->pressed & PSP_CTRL_UP) {
        if (state->artist_selected > 0) {
            state->artist_selected--;
            if (state->artist_selected < state->artist_scroll) {
                state->artist_scroll = state->artist_selected;
            }
        }
    }
    if (input->pressed & PSP_CTRL_DOWN) {
        if (state->artist_selected < count - 1) {
            state->artist_selected++;
            const int max_visible = ARTIST_VISIBLE_SLOTS;
            if (state->artist_selected >= state->artist_scroll + max_visible) {
                state->artist_scroll = state->artist_selected - max_visible + 1;
            }
        }
    }
    if (input->pressed & PSP_CTRL_CROSS) {
        memset(&state->artist_brief, 0, sizeof(state->artist_brief));
        state->artist_brief_loading = 0;
        ui_screen_artist_menu_reset();
        /* Gated: wait here until the artist card's brief info is loaded. */
        ui_screens_navigate(state, SCREEN_ARTIST_MENU);
    }
}

void ui_screen_artist_render(const AppState *state)
{
    ui_draw_clear(0xFF1A1A1A);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_ARTIST));

    int count = state->liked_artist_count;

    /* The navigation gate guarantees the list is loaded before entry, so the
       only zero-row states left are a load error and a genuinely empty list. */
    if (s_error || count == 0) {
        ui_draw_text(16.0f, 48.0f, locale_get(LOCALE_SCREEN_EMPTY), 0xFFBBBBBB);
        return;
    }

    {
        int start_idx = state->artist_scroll;
        int end_idx = start_idx + ARTIST_VISIBLE_SLOTS;
        if (end_idx > count) {
            end_idx = count;
        }

        if (start_idx != s_last_visible_start || end_idx != s_last_visible_end) {
            s_last_visible_start = start_idx;
            s_last_visible_end = end_idx;

            for (int i = start_idx; i < end_idx; i++) {
                const ArtistEntry *ar = &state->liked_artists[i];
                if (ar->cover_uri[0] && ar->artist_id != 0) {
                    cover_manager_request_cover(COVER_ENTITY_ARTIST, ar->artist_id,
                                                ar->cover_uri, COVER_PRIORITY_VISIBLE, "30x30");
                }
            }
            for (int i = end_idx; i < end_idx + 2 && i < count; i++) {
                const ArtistEntry *ar = &state->liked_artists[i];
                if (ar->cover_uri[0] && ar->artist_id != 0) {
                    cover_manager_request_cover(COVER_ENTITY_ARTIST, ar->artist_id,
                                                ar->cover_uri, COVER_PRIORITY_NEARBY, "30x30");
                }
            }
        }
    }

    const float item_height = 40.0f;
    const float thumb_size = 30.0f;
    const float header_x = 16.0f;
    const float selection_width = 6.0f;
    const float gap_after_sel = 5.0f;
    const float thumb_x = header_x + selection_width + gap_after_sel;
    const float gap_after_cover = 5.0f;
    const float text_x = thumb_x + thumb_size + gap_after_cover;
    const float start_y = 48.0f;
    const float text_max_w = 480.0f - text_x - 8.0f;

    int start_idx = state->artist_scroll;
    int end_idx = start_idx + ARTIST_VISIBLE_SLOTS;
    if (end_idx > count) {
        end_idx = count;
    }

    for (int i = start_idx; i < end_idx; i++) {
        float y = start_y + (float)(i - start_idx) * item_height;
        const ArtistEntry *ar = &state->liked_artists[i];

        if (i == state->artist_selected) {
            ui_draw_rect(header_x, y - 3.0f, selection_width, item_height - 4.0f, 0xFF00D5FF);
        }

        if (!cover_manager_draw_cover(COVER_ENTITY_ARTIST, ar->artist_id, NULL,
                                      (int)thumb_x, (int)y, (int)thumb_size, (int)thumb_size)) {
            if (cover_manager_is_loading(COVER_ENTITY_ARTIST, ar->artist_id, NULL)) {
                ui_draw_rect(thumb_x, y, thumb_size, thumb_size, 0xFF444444);
            }
        }

        text_render_clipped(text_x, y, ar->name,
                            i == state->artist_selected ? 0xFFFFFFFF : 0xFFBBBBBB,
                            text_max_w);

        char info[64];
        if (ar->genre[0]) {
            snprintf(info, sizeof(info), "%d \xd0\xb0\xd0\xbb\xd1\x8c\xd0\xb1"
                     " \xc2\xb7 %d \xd1\x82\xd1\x80\xd0\xb5\xd0\xba"
                     " \xc2\xb7 %s",
                     ar->album_count, ar->track_count, ar->genre);
        } else {
            snprintf(info, sizeof(info), "%d \xd0\xb0\xd0\xbb\xd1\x8c\xd0\xb1"
                     " \xc2\xb7 %d \xd1\x82\xd1\x80\xd0\xb5\xd0\xba",
                     ar->album_count, ar->track_count);
        }
        ui_draw_text(text_x, y + 16.0f, info, 0xFF888888);
    }

}
