// Открытие альбома в очередь. См. album_play.h.
#include "services/album_play.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/logger.h"
#include "services/ym_api_albums.h"
#include "services/playback_queue.h"

static SceUID s_tid = -1;
static char s_token[256];
static int s_album_id = 0;
static volatile int s_state = 0;  // 0 идёт, 1 готово, -1 провал
static char s_first_id[40];

static int album_worker(SceSize args, void *argp)
{
    YmApiContext ctx;
    ListIndexId *ids = NULL;
    int count = 0;

    (void)args;
    (void)argp;
    logLine("album_play: fetch album_id=%d\n", s_album_id);
    ctx.oauth_token = s_token;
    ctx.timeout_ms = 0;
    if (ym_api_album_track_ids(&ctx, s_album_id, &ids, &count) != 0) {
        logLine("album_play: fetch failed album_id=%d\n", s_album_id);
        s_state = -1;
        memset(s_token, 0, sizeof(s_token));
        logger_flush();
        return 0;
    }
    if (playback_queue_set_from_ids(ids, count, 0,
                                    PLAYBACK_QUEUE_SOURCE_ALBUM,
                                    s_album_id, 0) != 0) {
        logLine("album_play: queue set failed\n");
        free(ids);
        s_state = -1;
        memset(s_token, 0, sizeof(s_token));
        logger_flush();
        return 0;
    }
    snprintf(s_first_id, sizeof(s_first_id), "%s", ids[0]);
    free(ids);
    s_state = 1;
    logLine("album_play: ready album_id=%d count=%d\n", s_album_id, count);
    memset(s_token, 0, sizeof(s_token));
    logger_flush();
    return 0;
}

static void album_join(void)
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

void album_play_start(const char *token, int album_id)
{
    if (!token || !token[0] || album_id <= 0) {
        return;
    }
    album_join();
    if (s_tid >= 0) {
        logLine("album_play: busy, ignored album_id=%d\n", album_id);
        return;  // прошлый запрос ещё летит
    }
    snprintf(s_token, sizeof(s_token), "%s", token);
    s_album_id = album_id;
    s_state = 0;
    s_first_id[0] = '\0';
    s_tid = sceKernelCreateThread("album_play", album_worker,
                                  0x18, 64 * 1024, 0, NULL);
    if (s_tid < 0) {
        logLine("album_play: create failed 0x%08X\n", s_tid);
        memset(s_token, 0, sizeof(s_token));
        return;
    }
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        logLine("album_play: start failed\n");
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        memset(s_token, 0, sizeof(s_token));
        return;
    }
}

// 1 = воркер кончился (тред удалён), результат в s_state.
static int album_worker_done(void)
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

int album_play_poll(char *out_first_id, int id_size)
{
    int done;

    if (!album_worker_done()) {
        return 0;
    }
    done = s_state;
    s_state = 0;  // одноразовый: повторный poll не стреляет снова
    if (done == 1 && out_first_id && id_size > 0) {
        snprintf(out_first_id, (size_t)id_size, "%s", s_first_id);
    }
    return done;
}

int album_play_busy(void)
{
    if (s_tid >= 0 && !album_worker_done()) {
        return 1;
    }
    return s_state != 0;  // кончился, но результат ещё не забрали
}

void album_play_reset(void)
{
    album_join();
    s_state = 0;
    s_first_id[0] = '\0';
}
