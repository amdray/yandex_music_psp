/* Rotor "My wave" session. See wave.h.
 * First batch + refill run on a 64KB worker; results are staged and applied
 * on the UI thread (poll_first / wave_service) only when the generation
 * still matches, so a late worker can never corrupt a newer session. */
#include "services/wave.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/playback_queue.h"
#include "services/playback_reporter.h"
#include "services/token_loader.h"
#include "services/track_meta_store.h"
#include "services/ym_api_rotor.h"
#include "services/ym_api_types.h"

#define WAVE_BATCH_MAX 32
#define WAVE_REFILL_AHEAD 2
#define WAVE_REFILL_QUEUE_MAX 64
#define WAVE_COMPOSITE_SIZE 56
#define WAVE_WORKER_PRIORITY 0x18
#define WAVE_WORKER_STACK (64 * 1024)
#define WAVE_REFILL_RETRY_US 5000000ULL

static SceUID s_tid = -1;
static char s_token[256];
static int s_mode = 0; /* 0 idle, 1 first batch, 2 refill */
static volatile int s_state = 0; /* first batch: 0 pending, 1 ready, -1 failed */
static unsigned int s_generation = 0;
static WaveSession s_session;
static int s_unknown_session = 0;
static char s_first_id[40];

/* First-batch launch parameters + staged result (worker -> poll_first). */
static char s_first_seeds[YM_ROTOR_SEEDS_MAX][YM_ROTOR_SEED_SIZE];
static int s_first_seed_count = 0;
static int s_first_wave_model = 0;
static unsigned int s_first_generation = 0;
static YmRotorSequenceItem s_first_items[WAVE_BATCH_MAX];
static int s_first_count = 0;
static char s_first_session[YM_ROTOR_SESSION_ID_SIZE];
static char s_first_batch[YM_ROTOR_BATCH_ID_SIZE];

/* Refill request snapshot (UI thread) + staged result (worker -> service). */
static char s_refill_session[YM_ROTOR_SESSION_ID_SIZE];
static char s_refill_queue[WAVE_REFILL_QUEUE_MAX][WAVE_COMPOSITE_SIZE];
static int s_refill_queue_count = 0;
static unsigned int s_refill_generation = 0;
static YmRotorSequenceItem s_refill_items[WAVE_BATCH_MAX];
static int s_refill_count = 0;
static char s_refill_batch[YM_ROTOR_BATCH_ID_SIZE];
static int s_refill_terminated = 0;
static int s_refill_unknown = 0;
static unsigned int s_refill_result_generation = 0;
static volatile int s_refill_ready = 0;
static u64 s_refill_retry_after_us = 0;

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

/* Fresh token preferred (rotation-safe); cached launch token as fallback. */
static int wave_current_token(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (token_loader_read(out, (int)out_size) == 0 && out[0]) {
        return 0;
    }
    if (s_token[0]) {
        snprintf(out, out_size, "%s", s_token);
        return 0;
    }
    out[0] = '\0';
    return -1;
}

static int wave_worker(SceSize args, void *argp)
{
    YmApiContext api_ctx;
    char token_copy[256];
    int mode;

    (void)args;
    (void)argp;
    snprintf(token_copy, sizeof(token_copy), "%s", s_token);
    api_ctx.oauth_token = token_copy;
    api_ctx.timeout_ms = 0;
    mode = s_mode;

    if (mode == 1) {
        const char *seeds[YM_ROTOR_SEEDS_MAX];
        YmRotorSessionInfo info;
        int count = 0;
        int i;
        int rc;

        for (i = 0; i < YM_ROTOR_SEEDS_MAX; ++i) {
            seeds[i] = s_first_seeds[i];
        }
        logLine("wave: fetch first seeds=%d\n", s_first_seed_count);
        rc = ym_api_rotor_session_new(&api_ctx, seeds, s_first_seed_count,
                                      s_first_wave_model,
                                      &info, s_first_items,
                                      WAVE_BATCH_MAX, &count);
        memset(token_copy, 0, sizeof(token_copy));
        if (rc != 0 || count <= 0) {
            logLine("wave: first fetch failed\n");
            logger_flush();
            s_state = -1;
            return 0;
        }
        for (i = 0; i < count; ++i) {
            track_meta_store_put(&s_first_items[i].track);
        }
        snprintf(s_first_session, sizeof(s_first_session), "%s",
                 info.radio_session_id);
        snprintf(s_first_batch, sizeof(s_first_batch), "%s", info.batch_id);
        s_first_count = count;
        s_state = 1;
        logLine("wave: first fetched %d\n", count);
        logger_flush();
        return 0;
    }

    if (mode == 2) {
        const char *queue[WAVE_REFILL_QUEUE_MAX];
        YmRotorSessionInfo info;
        int count = 0;
        int i;
        int rc;

        for (i = 0; i < s_refill_queue_count; ++i) {
            queue[i] = s_refill_queue[i];
        }
        logLine("wave: fetch refill queue=%d\n", s_refill_queue_count);
        memset(&info, 0, sizeof(info));
        rc = ym_api_rotor_session_tracks(&api_ctx, s_refill_session,
                                         queue, s_refill_queue_count,
                                         &info, s_refill_items,
                                         WAVE_BATCH_MAX, &count);
        memset(token_copy, 0, sizeof(token_copy));
        if (rc != 0) {
            if (info.unknown_session) {
                s_refill_unknown = 1;
                s_refill_count = 0;
                s_refill_result_generation = s_refill_generation;
                s_refill_ready = 1;
                logLine("wave: refill unknown session\n");
            } else {
                s_refill_retry_after_us =
                    (u64)sceKernelGetSystemTimeWide() + WAVE_REFILL_RETRY_US;
                logLine("wave: refill failed, backoff\n");
            }
            logger_flush();
            return 0;
        }
        for (i = 0; i < count; ++i) {
            track_meta_store_put(&s_refill_items[i].track);
        }
        snprintf(s_refill_batch, sizeof(s_refill_batch), "%s", info.batch_id);
        s_refill_count = count;
        s_refill_terminated = info.terminated;
        s_refill_unknown = 0;
        s_refill_result_generation = s_refill_generation;
        s_refill_ready = 1;
        logLine("wave: refilled %d terminated=%d\n",
                count, info.terminated);
        logger_flush();
        return 0;
    }

    memset(token_copy, 0, sizeof(token_copy));
    return 0;
}

static void wave_launch_worker(void)
{
    s_tid = sceKernelCreateThread("wave_worker", wave_worker,
                                  WAVE_WORKER_PRIORITY,
                                  WAVE_WORKER_STACK, 0, NULL);
    if (s_tid < 0) {
        s_mode = 0;
        s_state = -1;
        return;
    }
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        s_mode = 0;
        s_state = -1;
        return;
    }
}

void wave_start_seeds(const char *token, const char *const *seeds,
                      int seed_count, int include_wave_model)
{
    int i;

    if (!token || !token[0]) {
        return;
    }
    wave_reset();
    snprintf(s_token, sizeof(s_token), "%s", token);

    memset(s_first_seeds, 0, sizeof(s_first_seeds));
    s_first_seed_count = 0;
    if (seeds && seed_count > 0) {
        s_first_seed_count = (seed_count < YM_ROTOR_SEEDS_MAX)
            ? seed_count : YM_ROTOR_SEEDS_MAX;
        for (i = 0; i < s_first_seed_count; ++i) {
            if (seeds[i] && seeds[i][0]) {
                snprintf(s_first_seeds[i], sizeof(s_first_seeds[i]),
                         "%s", seeds[i]);
            }
        }
    }
    s_first_wave_model = include_wave_model ? 1 : 0;
    s_first_generation = s_generation;
    s_first_count = 0;
    s_mode = 1;
    s_state = 0;
    wave_launch_worker();
    if (s_tid < 0) {
        memset(s_token, 0, sizeof(s_token));
    }
}

void wave_start_first(const char *token)
{
    wave_start_seeds(token, NULL, 0, 0);
}

int wave_poll_first(char *out_first_id, int id_size)
{
    ListIndexId ids[WAVE_BATCH_MAX];
    int i;

    if (!wave_worker_done()) {
        return 0;
    }
    if (s_mode != 1) {
        return 0;
    }
    if (s_state == 0) {
        return 0;
    }
    if (s_state < 0 || s_first_count <= 0 ||
        s_first_generation != s_generation) {
        s_state = 0;
        s_mode = 0;
        return -1;
    }

    /* Commit on the UI thread: session + queue + FLOW metadata. */
    for (i = 0; i < s_first_count; ++i) {
        snprintf(ids[i], sizeof(ids[i]), "%s", s_first_items[i].track.id);
    }
    if (playback_queue_set_from_ids(ids, s_first_count, 0,
                                    PLAYBACK_QUEUE_SOURCE_FLOW,
                                    0, (int)s_generation) != 0) {
        s_state = 0;
        s_mode = 0;
        return -1;
    }
    for (i = 0; i < s_first_count; ++i) {
        playback_queue_set_flow_meta(i, s_first_items[i].track.album_id,
                                     s_first_batch);
    }
    memset(&s_session, 0, sizeof(s_session));
    s_session.active = 1;
    s_session.generation = s_generation;
    snprintf(s_session.radio_session_id,
             sizeof(s_session.radio_session_id), "%s", s_first_session);
    s_session.refill_in_flight = 0;
    s_session.terminated = 0;
    s_unknown_session = 0;
    snprintf(s_first_id, sizeof(s_first_id), "%s", ids[0]);

    /* radioStarted goes out after the session exists, before the first
     * track starts (the controller queues play-audio/trackStarted after
     * the first PCM block). */
    {
        char token[256];

        if (wave_current_token(token, sizeof(token)) == 0) {
            playback_reporter_enqueue_radio_started(s_first_session, token);
            memset(token, 0, sizeof(token));
        } else {
            logLine("wave: radioStarted skipped, no token\n");
        }
    }

    s_state = 0;
    s_mode = 0;
    if (out_first_id && id_size > 0) {
        snprintf(out_first_id, (size_t)id_size, "%s", s_first_id);
    }
    logLine("wave: first applied %d session ready\n", s_first_count);
    logger_flush();
    return 1;
}

static void wave_consume_refill(void)
{
    if (s_refill_ready &&
        s_refill_result_generation == s_generation && s_session.active) {
        if (s_refill_unknown) {
            /* Server forgot the session: keep playing what is queued, but
             * stop refills and feedback (the server cannot accept them). */
            s_unknown_session = 1;
            s_session.terminated = 1;
            logLine("wave: session unknown, refills stopped\n");
        } else {
            if (s_refill_terminated) {
                s_session.terminated = 1;
            }
            if (s_refill_count > 0) {
                PlaybackQueueInfo info;
                WaveQueueItem flow[WAVE_BATCH_MAX];
                int i;

                for (i = 0; i < s_refill_count; ++i) {
                    snprintf(flow[i].track_id, sizeof(flow[i].track_id),
                             "%s", s_refill_items[i].track.id);
                    flow[i].album_id = s_refill_items[i].track.album_id;
                    snprintf(flow[i].batch_id, sizeof(flow[i].batch_id),
                             "%s", s_refill_batch);
                }
                if (playback_queue_get_info(&info) == 0 &&
                    info.source == PLAYBACK_QUEUE_SOURCE_FLOW) {
                    if (playback_queue_append_flow_items(flow,
                                                         s_refill_count) == 0) {
                        logLine("wave: refilled +%d\n", s_refill_count);
                    }
                } else {
                    logLine("wave: refill dropped (not flow)\n");
                }
            } else if (!s_refill_terminated) {
                logLine("wave: empty batch, backoff\n");
                s_refill_retry_after_us =
                    (u64)sceKernelGetSystemTimeWide() + WAVE_REFILL_RETRY_US;
            }
        }
        logger_flush();
    } else if (s_refill_ready) {
        logLine("wave: stale refill dropped\n");
    }
    s_refill_ready = 0;
    s_refill_count = 0;
    s_refill_unknown = 0;
    s_refill_terminated = 0;
    s_session.refill_in_flight = 0;
}

void wave_service(void)
{
    PlaybackQueueInfo info;
    WaveQueueItem flow_items[WAVE_REFILL_QUEUE_MAX];
    int remaining;
    int item_count;
    int i;

    if (s_tid >= 0 && !wave_worker_done()) {
        return;
    }
    if (s_mode == 2) {
        wave_consume_refill();
        s_mode = 0;
    }
    if (s_mode != 0) {
        return; /* first batch pending for poll_first */
    }
    if (s_tid >= 0) {
        return;
    }
    if (!s_session.active || s_session.terminated || s_unknown_session) {
        return;
    }
    if (s_session.refill_in_flight) {
        return;
    }
    if (playback_queue_get_info(&info) != 0 ||
        info.source != PLAYBACK_QUEUE_SOURCE_FLOW ||
        info.order_count <= 0) {
        return;
    }
    remaining = info.order_count - info.order_pos - 1;
    if (remaining > WAVE_REFILL_AHEAD) {
        return;
    }
    if ((u64)sceKernelGetSystemTimeWide() < s_refill_retry_after_us) {
        return;
    }
    item_count = playback_queue_get_flow_items(flow_items,
                                               WAVE_REFILL_QUEUE_MAX);
    if (item_count <= 0) {
        return;
    }
    snprintf(s_refill_session, sizeof(s_refill_session), "%s",
             s_session.radio_session_id);
    s_refill_queue_count = 0;
    for (i = 0; i < item_count; ++i) {
        if (ym_api_rotor_composite_id(flow_items[i].track_id,
                                      flow_items[i].album_id,
                                      s_refill_queue[i],
                                      sizeof(s_refill_queue[i])) == 0) {
            s_refill_queue_count++;
        } else {
            break;
        }
    }
    if (s_refill_queue_count <= 0) {
        return;
    }
    s_refill_generation = s_generation;
    s_refill_ready = 0;
    s_mode = 2;
    s_session.refill_in_flight = 1;
    wave_launch_worker();
    if (s_tid < 0) {
        s_mode = 0;
        s_session.refill_in_flight = 0;
        s_refill_retry_after_us =
            (u64)sceKernelGetSystemTimeWide() + WAVE_REFILL_RETRY_US;
    }
}

void wave_reset(void)
{
    SceUInt timeout_us;

    s_generation++;
    if (s_generation == 0) {
        s_generation = 1;
    }
    if (s_tid >= 0) {
        timeout_us = 15000000U;
        if (sceKernelWaitThreadEnd(s_tid, &timeout_us) == 0) {
            sceKernelDeleteThread(s_tid);
            s_tid = -1;
        }
    }
    memset(&s_session, 0, sizeof(s_session));
    s_unknown_session = 0;
    memset(s_token, 0, sizeof(s_token));
    s_mode = 0;
    s_state = 0;
    s_first_count = 0;
    s_first_id[0] = '\0';
    s_refill_queue_count = 0;
    s_refill_count = 0;
    s_refill_ready = 0;
    s_refill_retry_after_us = 0;
}

/* Уход с экрана волны (успешный старт -> плеер, или назад): ждём
 * недокачанный first-fetch, но сессию и generation НЕ трогаем —
 * музыка уже может играть из очереди, отчётность жива. Поздний
 * результат воркера никто не применит (poll_first вызывается только
 * активным экраном), следующий старт начнётся с wave_reset. */
void wave_dismiss_screen(void)
{
    SceUInt timeout_us;

    if (s_tid >= 0) {
        timeout_us = 15000000U;
        if (sceKernelWaitThreadEnd(s_tid, &timeout_us) == 0) {
            sceKernelDeleteThread(s_tid);
            s_tid = -1;
        }
    }
}

int wave_is_active(void)
{
    return (s_session.active && !s_unknown_session) ? 1 : 0;
}

unsigned int wave_current_generation(void)
{
    return s_generation;
}

int wave_get_session(WaveSession *out)
{
    if (!out || !s_session.active) {
        return -1;
    }
    *out = s_session;
    return 0;
}

int wave_get_session_id(char *out, size_t out_size)
{
    if (!out || out_size == 0 || !s_session.active ||
        !s_session.radio_session_id[0] || s_unknown_session) {
        return -1;
    }
    snprintf(out, out_size, "%s", s_session.radio_session_id);
    return 0;
}

static void wave_note_terminal(const char *track_id, int album_id,
                               int played_ms, int len_ms, int finished)
{
    WaveQueueItem flow_items[WAVE_REFILL_QUEUE_MAX];
    PlaybackReportContext rep_ctx;
    char session_id[YM_ROTOR_SESSION_ID_SIZE];
    char token[256];
    const char *batch_id = NULL;
    int found_album = album_id;
    int item_count;
    int i;
    double played_s;
    double len_s;

    if (!track_id || !track_id[0]) {
        return;
    }
    if (!s_session.active || !s_session.radio_session_id[0] ||
        s_unknown_session) {
        logLine("wave: note dropped, no session\n");
        return;
    }
    item_count = playback_queue_get_flow_items(flow_items,
                                               WAVE_REFILL_QUEUE_MAX);
    for (i = 0; i < item_count; ++i) {
        if (strcmp(flow_items[i].track_id, track_id) == 0) {
            batch_id = flow_items[i].batch_id;
            found_album = flow_items[i].album_id;
            break;
        }
    }
    if (!batch_id || !batch_id[0]) {
        logLine("wave: note dropped, no batch\n");
        return;
    }
    snprintf(session_id, sizeof(session_id), "%s",
             s_session.radio_session_id);
    playback_report_context_start(&rep_ctx, track_id, found_album, batch_id,
                                  len_ms, s_generation,
                                  PLAYBACK_QUEUE_SOURCE_FLOW);
    if (wave_current_token(token, sizeof(token)) != 0) {
        logLine("wave: note dropped, no token\n");
        return;
    }
    played_s = (played_ms > 0) ? ((double)played_ms / 1000.0) : 0.0;
    len_s = (len_ms > 0) ? ((double)len_ms / 1000.0) : 0.0;
    if (finished) {
        playback_reporter_enqueue_feedback(PLAYBACK_REPORT_TRACK_FINISHED,
                                           &rep_ctx, token, session_id,
                                           played_s, len_s);
    } else {
        playback_reporter_enqueue_feedback(PLAYBACK_REPORT_SKIP,
                                           &rep_ctx, token, session_id,
                                           played_s, len_s);
    }
    memset(token, 0, sizeof(token));
    logLine("wave: note %s queued\n", finished ? "finished" : "skip");
}

void wave_note_skip(const char *track_id, int album_id,
                    int played_ms, int len_ms)
{
    wave_note_terminal(track_id, album_id, played_ms, len_ms, 0);
}

void wave_note_finished(const char *track_id, int album_id,
                        int played_ms, int len_ms)
{
    wave_note_terminal(track_id, album_id, played_ms, len_ms, 1);
}
