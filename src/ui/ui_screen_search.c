// Экран поиска треков: OSK-запрос -> воркер (ym_api_search_tracks) ->
// очередь + NOW_PLAYING. Воркер и выход — по образцу экрана логина
// (bounded join, живой тред не трогаем); игра с позиции — по образцу
// топа треков артиста (очередь из id, метаданные — штатный PENDING-механизм).
#include "ui/ui_screen_search.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "app/app_state.h"
#include "core/logger.h"
#include "services/locale.h"
#include "services/net_tls.h"
#include "services/osk_input.h"
#include "services/playback_controller.h"
#include "services/playback_queue.h"
#include "services/token_loader.h"
#include "services/ym_api.h"
#include "services/ym_api_search.h"
#include "ui/ui_common.h"
#include "ui/ui_draw.h"
#include "ui/ui_screens.h"

#define SEARCH_VISIBLE_ROWS 6
#define SEARCH_QUERY_MAX 128

// Статус: 0 нет запроса, 1 загрузка, 2 готово, -1 ошибка/отмена.
static char s_query[SEARCH_QUERY_MAX];
static int s_has_query = 0;
static int s_status = 0;
static int s_selected = 0;
static int s_scroll = 0;
static int s_generation = 0;

static ListIndexId s_ids[YM_API_SEARCH_MAX];
static int s_count = 0;

// Метаданные, подтянутые воркером (POST /tracks) для красивого списка.
// Окно в RAM воркера-экрана; UI только читает. Негидратированная строка —
// номер + id (track_meta_store_get из UI запрещён: там дисковый I/O).
static TrackEntry s_meta[YM_API_SEARCH_MAX];
static unsigned char s_meta_valid[YM_API_SEARCH_MAX];

static SceUID s_tid = -1;
static volatile int s_cancel = 0;
static volatile int s_done = 0;
static volatile int s_rc = -1;
static char s_token[256];

static void search_join_worker(void)
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

static YmApiStreamDecision search_hydrate_sink(const TrackEntry *entry,
                                               void *user_data)
{
    int k;

    (void)user_data;
    if (!entry || !entry->id[0]) {
        return YM_API_STREAM_CONTINUE;
    }
    for (k = 0; k < s_count; k++) {
        if (strcmp(s_ids[k], entry->id) == 0) {
            memcpy(&s_meta[k], entry, sizeof(*entry));
            s_meta_valid[k] = 1;
            break;
        }
    }
    return YM_API_STREAM_CONTINUE;
}

static int search_worker(SceSize args, void *argp)
{
    YmApiContext ctx;
    int n, h, step, status;

    (void)args;
    (void)argp;
    logLine("search: worker start query='%.32s'\n", s_query);
    net_tls_cancel_bind((volatile int *)&s_cancel);
    ctx.oauth_token = s_token;
    ctx.timeout_ms = 0;
    n = ym_api_search_tracks(&ctx, s_query, s_ids, YM_API_SEARCH_MAX);
    if (n <= 0) {
        memset(s_token, 0, sizeof(s_token));
        s_count = 0;
        s_rc = -1;
        s_done = 1;
        logLine("search: fetch failed\n");
        logger_flush();
        net_tls_cancel_unbind();
        return 0;
    }
    s_count = n;
    memset(s_meta_valid, 0, sizeof(s_meta_valid));
    // Догрузка названий чанками (лимит POST /tracks за раз).
    for (h = 0; h < n && !s_cancel; h += step) {
        step = n - h;
        if (step > YM_API_HYDRATE_MAX) {
            step = YM_API_HYDRATE_MAX;
        }
        status = 0;
        if (ym_api_tracks_hydrate(&ctx, &s_ids[h], step,
                                  search_hydrate_sink, NULL,
                                  &status) != 0) {
            logLine("search: hydrate failed off=%d status=%d\n", h, status);
            break;
        }
    }
    memset(s_token, 0, sizeof(s_token));
    s_rc = 0;
    s_done = 1;
    logLine("search: done n=%d\n", n);
    logger_flush();
    net_tls_cancel_unbind();
    return 0;
}

static int search_poll_finished(void)
{
    SceKernelThreadRunStatus st;

    if (s_tid < 0) {
        return 1;
    }
    st.size = sizeof(st);
    if (sceKernelReferThreadRunStatus(s_tid, &st) == 0 &&
        st.status == PSP_THREAD_STOPPED) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        return 1;
    }
    return 0;
}

static void search_start_worker(void)
{
    s_cancel = 1;
    search_join_worker();
    if (s_tid >= 0) {
        return;  // старый воркер ещё висит — новый не плодим
    }
    if (token_loader_read(s_token, sizeof(s_token)) != 0) {
        s_status = -1;
        logLine("search: no token\n");
        logger_flush();
        return;
    }
    s_cancel = 0;
    s_done = 0;
    s_rc = -1;
    s_count = 0;
    s_selected = 0;
    s_scroll = 0;
    memset(s_meta_valid, 0, sizeof(s_meta_valid));
    s_generation++;
    s_status = 1;
    s_tid = sceKernelCreateThread("search_worker", search_worker,
                                  0x18, 64 * 1024, 0, NULL);
    if (s_tid < 0) {
        memset(s_token, 0, sizeof(s_token));
        s_status = -1;
        logLine("search: create thread failed 0x%08X\n", s_tid);
        logger_flush();
        return;
    }
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        memset(s_token, 0, sizeof(s_token));
        s_status = -1;
        logLine("search: start thread failed\n");
        logger_flush();
        return;
    }
    logLine("search: worker started\n");
    logger_flush();
}

// Один проход OSK: 0 = есть запрос, иначе -1 (отмена/пусто).
static int search_ask_query(void)
{
    char buf[SEARCH_QUERY_MAX];
    int r = osk_input_text(locale_get(LOCALE_SCREEN_SEARCH),
                           s_has_query ? s_query : "",
                           buf, sizeof(buf));
    if (r != 0 || !buf[0]) {
        return -1;
    }
    snprintf(s_query, sizeof(s_query), "%s", buf);
    s_has_query = 1;
    return 0;
}

static void search_play_at(AppState *state, int index)
{
    if (!state || index < 0 || index >= s_count) {
        return;
    }
    // Источник PLAYBACK_QUEUE_SOURCE_NONE: результаты поиска — разовый
    // ad-hoc список без привязки к альбому/плейлисту/волне/артисту
    // (других значений enum нет, shared enums не трогаем).
    if (playback_queue_set_from_ids(s_ids, s_count, index,
                                    PLAYBACK_QUEUE_SOURCE_NONE,
                                    0, s_generation) != 0) {
        logLine("search: queue set failed\n");
        logger_flush();
        return;
    }
    logLine("search: play index=%d count=%d\n", index, s_count);
    logger_flush();
    memset(&state->now_playing_track, 0, sizeof(state->now_playing_track));
    snprintf(state->now_playing_track.id,
             sizeof(state->now_playing_track.id), "%s", s_ids[index]);
    app_state_push(state, SCREEN_NOW_PLAYING);
    playback_controller_request_play_current();
}

void ui_screen_search_on_enter(AppState *state)
{
    (void)state;
    if (s_has_query && s_count > 0) {
        return;  // повторный вход: показываем прошлые результаты
    }
    if (s_tid >= 0 && !search_poll_finished()) {
        s_status = 1;
        return;  // воркер с прошлого входа ещё летит
    }
    if (search_ask_query() == 0) {
        search_start_worker();
    } else {
        s_status = -1;
        logLine("search: query cancelled\n");
        logger_flush();
    }
}

void ui_screen_search_on_exit(AppState *state)
{
    (void)state;
    logLine("search: exit\n");
    s_cancel = 1;
    search_join_worker();
}

void ui_screen_search_update(AppState *state)
{
    (void)state;
    if (s_status != 1) {
        return;
    }
    if (!search_poll_finished() || !s_done) {
        return;
    }
    if (s_rc == 0 && s_count > 0) {
        s_status = 2;
        logLine("search: ready count=%d\n", s_count);
    } else {
        s_status = -1;
        logLine("search: failed\n");
    }
    logger_flush();
}

void ui_screen_search_handle_input(AppState *state, const InputState *input)
{
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
            if (s_selected >= s_scroll + SEARCH_VISIBLE_ROWS) {
                s_scroll = s_selected - SEARCH_VISIBLE_ROWS + 1;
            }
        }
    }
    if (input->pressed & PSP_CTRL_SQUARE) {
        // Новый запрос (работает и из пустого/ошибочного состояния).
        if (search_ask_query() == 0) {
            search_start_worker();
        } else {
            s_status = -1;
        }
    }
    if (input->pressed & PSP_CTRL_CROSS) {
        if (s_count > 0) {
            search_play_at(state, s_selected);
        } else if (search_ask_query() == 0) {
            search_start_worker();
        } else {
            s_status = -1;
        }
    }
}

void ui_screen_search_render(const AppState *state)
{
    char line[512];
    int start_idx, end_idx, row;

    (void)state;
    ui_draw_clear(0xFF1A1A1A);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_SEARCH));
    snprintf(line, sizeof(line), "%.100s",
             s_has_query ? s_query : "...");
    ui_draw_text(16.0f, 30.0f, line, 0xFFBBBBBB);

    if (s_status == 1) {
        ui_draw_text(16.0f, 52.0f, "...", 0xFF00D5FF);
    } else if (s_status == -1 && s_count == 0) {
        ui_draw_text(16.0f, 52.0f,
                     locale_get(LOCALE_SCREEN_EMPTY), 0xFFBBBBBB);
    } else if (s_status == 2 && s_count == 0) {
        ui_draw_text(16.0f, 52.0f,
                     locale_get(LOCALE_SCREEN_EMPTY), 0xFFBBBBBB);
    } else if (s_count > 0) {
        start_idx = s_scroll;
        if (start_idx < 0) {
            start_idx = 0;
        }
        if (start_idx > s_count - 1) {
            start_idx = s_count - 1;
        }
        end_idx = start_idx + SEARCH_VISIBLE_ROWS;
        if (end_idx > s_count) {
            end_idx = s_count;
        }
        for (row = start_idx; row < end_idx; row++) {
            float y = 52.0f + (float)(row - start_idx) * 34.0f;
            if (row == s_selected) {
                ui_draw_rect(16.0f, y - 3.0f, 6.0f, 30.0f, 0xFF00D5FF);
            }
            if (s_meta_valid[row]) {
                if (s_meta[row].artist[0]) {
                    snprintf(line, sizeof(line), "%d. %s - %s",
                             row + 1, s_meta[row].artist,
                             s_meta[row].title);
                } else {
                    snprintf(line, sizeof(line), "%d. %s",
                             row + 1, s_meta[row].title);
                }
            } else {
                snprintf(line, sizeof(line), "%d. %s", row + 1, s_ids[row]);
            }
            ui_draw_text(30.0f, y, line,
                         row == s_selected ? 0xFFFFFFFF : 0xFFBBBBBB);
        }
    }
}
