/* Playback reporter worker. See playback_reporter.h for the contract. */
#include "services/playback_reporter.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/ym_api_rotor.h"
#include "services/ym_api_types.h"

#define REPORTER_QUEUE_CAP 32
#define REPORTER_THREAD_PRIORITY 0x18
#define REPORTER_STACK_SIZE (64 * 1024)
#define REPORTER_MAX_ATTEMPTS 4 /* first try + 3 retries */

typedef struct {
    int used;
    PlaybackReportType type;
    char token[256];
    int uid;
    char radio_session_id[YM_ROTOR_SESSION_ID_SIZE];
    char batch_id[YM_ROTOR_BATCH_ID_SIZE];
    char track_id[40];
    int album_id;
    char play_id[40];
    double total_played_s;
    double track_len_s;
    char timestamp[YM_ROTOR_TIMESTAMP_SIZE];
    char client_now[YM_ROTOR_TIMESTAMP_SIZE];
} ReporterEvent;

static ReporterEvent s_events[REPORTER_QUEUE_CAP];
static int s_head = 0;
static int s_tail = 0;
static int s_count = 0;
static SceLwMutexWorkarea s_mutex;
static int s_mutex_initialized = 0;
static SceUID s_sema = -1;
static SceUID s_thread = -1;
static volatile int s_running = 0;
static unsigned int s_play_counter = 0;
static int s_cached_uid = 0;
static char s_cached_token[256];

static void reporter_lock(void)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_mutex, 1, NULL);
    }
}

static void reporter_unlock(void)
{
    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_mutex, 1);
    }
}

void playback_report_context_start(PlaybackReportContext *ctx,
                                   const char *track_id, int album_id,
                                   const char *batch_id,
                                   int duration_ms,
                                   unsigned int generation,
                                   PlaybackQueueSource source)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    if (track_id && track_id[0]) {
        snprintf(ctx->track_id, sizeof(ctx->track_id), "%s", track_id);
    }
    ctx->album_id = album_id;
    if (batch_id && batch_id[0]) {
        snprintf(ctx->batch_id, sizeof(ctx->batch_id), "%s", batch_id);
    }
    ctx->duration_ms = duration_ms;
    ctx->generation = generation;
    ctx->source = source;
    ctx->active = (source == PLAYBACK_QUEUE_SOURCE_FLOW) ? 1 : 0;
}

int playback_reporter_new_play_id(char *out, size_t out_size)
{
    u64 now_us;
    unsigned int counter;
    int written;

    if (!out || out_size == 0) {
        return -1;
    }
    now_us = (u64)sceKernelGetSystemTimeWide();
    reporter_lock();
    s_play_counter++;
    counter = s_play_counter;
    reporter_unlock();
    written = snprintf(out, out_size, "%08X-%04X",
                       (unsigned int)(now_us & 0xFFFFFFFFu),
                       (unsigned int)(counter & 0xFFFFu));
    if (written <= 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

/* Copy a fully-formed event into the bounded FIFO. Never blocks on the
 * network and never allocates. On overflow the newest event is dropped
 * (the player must never stall for telemetry). */
static int reporter_push(const ReporterEvent *event)
{
    int rc = -1;

    if (!event) {
        return -1;
    }
    reporter_lock();
    if (s_count < REPORTER_QUEUE_CAP) {
        s_events[s_tail] = *event;
        s_events[s_tail].used = 1;
        s_tail = (s_tail + 1) % REPORTER_QUEUE_CAP;
        s_count++;
        rc = 0;
    }
    reporter_unlock();
    if (rc == 0) {
        if (s_sema >= 0) {
            sceKernelSignalSema(s_sema, 1);
        }
    } else {
        logLine("reporter: queue full, event dropped type=%d\n",
                (int)event->type);
    }
    return rc;
}

static int reporter_fill_common(ReporterEvent *event,
                                const PlaybackReportContext *ctx,
                                const char *token, int uid)
{
    if (!event || !ctx || !token || !token[0] || !ctx->track_id[0]) {
        return -1;
    }
    memset(event, 0, sizeof(*event));
    snprintf(event->token, sizeof(event->token), "%s", token);
    event->uid = uid;
    snprintf(event->track_id, sizeof(event->track_id), "%s", ctx->track_id);
    event->album_id = ctx->album_id;
    snprintf(event->batch_id, sizeof(event->batch_id), "%s", ctx->batch_id);
    snprintf(event->play_id, sizeof(event->play_id), "%s", ctx->play_id);
    event->track_len_s = (ctx->duration_ms > 0)
        ? ((double)ctx->duration_ms / 1000.0) : 0.0;
    return 0;
}

int playback_reporter_enqueue_play_audio(const PlaybackReportContext *ctx,
                                         const char *token, int uid)
{
    ReporterEvent event;

    if (reporter_fill_common(&event, ctx, token, uid) != 0) {
        return -1;
    }
    if (!event.play_id[0]) {
        return -1;
    }
    event.type = PLAYBACK_REPORT_PLAY_AUDIO;
    if (ym_api_rotor_timestamp_utc(event.timestamp, sizeof(event.timestamp)) < 0) {
        return -1;
    }
    snprintf(event.client_now, sizeof(event.client_now), "%s", event.timestamp);
    return reporter_push(&event);
}

int playback_reporter_enqueue_feedback(PlaybackReportType type,
                                       const PlaybackReportContext *ctx,
                                       const char *token,
                                       const char *radio_session_id,
                                       double total_played_s,
                                       double track_len_s)
{
    ReporterEvent event;

    if (type == PLAYBACK_REPORT_PLAY_AUDIO ||
        type == PLAYBACK_REPORT_RADIO_STARTED) {
        return -1;
    }
    if (!radio_session_id || !radio_session_id[0]) {
        return -1;
    }
    if (reporter_fill_common(&event, ctx, token, 0) != 0) {
        return -1;
    }
    if (!event.batch_id[0]) {
        return -1;
    }
    event.type = type;
    snprintf(event.radio_session_id, sizeof(event.radio_session_id),
             "%s", radio_session_id);
    event.total_played_s = (total_played_s > 0.0) ? total_played_s : 0.0;
    if (track_len_s > 0.0) {
        event.track_len_s = track_len_s;
    }
    return reporter_push(&event);
}

int playback_reporter_enqueue_radio_started(const char *radio_session_id,
                                            const char *token)
{
    ReporterEvent event;

    if (!radio_session_id || !radio_session_id[0] || !token || !token[0]) {
        return -1;
    }
    memset(&event, 0, sizeof(event));
    event.type = PLAYBACK_REPORT_RADIO_STARTED;
    snprintf(event.token, sizeof(event.token), "%s", token);
    snprintf(event.radio_session_id, sizeof(event.radio_session_id),
             "%s", radio_session_id);
    return reporter_push(&event);
}

/* Resolve uid for events queued with uid <= 0. The value is cached while
 * the token is unchanged; the token itself is never logged. */
static int reporter_resolve_uid(const char *token)
{
    YmApiContext api_ctx;
    int uid = 0;

    reporter_lock();
    if (s_cached_uid > 0 && strcmp(s_cached_token, token) == 0) {
        uid = s_cached_uid;
        reporter_unlock();
        return uid;
    }
    reporter_unlock();

    api_ctx.oauth_token = token;
    api_ctx.timeout_ms = 0;
    if (ym_api_rotor_resolve_uid(&api_ctx, &uid) != 0 || uid <= 0) {
        logLine("reporter: uid resolve failed\n");
        return -1;
    }
    reporter_lock();
    s_cached_uid = uid;
    snprintf(s_cached_token, sizeof(s_cached_token), "%s", token);
    reporter_unlock();
    return uid;
}

static int reporter_send_once(const ReporterEvent *event)
{
    YmApiContext api_ctx;
    int uid = event->uid;

    api_ctx.oauth_token = event->token;
    api_ctx.timeout_ms = 0;

    switch (event->type) {
        case PLAYBACK_REPORT_PLAY_AUDIO:
            if (uid <= 0) {
                uid = reporter_resolve_uid(event->token);
                if (uid <= 0) {
                    return -1;
                }
            }
            return ym_api_rotor_play_audio(&api_ctx, uid,
                                           event->track_id, event->album_id,
                                           event->play_id, 0,
                                           event->timestamp, event->client_now,
                                           event->track_len_s);
        case PLAYBACK_REPORT_RADIO_STARTED:
            return ym_api_rotor_feedback_radio_started(&api_ctx,
                                                       event->radio_session_id);
        case PLAYBACK_REPORT_TRACK_STARTED:
            return ym_api_rotor_feedback_track_started(&api_ctx,
                                                       event->radio_session_id,
                                                       event->batch_id,
                                                       event->track_id,
                                                       event->album_id,
                                                       event->track_len_s);
        case PLAYBACK_REPORT_TRACK_FINISHED:
            return ym_api_rotor_feedback_track_finished(&api_ctx,
                                                        event->radio_session_id,
                                                        event->batch_id,
                                                        event->track_id,
                                                        event->album_id,
                                                        event->total_played_s,
                                                        event->track_len_s);
        case PLAYBACK_REPORT_SKIP:
            return ym_api_rotor_feedback_skip(&api_ctx,
                                              event->radio_session_id,
                                              event->batch_id,
                                              event->track_id,
                                              event->album_id,
                                              event->total_played_s,
                                              event->track_len_s);
        case PLAYBACK_REPORT_LIKE:
            return ym_api_rotor_feedback_like(&api_ctx,
                                              event->radio_session_id,
                                              event->batch_id,
                                              event->track_id,
                                              event->album_id,
                                              event->track_len_s);
        case PLAYBACK_REPORT_UNLIKE:
            return ym_api_rotor_feedback_unlike(&api_ctx,
                                                event->radio_session_id,
                                                event->batch_id,
                                                event->track_id,
                                                event->album_id,
                                                event->track_len_s);
        default:
            return -1;
    }
}

static int reporter_worker(SceSize args, void *argp)
{
    static const SceUInt backoff_us[REPORTER_MAX_ATTEMPTS] = {0, 1000000u, 2000000u, 4000000u};

    (void)args;
    (void)argp;
    logLine("reporter: worker started\n");

    while (s_running) {
        ReporterEvent event;
        int attempt;
        int send_rc = -1;

        if (sceKernelWaitSema(s_sema, 1, NULL) < 0) {
            break;
        }
        if (!s_running) {
            break;
        }

        reporter_lock();
        if (s_count <= 0) {
            reporter_unlock();
            continue;
        }
        event = s_events[s_head];
        s_events[s_head].used = 0;
        memset(s_events[s_head].token, 0, sizeof(s_events[s_head].token));
        s_head = (s_head + 1) % REPORTER_QUEUE_CAP;
        s_count--;
        reporter_unlock();

        /* YM_ROTOR_ERR_RETRY (transport/408/429/5xx) backs off on the worker
         * only; YM_ROTOR_ERR is final. Order is preserved: the head event
         * completes (or exhausts attempts) before the next is sent. */
        for (attempt = 0; attempt < REPORTER_MAX_ATTEMPTS; ++attempt) {
            if (attempt > 0) {
                sceKernelDelayThread(backoff_us[attempt]);
                if (!s_running) {
                    break;
                }
            }
            send_rc = reporter_send_once(&event);
            if (send_rc == YM_ROTOR_OK || send_rc == YM_ROTOR_ERR) {
                break;
            }
            /* Transient: log coarsely and back off. */
            logLine("reporter: send retryable type=%d attempt=%d\n",
                    (int)event.type, attempt);
        }
        if (send_rc == 0) {
            logLine("reporter: sent type=%d track='%s'\n",
                    (int)event.type, event.track_id);
        } else {
            logLine("reporter: dropped type=%d track='%s'\n",
                    (int)event.type, event.track_id);
        }
        logger_flush();
        memset(event.token, 0, sizeof(event.token));
    }

    logLine("reporter: worker stopped\n");
    return 0;
}

void playback_reporter_init(void)
{
    if (s_running) {
        return;
    }
    memset(s_events, 0, sizeof(s_events));
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_cached_uid = 0;
    s_cached_token[0] = '\0';

    if (sceKernelCreateLwMutex(&s_mutex, "reporter", 0, 0, NULL) >= 0) {
        s_mutex_initialized = 1;
    }
    s_sema = sceKernelCreateSema("reporter", 0, 0, REPORTER_QUEUE_CAP, NULL);
    if (s_sema < 0) {
        return;
    }
    s_running = 1;
    s_thread = sceKernelCreateThread("reporter_worker", reporter_worker,
                                     REPORTER_THREAD_PRIORITY,
                                     REPORTER_STACK_SIZE, 0, NULL);
    if (s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0) {
        s_running = 0;
        if (s_thread >= 0) {
            sceKernelDeleteThread(s_thread);
            s_thread = -1;
        }
        sceKernelDeleteSema(s_sema);
        s_sema = -1;
        return;
    }
    logLine("reporter: init\n");
}

int playback_reporter_quiesce(void)
{
    if (!s_running && s_thread < 0) {
        return 0;
    }
    s_running = 0;
    if (s_sema >= 0) {
        sceKernelSignalSema(s_sema, 1);
    }
    if (s_thread >= 0) {
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_thread, &timeout_us) < 0) {
            logLine("reporter: worker still active; resources retained\n");
            return -1;
        }
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    return 0;
}

void playback_reporter_shutdown(void)
{
    if (playback_reporter_quiesce() < 0) {
        return;
    }
    if (s_sema >= 0) {
        sceKernelDeleteSema(s_sema);
        s_sema = -1;
    }
    if (s_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_mutex);
        s_mutex_initialized = 0;
    }
    reporter_lock();
    memset(s_events, 0, sizeof(s_events));
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_cached_uid = 0;
    memset(s_cached_token, 0, sizeof(s_cached_token));
    reporter_unlock();
}
