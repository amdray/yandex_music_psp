#include "ui/ui_wave_carousel.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app_state.h"
#include "core/logger.h"
#include "services/audio_player.h"
#include "services/cover_manager.h"
#include "services/locale.h"
#include "services/playback_controller.h"
#include "services/token_loader.h"
#include "services/wave.h"
#include "services/ym_api_rotor.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_glow_bg.h"

#define CAROUSEL_MAX_CARDS 8
#define CAROUSEL_VISIBLE 6

static int s_open = 0;
static int s_album_id = 0;
static WaveCarouselItem s_cards[CAROUSEL_MAX_CARDS];
static int s_count = 0;
static int s_selected = 0;
static int s_scroll = 0;
static int s_loading = 0;   // воркер карточек летит
static int s_error = 0;
static int s_pending_session = 0;  // ждём session/new после выбора
static SceUID s_tid = -1;
static char s_token[256];
static int s_want_album = 0;

static int carousel_worker(SceSize args, void *argp)
{
    YmApiContext ctx;
    WaveCarouselItem cards[CAROUSEL_MAX_CARDS];
    int n;

    (void)args;
    (void)argp;
    ctx.oauth_token = s_token;
    ctx.timeout_ms = 0;
    n = ym_api_rotor_album_wave_cards(&ctx, s_want_album, cards,
                                      CAROUSEL_MAX_CARDS);
    memset(s_token, 0, sizeof(s_token));
    if (n < 0) {
        s_error = 1;
        logLine("carousel: fetch failed album=%d\n", s_want_album);
    } else {
        memcpy(s_cards, cards, sizeof(WaveCarouselItem) * (size_t)n);
        s_count = n;
        s_selected = 0;
        s_scroll = 0;
        s_error = (n == 0);
        logLine("carousel: got %d cards album=%d\n", n, s_want_album);
        // Обложки карточек (только с распознанным id, иначе имя).
        {
            int i;
            for (i = 0; i < n; i++) {
                CoverEntityType et;
                int eid = 0;
                const char *col;
                if (!s_cards[i].cover_uri[0] ||
                    !s_cards[i].entity_type[0]) {
                    continue;
                }
                if (strcmp(s_cards[i].entity_type, "album") == 0) {
                    et = COVER_ENTITY_ALBUM;
                } else if (strcmp(s_cards[i].entity_type, "artist") == 0) {
                    et = COVER_ENTITY_ARTIST;
                } else if (strcmp(s_cards[i].entity_type, "playlist") == 0) {
                    et = COVER_ENTITY_PLAYLIST;
                } else {
                    continue;
                }
                col = strchr(s_cards[i].seeds[0], ':');
                if (s_cards[i].seed_count > 0 && col) {
                    eid = atoi(col + 1);
                }
                if (eid > 0) {
                    cover_manager_request_cover(et, eid,
                                                s_cards[i].cover_uri,
                                                COVER_PRIORITY_VISIBLE,
                                                "30x30");
                }
            }
        }
    }
    logger_flush();
    return 0;
}

static void carousel_join(void)
{
    SceUInt timeout_us;

    if (s_tid < 0) {
        return;
    }
    timeout_us = 8000000U;
    if (sceKernelWaitThreadEnd(s_tid, &timeout_us) == 0) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
    }
}

void carousel_open(AppState *state)
{
    char token[256];
    int album_id = 0;

    if (!state || s_open) {
        return;
    }
    if (state->now_playing_track.id[0]) {
        album_id = state->now_playing_track.album_id;
    }
    if (token_loader_read(token, sizeof(token)) != 0) {
        return;
    }
    carousel_join();
    if (s_tid >= 0) {
        memset(token, 0, sizeof(token));
        return;
    }
    s_open = 1;
    s_album_id = album_id;
    s_count = 0;
    s_selected = 0;
    s_scroll = 0;
    s_error = 0;
    s_pending_session = 0;
    memset(s_cards, 0, sizeof(s_cards));
    if (album_id <= 0) {
        // Без контекста альбома — только «волна по треку», без запроса.
        memset(token, 0, sizeof(token));
        logLine("carousel: open, no album context\n");
        return;
    }
    snprintf(s_token, sizeof(s_token), "%s", token);
    memset(token, 0, sizeof(token));
    s_want_album = album_id;
    s_loading = 1;
    s_tid = sceKernelCreateThread("carousel", carousel_worker,
                                  0x18, 64 * 1024, 0, NULL);
    if (s_tid < 0) {
        memset(s_token, 0, sizeof(s_token));
        s_loading = 0;
        s_error = 1;
        return;
    }
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        memset(s_token, 0, sizeof(s_token));
        s_loading = 0;
        s_error = 1;
        return;
    }
    logLine("carousel: open album=%d\n", album_id);
}

void carousel_close(void)
{
    s_open = 0;
    s_pending_session = 0;
    carousel_join();
    s_loading = 0;
}

static void carousel_reap(void)
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

// Запуск сессии по набору сидов: текущий трек закрывается одним skip.
static void carousel_start_session(AppState *state, const char *const *seeds,
                                   int seed_count, int wave_model)
{
    char token[256];
    AudioPlayerSnapshot snap;
    char tid[40];
    int alb = 0, pos = 0, len = 0;

    if (!state || !seeds || seed_count <= 0) {
        return;
    }
    snprintf(tid, sizeof(tid), "%s", state->now_playing_track.id);
    alb = state->now_playing_track.album_id;
    if (audio_player_get_snapshot(&snap)) {
        pos = snap.position_ms;
        len = snap.duration_ms;
    }
    if (tid[0] && pos > 0) {
        wave_note_skip(tid, alb, pos, len);
    }
    if (token_loader_read(token, sizeof(token)) != 0) {
        return;
    }
    logLine("carousel: start session seeds=%d\n", seed_count);
    wave_start_seeds(token, seeds, seed_count, wave_model);
    memset(token, 0, sizeof(token));
    s_pending_session = 1;
    s_open = 0;  // оверлей закрываем сразу, трек подхватит update
}

void carousel_update(AppState *state)
{
    char first_id[40];
    int rc;

    carousel_reap();
    if (!s_open && !s_pending_session) {
        return;
    }
    if (!s_pending_session) {
        return;
    }
    rc = wave_poll_first(first_id, sizeof(first_id));
    if (rc == 1) {
        memset(&state->now_playing_track, 0, sizeof(state->now_playing_track));
        snprintf(state->now_playing_track.id, sizeof(state->now_playing_track.id),
                 "%s", first_id);
        s_pending_session = 0;
        playback_controller_request_play_current();
        logLine("carousel: session playing\n");
    } else if (rc == -1) {
        s_pending_session = 0;
        logLine("carousel: session failed\n");
    }
    logger_flush();
}

void carousel_input(AppState *state, const InputState *input)
{
    int rows = s_count + 1;  // строка 0 — «волна по треку»
    int i;
    const char *seeds[4];

    (void)state;
    if (input->pressed & PSP_CTRL_CIRCLE) {
        s_open = 0;
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
        if (s_selected < rows - 1) {
            s_selected++;
            if (s_selected >= s_scroll + CAROUSEL_VISIBLE) {
                s_scroll = s_selected - CAROUSEL_VISIBLE + 1;
            }
        }
    }
    if (input->pressed & PSP_CTRL_CROSS) {
        if (s_selected == 0) {
            // «Моя волна по треку»: seed track:<id>.
            char seedbuf[64];
            if (!state || !state->now_playing_track.id[0]) {
                return;
            }
            snprintf(seedbuf, sizeof(seedbuf), "track:%s",
                     state->now_playing_track.id);
            logLine("carousel: track wave\n");
            {
                // seeds[] должен жить до вызова — локальный массив ок
                // (wave_start_seeds копирует синхронно внутрь start).
                static char s_seed_copy[64];
                const char *arr[1];
                snprintf(s_seed_copy, sizeof(s_seed_copy), "%s", seedbuf);
                arr[0] = s_seed_copy;
                carousel_start_session(state, arr, 1, 1);
            }
        } else {
            WaveCarouselItem *c = &s_cards[s_selected - 1];
            if (c->seed_count <= 0) {
                return;
            }
            for (i = 0; i < c->seed_count && i < 4; i++) {
                seeds[i] = c->seeds[i];
            }
            carousel_start_session(state, seeds, c->seed_count, 0);
        }
    }
}

void carousel_render(const AppState *state)
{
    int rows = s_count + 1;
    int start_idx, end_idx, row;
    float bx = 40.0f, bw = 400.0f, by = 40.0f;

    (void)state;
    if (!s_open) {
        return;
    }
    // Свечение как в Яндексе за панелью карусели.
    glow_bg_draw(0.0f, 0.0f, 480.0f, 272.0f);
    ui_draw_rect(0.0f, 0.0f, 480.0f, 272.0f, 0x66000000);
    ui_draw_rect(bx, by, bw, 30.0f + (float)CAROUSEL_VISIBLE * 24.0f, 0xEE1A1A1A);
    ui_draw_text(bx + 12.0f, by + 8.0f, locale_get(LOCALE_CAROUSEL_TITLE),
                 0xFFFFFFFF);
    start_idx = s_scroll;
    if (start_idx < 0) {
        start_idx = 0;
    }
    if (start_idx > rows - 1) {
        start_idx = rows - 1;
    }
    end_idx = start_idx + CAROUSEL_VISIBLE;
    if (end_idx > rows) {
        end_idx = rows;
    }
    if (s_loading) {
        ui_draw_text(bx + 12.0f, by + 34.0f, "...", 0xFF00D5FF);
        return;
    }
    for (row = start_idx; row < end_idx; row++) {
        float y = by + 34.0f + (float)(row - start_idx) * 24.0f;
        char line[160];
        if (row == s_selected) {
            ui_draw_rect(bx + 6.0f, y - 2.0f, 6.0f, 14.0f, 0xFF00D5FF);
        }
        if (row == 0) {
            snprintf(line, sizeof(line), "%s", locale_get(LOCALE_TRACK_WAVE));
            ui_draw_text(bx + 18.0f, y, line,
                         row == s_selected ? 0xFFFFFFFF : 0xFFBBBBBB);
            continue;
        }
        {
            WaveCarouselItem *c = &s_cards[row - 1];
            CoverEntityType et;
            int eid = 0;
            const char *col;
            int has_cover = 0;
            if (c->cover_uri[0] && c->entity_type[0] && c->seed_count > 0) {
                if (strcmp(c->entity_type, "album") == 0) {
                    et = COVER_ENTITY_ALBUM;
                } else if (strcmp(c->entity_type, "artist") == 0) {
                    et = COVER_ENTITY_ARTIST;
                } else if (strcmp(c->entity_type, "playlist") == 0) {
                    et = COVER_ENTITY_PLAYLIST;
                } else {
                    et = COVER_ENTITY_ALBUM;
                    eid = -1;
                }
                if (eid >= 0) {
                    col = strchr(c->seeds[0], ':');
                    if (col) {
                        eid = atoi(col + 1);
                    }
                }
                if (eid > 0) {
                    has_cover = cover_manager_draw_cover(et, eid, "30x30",
                                                         (int)(bx + 18.0f),
                                                         (int)y, 20, 20);
                    if (!has_cover &&
                        cover_manager_is_loading(et, eid, "30x30")) {
                        ui_draw_rect(bx + 18.0f, y, 20.0f, 20.0f, 0xFF444444);
                    }
                }
            }
            snprintf(line, sizeof(line), "%.80s", c->name);
            ui_draw_text(bx + (has_cover || eid > 0 ? 44.0f : 18.0f), y, line,
                         row == s_selected ? 0xFFFFFFFF : 0xFFBBBBBB);
        }
    }
}

int carousel_is_open(void)
{
    return s_open;
}
