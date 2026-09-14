// Персональная волна. См. wave.h.
#include "services/wave.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/ym_api_wave.h"
#include "services/playback_queue.h"
#include "services/token_loader.h"

#define WAVE_BATCH_MAX 32
#define WAVE_REFILL_AHEAD 2

static SceUID s_tid = -1;
static char s_token[256];
static int s_mode = 0;  // 1 первая пачка, 2 догрузка
static volatile int s_state = 0;  // 0 идёт, 1 готово, -1 провал
static ListIndexId s_ids[WAVE_BATCH_MAX];
static int s_count = 0;
static char s_first_id[40];

static int wave_worker(SceSize args, void *argp)
{
    YmApiContext ctx;
    int n;

    (void)args;
    (void)argp;
    ctx.oauth_token = s_token;
    ctx.timeout_ms = 0;
    logLine("wave: fetch mode=%d\n", s_mode);
    n = ym_api_wave_station_tracks(&ctx, WAVE_STATION_MY, s_ids,
                                   WAVE_BATCH_MAX);
    memset(s_token, 0, sizeof(s_token));
    if (n <= 0) {
        logLine("wave: fetch failed\n");
        s_state = -1;
        logger_flush();
        return 0;
    }
    s_count = n;
    if (s_mode == 1) {
        if (playback_queue_set_from_ids(s_ids, n, 0,
                                        PLAYBACK_QUEUE_SOURCE_FLOW,
                                        0, 0) != 0) {
            s_state = -1;
        } else {
            snprintf(s_first_id, sizeof(s_first_id), "%s", s_ids[0]);
            s_state = 1;
        }
    } else {
        // Догрузка применяется вызывателем (проверка source там же).
        s_state = 1;
    }
    logLine("wave: fetched %d mode=%d\n", n, s_mode);
    logger_flush();
    return 0;
}

static int wave_worker_done(void)
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

static void wave_launch(int mode, const char *token)
{
    if (!token || !token[0]) {
        return;
    }
    if (s_tid >= 0 && !wave_worker_done()) {
        return;  // уже летит
    }
    snprintf(s_token, sizeof(s_token), "%s", token);
    s_mode = mode;
    s_state = 0;
    s_count = 0;
    s_first_id[0] = '\0';
    s_tid = sceKernelCreateThread("wave_worker", wave_worker,
                                  0x18, 64 * 1024, 0, NULL);
    if (s_tid < 0) {
        memset(s_token, 0, sizeof(s_token));
        return;
    }
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        memset(s_token, 0, sizeof(s_token));
        return;
    }
}

void wave_start_first(const char *token)
{
    wave_launch(1, token);
}

int wave_poll_first(char *out_first_id, int id_size)
{
    int done;

    if (!wave_worker_done()) {
        return 0;
    }
    done = s_state;
    s_state = 0;
    if (done == 1 && out_first_id && id_size > 0) {
        snprintf(out_first_id, (size_t)id_size, "%s", s_first_id);
    }
    return done;
}

void wave_service(void)
{
    PlaybackQueueInfo info;
    char token[256];
    int remaining;

    if (s_tid >= 0 && !wave_worker_done()) {
        return;  // догрузка уже летит
    }
    if (s_tid < 0 && s_state != 0) {
        // Прошлый результат: применить или выкинуть.
        int st = s_state;
        s_state = 0;
        if (st == 1 && s_mode == 2 && s_count > 0) {
            if (playback_queue_get_info(&info) == 0 &&
                info.source == PLAYBACK_QUEUE_SOURCE_FLOW) {
                if (playback_queue_append_ids(s_ids, s_count) == 0) {
                    logLine("wave: refilled +%d\n", s_count);
                    logger_flush();
                }
            } else {
                logLine("wave: refill dropped (not flow)\n");
            }
        }
        s_count = 0;
    }
    if (s_tid >= 0) {
        return;
    }
    // Волна играет и почти кончилась — тянем следующую пачку заранее.
    if (playback_queue_get_info(&info) != 0 ||
        info.source != PLAYBACK_QUEUE_SOURCE_FLOW ||
        info.order_count <= 0) {
        return;
    }
    remaining = info.order_count - info.order_pos - 1;
    if (remaining > WAVE_REFILL_AHEAD) {
        return;
    }
    if (token_loader_read(token, sizeof(token)) != 0) {
        return;
    }
    wave_launch(2, token);
    memset(token, 0, sizeof(token));
}

void wave_reset(void)
{
    SceUInt timeout_us;

    if (s_tid >= 0) {
        timeout_us = 15000000U;
        if (sceKernelWaitThreadEnd(s_tid, &timeout_us) == 0) {
            sceKernelDeleteThread(s_tid);
            s_tid = -1;
        }
    }
    s_state = 0;
    s_count = 0;
    s_first_id[0] = '\0';
}
