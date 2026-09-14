#include "ui/ui_screen_artist_menu.h"

#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

#include "app/artist.h"
#include "core/logger.h"
#include "fonts/text.h"
#include "services/cover_manager.h"
#include "services/locale.h"
#include "services/net_client.h"
#include "services/token_loader.h"
#include "ui/ui_common.h"
#include "ui/ui_draw.h"

#define ARTIST_MENU_VISIBLE 6

static int s_tab = 0;      /* 0=albums, 1=also_albums */
static int s_selected = 0;
static int s_scroll = 0;
static int s_generation = 0;

static void poll_artist_brief_result(AppState *state)
{
    ArtistBriefInfo brief;
    int generation = 0;
    int rc = -1;

    memset(&brief, 0, sizeof(brief));
    if (!net_client_artist_brief_load_poll(&generation, &rc, &brief)) {
        return;
    }

    if (generation != s_generation) {
        logLine("artist_menu: stale result ignored generation=%d current=%d\n",
                generation, s_generation);
        return;
    }

    if (rc == 0) {
        memcpy(&state->artist_brief, &brief, sizeof(state->artist_brief));
        state->artist_brief_loading = 2;
        logLine("artist_menu: ok albums=%d also=%d\n",
                state->artist_brief.album_count,
                state->artist_brief.also_album_count);
    } else {
        state->artist_brief_loading = -1;
        logLine("artist_menu: brief-info fetch failed rc=%d\n", rc);
    }
}

static void start_artist_brief_fetch(AppState *state)
{
    char token[256];

    if (state->artist_brief_loading != 0) {
        return;
    }
    if (state->artist_selected < 0 || state->artist_selected >= state->liked_artist_count) {
        state->artist_brief_loading = -1;
        return;
    }

    int artist_id = state->liked_artists[state->artist_selected].artist_id;
    const char *cov_uri = state->liked_artists[state->artist_selected].cover_uri;
    if (artist_id <= 0 || token_loader_read(token, sizeof(token)) != 0) {
        state->artist_brief_loading = -1;
        return;
    }

    int rc = net_client_artist_brief_load_start(token, artist_id, s_generation);
    memset(token, 0, sizeof(token));
    if (rc == 0) {
        memset(&state->artist_brief, 0, sizeof(state->artist_brief));
        state->artist_brief_loading = 1;
        if (cov_uri[0]) {
            cover_manager_request_cover(COVER_ENTITY_ARTIST,
                                        artist_id,
                                        cov_uri,
                                        COVER_PRIORITY_VISIBLE,
                                        "200x200");
        }
    } else if (rc < 0) {
        state->artist_brief_loading = -1;
        logLine("artist_menu: brief load start failed rc=%d\n", rc);
    }
}

void ui_screen_artist_menu_reset(void)
{
    s_generation++;
    s_tab = 0;
    s_selected = 0;
    s_scroll = 0;
}

void ui_screen_artist_menu_update(AppState *state)
{
    poll_artist_brief_result(state);
    start_artist_brief_fetch(state);
}

/* Navigation-gate hook: kick the brief-info load and report readiness
   (the artist list waits on this before pushing). */
int ui_screen_artist_menu_content_ready(AppState *state)
{
    poll_artist_brief_result(state);
    start_artist_brief_fetch(state);
    if (state->artist_brief_loading == 2) {
        return 1;
    }
    if (state->artist_brief_loading == -1) {
        return -1;
    }
    return 0;
}

void ui_screen_artist_menu_handle_input(AppState *state, const InputState *input)
{
    const ArtistBriefInfo *brief = &state->artist_brief;
    int count = (s_tab == 0) ? brief->album_count : brief->also_album_count;

    if (input->pressed & PSP_CTRL_LTRIGGER) {
        if (s_tab != 0) {
            s_tab = 0;
            s_selected = 0;
            s_scroll = 0;
        }
        return;
    }
    if (input->pressed & PSP_CTRL_RTRIGGER) {
        if (s_tab != 1) {
            s_tab = 1;
            s_selected = 0;
            s_scroll = 0;
        }
        return;
    }

    if (input->pressed & PSP_CTRL_UP) {
        if (s_selected > 0) {
            s_selected--;
            if (s_selected < s_scroll) {
                s_scroll = s_selected;
            }
        }
    }
    if (input->pressed & PSP_CTRL_DOWN) {
        if (count > 0 && s_selected < count - 1) {
            s_selected++;
            if (s_selected >= s_scroll + ARTIST_MENU_VISIBLE) {
                s_scroll = s_selected - ARTIST_MENU_VISIBLE + 1;
            }
        }
    }
}

void ui_screen_artist_menu_render(const AppState *state)
{
    ui_draw_clear(0xFF1A1A1A);

    const ArtistBriefInfo *brief = &state->artist_brief;
    const ArtistEntry *artist = &state->liked_artists[state->artist_selected];

    const float cov_x = 4.0f;
    const float cov_y = 4.0f;
    const float cov_sz = 200.0f;

    int artist_id = artist->artist_id;
    if (!cover_manager_draw_cover(COVER_ENTITY_ARTIST, artist_id, "200x200",
                                  (int)cov_x, (int)cov_y, (int)cov_sz, (int)cov_sz)) {
        ui_draw_rect(cov_x, cov_y, cov_sz, cov_sz, 0xFF333333);
    }

    if (state->artist_brief_loading == 2) {
        char info[48];
        snprintf(info, sizeof(info),
                 "\xd0\xa2\xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xb2: %d",
                 brief->count_tracks);
        ui_draw_text(cov_x, cov_y + cov_sz + 4.0f, info, 0xFFBBBBBB);
        snprintf(info, sizeof(info),
                 "\xd0\x90\xd0\xbb\xd1\x8c\xd0\xb1\xd0\xbe\xd0\xbc\xd0\xbe\xd0\xb2: %d",
                 brief->count_direct_albums);
        ui_draw_text(cov_x, cov_y + cov_sz + 18.0f, info, 0xFFBBBBBB);
    }

    const float col_x = 212.0f;
    const float col_w = 264.0f;
    const float tab_label_y = 22.0f;
    const float list_y_start = 44.0f;
    const float item_h = 34.0f;
    const float sel_w = 4.0f;

    ui_draw_text(col_x, 4.0f, artist->name, 0xFFFFFFFF);

    ui_draw_text(col_x, tab_label_y,
                 "\xd0\x90\xd0\xbb\xd1\x8c\xd0\xb1\xd0\xbe\xd0\xbc\xd1\x8b",
                 s_tab == 0 ? 0xFFFFFFFF : 0xFF888888);
    ui_draw_text(col_x + 78.0f, tab_label_y,
                 "\xd0\x94\xd1\x80\xd1\x83\xd0\xb3\xd0\xb8\xd0\xb5",
                 s_tab == 1 ? 0xFFFFFFFF : 0xFF888888);

    float active_tab_x = (s_tab == 0) ? col_x : col_x + 78.0f;
    ui_draw_rect(active_tab_x, tab_label_y + 13.0f, 48.0f, 2.0f, 0xFF00D5FF);
    ui_draw_rect(col_x, list_y_start - 2.0f, col_w, 1.0f, 0xFF333333);

    /* The navigation gate guarantees the brief is loaded (or failed) before
       entry — no in-between loading state can be visible here. */
    if (state->artist_brief_loading == -1) {
        ui_draw_text(col_x, list_y_start,
                     locale_get(LOCALE_SCREEN_EMPTY), 0xFFBBBBBB);
    } else {
        const ArtistAlbumEntry *list = (s_tab == 0) ? brief->albums : brief->also_albums;
        int count = (s_tab == 0) ? brief->album_count : brief->also_album_count;

        if (count == 0) {
            ui_draw_text(col_x, list_y_start,
                         locale_get(LOCALE_SCREEN_EMPTY), 0xFFBBBBBB);
        } else {
            int start_idx = s_scroll;
            int end_idx = start_idx + ARTIST_MENU_VISIBLE;
            if (end_idx > count) {
                end_idx = count;
            }
            for (int i = start_idx; i < end_idx; i++) {
                float y = list_y_start + (float)(i - start_idx) * item_h;
                const ArtistAlbumEntry *alb = &list[i];

                if (i == s_selected) {
                    ui_draw_rect(col_x, y - 1.0f, sel_w, item_h - 2.0f, 0xFF00D5FF);
                }

                {
                    float title_x = col_x + sel_w + 3.0f;
                    u32 title_color = i == s_selected ? 0xFFFFFFFF : 0xFFBBBBBB;
                    u32 version_color = i == s_selected ? 0xFFBBBBBB : 0xFF777777;
                    ui_draw_text(title_x, y, alb->title, title_color);
                    if (alb->version[0]) {
                        ui_draw_text(title_x + text_measure_width(alb->title) +
                                     text_measure_width(" "),
                                     y, alb->version, version_color);
                    }
                }

                if (alb->year > 0) {
                    char sub[16];
                    snprintf(sub, sizeof(sub), "%d", alb->year);
                    ui_draw_text(col_x + sel_w + 3.0f, y + 16.0f, sub, 0xFF888888);
                }
            }
        }
    }

    ui_common_draw_prompts(LOCALE_PLAYLIST_BACK_PROMPT, LOCALE_PLAYLIST_TAB_PROMPT);
}
