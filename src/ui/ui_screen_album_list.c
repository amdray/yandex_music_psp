#include "ui/ui_screen_album_list.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/artist.h"  // ArtistAlbumEntry как строка альбома
#include "core/logger.h"
#include "services/album_play.h"
#include "services/cover_manager.h"
#include "services/locale.h"
#include "services/playback_controller.h"
#include "services/playback_queue.h"
#include "services/token_loader.h"
#include "services/ym_api_albums.h"
#include "fonts/text.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_screens.h"

#define ALBUM_LIST_VISIBLE 6
#define ALBUM_LIST_MAX 256

// Состояние списка (один fetch на uid, как везде).
static ArtistAlbumEntry *s_albums = NULL;
static int s_count = 0;
static int s_selected = 0;
static int s_scroll = 0;
static int s_uid = 0;
static int s_loading = 0;   // воркер списка летит
static int s_error = 0;
static SceUID s_tid = -1;
static char s_token[256];
static int s_status_open = 0;  // ждём открытие альбома (album_play)

static int album_list_worker(SceSize args, void *argp)
{
    YmApiContext ctx;
    ArtistAlbumEntry *list = NULL;
    int count = 0;

    (void)args;
    (void)argp;
    ctx.oauth_token = s_token;
    ctx.timeout_ms = 0;
    logLine("album_list: fetch uid=%d\n", s_uid);
    if (ym_api_liked_albums(&ctx, s_uid, &list, &count) != 0) {
        logLine("album_list: fetch failed\n");
        s_error = 1;
    } else {
        free(s_albums);
        s_albums = list;
        s_count = count;
        if (s_count > ALBUM_LIST_MAX) {
            s_count = ALBUM_LIST_MAX;
        }
        s_selected = 0;
        s_scroll = 0;
        s_error = (s_count == 0);
        logLine("album_list: got %d\n", s_count);
    }
    memset(s_token, 0, sizeof(s_token));
    logger_flush();
    return 0;
}

static void album_list_join(void)
{
    SceUInt timeout_us;

    if (s_tid < 0) {
        return;
    }
    timeout_us = 15000000U;
    if (sceKernelWaitThreadEnd(s_tid, &timeout_us) == 0) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
    }
}

static void album_list_start(AppState *state)
{
    char token[256];

    if (s_loading || state->currentUser.uid <= 0) {
        return;
    }
    // Новый пользователь — список заново.
    if (s_uid != state->currentUser.uid) {
        free(s_albums);
        s_albums = NULL;
        s_count = 0;
        s_selected = 0;
        s_scroll = 0;
        s_error = 0;
        s_uid = state->currentUser.uid;
    } else if (s_albums || s_error) {
        return;  // уже есть (или честная ошибка)
    }
    album_list_join();
    if (s_tid >= 0) {
        return;
    }
    if (token_loader_read(token, sizeof(token)) != 0) {
        s_error = 1;
        return;
    }
    snprintf(s_token, sizeof(s_token), "%s", token);
    memset(token, 0, sizeof(token));
    s_loading = 1;
    s_error = 0;
    s_tid = sceKernelCreateThread("album_list", album_list_worker,
                                  0x18, 64 * 1024, 0, NULL);
    if (s_tid < 0) {
        s_loading = 0;
        memset(s_token, 0, sizeof(s_token));
        s_error = 1;
        return;
    }
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        s_loading = 0;
        memset(s_token, 0, sizeof(s_token));
        s_error = 1;
        return;
    }
}

static void album_list_reap(void)
{
    SceKernelThreadRunStatus st;

    if (s_tid < 0) {
        return;
    }
    st.size = sizeof(st);
    if (sceKernelReferThreadRunStatus(s_tid, &st) == 0 &&
        st.status == PSP_THREAD_STOPPED) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        s_loading = 0;
    }
}

void ui_screen_album_list_update(AppState *state)
{
    char first_id[40];
    int rc;

    album_list_reap();
    // Гость — чужой список не показываем.
    if (state->currentUser.uid <= 0) {
        if (s_albums || s_count > 0 || s_uid != 0) {
            free(s_albums);
            s_albums = NULL;
            s_count = 0;
            s_selected = 0;
            s_scroll = 0;
            s_error = 0;
            s_uid = 0;
        }
        return;
    }
    album_list_start(state);
    // Открытие альбома докатилось — в очередь и на плеер.
    rc = album_play_poll(first_id, sizeof(first_id));
    if (rc == 1) {
        memset(&state->now_playing_track, 0, sizeof(state->now_playing_track));
        snprintf(state->now_playing_track.id, sizeof(state->now_playing_track.id),
                 "%s", first_id);
        s_status_open = 0;
        ui_screens_navigate(state, SCREEN_NOW_PLAYING);
        playback_controller_request_play_current();
    } else if (rc == -1) {
        s_status_open = 0;
        logLine("album_list: open failed\n");
    }
}

void ui_screen_album_list_handle_input(AppState *state, const InputState *input)
{
    char token[256];

    (void)state;

    if (input->pressed & PSP_CTRL_UP) {
        if (s_selected > 0) {
            s_selected--;
            if (s_selected < s_scroll) {
                s_scroll = s_selected;
            }
        }
    }
    if (input->pressed & PSP_CTRL_DOWN) {
        if (s_selected < s_count - 1) {
            s_selected++;
            if (s_selected >= s_scroll + ALBUM_LIST_VISIBLE) {
                s_scroll = s_selected - ALBUM_LIST_VISIBLE + 1;
            }
        }
    }
    if (input->pressed & PSP_CTRL_CROSS) {
        if (s_selected >= 0 && s_selected < s_count &&
            !album_play_busy()) {
            int album_id = s_albums[s_selected].album_id;
            if (album_id > 0 &&
                token_loader_read(token, sizeof(token)) == 0) {
                logLine("album_list: open album_id=%d\n", album_id);
                album_play_start(token, album_id);
                memset(token, 0, sizeof(token));
                s_status_open = 1;
            }
        }
    }
}

void ui_screen_album_list_render(void)
{
    int i, start_idx, end_idx;

    ui_draw_clear(UI_COLOR_BACKGROUND);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_ALBUMS));

    if (s_loading) {
        ui_draw_text(16.0f, 48.0f, "...", UI_COLOR_ACTIVE);
        return;
    }
    if (s_error || s_count == 0) {
        ui_draw_text(16.0f, 48.0f, locale_get(LOCALE_SCREEN_EMPTY), UI_COLOR_ACTIVE);
        return;
    }

    start_idx = s_scroll;
    end_idx = start_idx + ALBUM_LIST_VISIBLE;
    if (end_idx > s_count) {
        end_idx = s_count;
    }
    // Обложки видимых.
    for (i = start_idx; i < end_idx; i++) {
        const ArtistAlbumEntry *a = &s_albums[i];
        if (a->cover_uri[0] && a->album_id != 0) {
            cover_manager_request_cover(COVER_ENTITY_ALBUM, a->album_id,
                                        a->cover_uri, COVER_PRIORITY_VISIBLE,
                                        "30x30");
        }
    }

    for (i = start_idx; i < end_idx; i++) {
        float y = 48.0f + (float)(i - start_idx) * 34.0f;
        const ArtistAlbumEntry *a = &s_albums[i];
        char sub[32];

        if (i == s_selected) {
            ui_draw_rect(16.0f, y - 3.0f, 6.0f, 30.0f, UI_COLOR_ACCENT);
        }
        if (!cover_manager_draw_cover(COVER_ENTITY_ALBUM, a->album_id, NULL,
                                      27, (int)y, 30, 30)) {
            if (cover_manager_is_loading(COVER_ENTITY_ALBUM, a->album_id, NULL)) {
                ui_draw_rect(27.0f, y, 30.0f, 30.0f, UI_COLOR_INACTIVE);
            }
        }
        text_render_clipped(62.0f, y, a->title,
                            i == s_selected ? UI_COLOR_PRIMARY : UI_COLOR_ACTIVE,
                            480.0f - 62.0f - 8.0f);
        if (a->year > 0) {
            snprintf(sub, sizeof(sub), "%d", a->year);
        } else if (a->track_count > 0) {
            snprintf(sub, sizeof(sub), "%d", a->track_count);
        } else {
            sub[0] = '\0';
        }
        if (sub[0]) {
            ui_draw_text(62.0f, y + 16.0f, sub, UI_COLOR_INACTIVE);
        }
    }

    if (s_status_open) {
        ui_draw_text(16.0f, 250.0f, "...", UI_COLOR_ACCENT);
    }
}
