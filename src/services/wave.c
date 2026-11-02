/* Rotor sessions: one worker-owned job, one pending request, one active session. */
#include "services/wave.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/net_tls.h"
#include "services/playback_queue.h"
#include "services/playback_reporter.h"
#include "services/token_loader.h"
#include "services/track_meta_store.h"
#include "services/ym_api_rotor.h"

#define WAVE_BATCH_MAX 32
#define WAVE_REFILL_AHEAD 2
#define WAVE_REFILL_QUEUE_MAX 64
#define WAVE_COMPOSITE_SIZE 56
#define WAVE_WORKER_PRIORITY 0x18
#define WAVE_WORKER_STACK (64 * 1024)
#define WAVE_REFILL_RETRY_US 5000000ULL
#define WAVE_REFILL_BACKOFF_MAX_US 60000000ULL

typedef enum { WAVE_JOB_NONE, WAVE_JOB_FIRST, WAVE_JOB_CLONE, WAVE_JOB_REFILL } WaveJobKind;
typedef enum { WAVE_REQUEST_NONE, WAVE_REQUEST_QUEUED,
               WAVE_REQUEST_LOADING, WAVE_REQUEST_READY,
               WAVE_REQUEST_FAILED } WaveRequestState;

typedef struct {
    unsigned int id;
    WaveJobKind kind;
    WaveRequestParams params;
    WaveRequestState state;
    char session_id[YM_ROTOR_SESSION_ID_SIZE];
    TrackRef history[WAVE_REFILL_QUEUE_MAX];
    int history_count;
    int source_generation;
} WaveRequest;

typedef struct {
    WaveJobKind kind;
    unsigned int generation;
    WaveRequestParams params;
    char token[256];
    char session_id[YM_ROTOR_SESSION_ID_SIZE];
    char track_to_start_from[TRACK_ID_SIZE];
    char queue[WAVE_REFILL_QUEUE_MAX][WAVE_COMPOSITE_SIZE];
    int queue_count;
    YmRotorSessionInfo result_session;
    YmRotorSequenceItem items[WAVE_BATCH_MAX];
    int count;
    int result;
} WaveJob;

static SceUID s_tid = -1;
static int s_job_started;
static volatile int s_cancel;
static int s_stopping;
static WaveJob s_job;
static WaveRequest s_request;
static unsigned int s_request_sequence;
static unsigned int s_generation;
static WaveSession s_session;
static WaveRequestParams s_context_params;
static int s_context_known;
static int s_unknown_session;
static u64 s_refill_backoff_us;
static u64 s_refill_retry_after_us;

static unsigned int next_generation(unsigned int current)
{
    current++;
    return current ? current : 1;
}

static u64 wave_refill_backoff_next(void)
{
    u64 cur;
    if (s_refill_backoff_us < WAVE_REFILL_RETRY_US)
        s_refill_backoff_us = WAVE_REFILL_RETRY_US;
    cur = s_refill_backoff_us;
    if (s_refill_backoff_us < WAVE_REFILL_BACKOFF_MAX_US)
        s_refill_backoff_us *= 2;
    if (s_refill_backoff_us > WAVE_REFILL_BACKOFF_MAX_US)
        s_refill_backoff_us = WAVE_REFILL_BACKOFF_MAX_US;
    return (u64)sceKernelGetSystemTimeWide() + cur;
}

/* s_job and its cancel flag remain owned until deletion succeeds. A thread
 * whose start failed is dormant and can be deleted without a STOPPED poll. */
static int wave_worker_done(void)
{
    SceKernelThreadRunStatus status;
    if (s_tid < 0) return 1;
    if (s_job_started) {
        status.size = sizeof(status);
        if (sceKernelReferThreadRunStatus(s_tid, &status) != 0) return -1;
        if (status.status != PSP_THREAD_STOPPED) return 0;
    }
    if (sceKernelDeleteThread(s_tid) < 0) return -1;
    s_tid = -1;
    s_job_started = 0;
    return 1;
}

static int wave_worker(SceSize args, void *argp)
{
    YmApiContext ctx;
    const char *values[WAVE_REFILL_QUEUE_MAX];
    int i;
    (void)args;
    (void)argp;

    net_tls_cancel_bind(&s_cancel);
    s_job.result = -1;
    if (s_cancel || token_loader_read(s_job.token, sizeof(s_job.token)) != 0)
        goto done;
    ctx.oauth_token = s_job.token;
    ctx.timeout_ms = 0;
    if (s_job.kind == WAVE_JOB_FIRST) {
        for (i = 0; i < s_job.params.seed_count; i++)
            values[i] = s_job.params.seeds[i];
        s_job.result = ym_api_rotor_session_new(
            &ctx, values, s_job.params.seed_count,
            s_job.params.include_wave_model, &s_job.result_session,
            s_job.items, WAVE_BATCH_MAX, &s_job.count);
    } else {
        for (i = 0; i < s_job.queue_count; i++)
            values[i] = s_job.queue[i];
        if (s_job.kind == WAVE_JOB_CLONE) {
            s_job.result = ym_api_rotor_session_clone(
                &ctx, s_job.session_id, values, s_job.queue_count,
                s_job.track_to_start_from, &s_job.result_session,
                s_job.items, WAVE_BATCH_MAX, &s_job.count);
        } else {
            s_job.result = ym_api_rotor_session_tracks(
                &ctx, s_job.session_id, values, s_job.queue_count,
                &s_job.result_session, s_job.items, WAVE_BATCH_MAX, &s_job.count);
        }
    }
    if (s_job.result == 0) {
        for (i = 0; i < s_job.count && !s_cancel; i++) {
            TrackRef ref;
            memset(&ref, 0, sizeof(ref));
            snprintf(ref.id, sizeof(ref.id), "%s", s_job.items[i].track.id);
            ref.album_id = s_job.items[i].track.album_id;
            track_meta_store_put(&ref, &s_job.items[i].track, 0);
        }
    }
done:
    memset(s_job.token, 0, sizeof(s_job.token));
    net_tls_cancel_unbind();
    return 0;
}

static void wave_launch_worker(void)
{
    s_cancel = 0;
    s_job.result = -1;
    s_job_started = 0;
    s_tid = sceKernelCreateThread("wave_worker", wave_worker,
                                  WAVE_WORKER_PRIORITY,
                                  WAVE_WORKER_STACK, 0, NULL);
    if (s_tid < 0) goto fail;
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        if (sceKernelDeleteThread(s_tid) >= 0) s_tid = -1;
        goto fail;
    }
    s_job_started = 1;
    return;
fail:
    if (s_job.kind == s_request.kind && s_job.generation == s_request.id)
        s_request.state = WAVE_REQUEST_FAILED;
}

static int params_valid(const WaveRequestParams *params)
{
    int i;
    if (!params || params->seed_count < 0 ||
        params->seed_count > YM_ROTOR_SEEDS_MAX) return 0;
    for (i = 0; i < params->seed_count; i++)
        if (!params->seeds[i][0] ||
            !memchr(params->seeds[i], '\0', YM_ROTOR_SEED_SIZE)) return 0;
    return 1;
}

static int context_equal(const WaveRequestParams *a, const WaveRequestParams *b)
{
    int used[WAVE_SEEDS_MAX] = {0};
    int count_a = a->seed_count ? a->seed_count : 1;
    int count_b = b->seed_count ? b->seed_count : 1;
    int i, j;
    if (count_a != count_b) return 0;
    for (i = 0; i < count_a; i++) {
        const char *seed_a = a->seed_count ? a->seeds[i] : YM_ROTOR_DEFAULT_SEED;
        for (j = 0; j < count_b; j++) {
            const char *seed_b = b->seed_count ? b->seeds[j] : YM_ROTOR_DEFAULT_SEED;
            if (!used[j] && strcmp(seed_a, seed_b) == 0) {
                used[j] = 1;
                break;
            }
        }
        if (j == count_b) return 0;
    }
    return 1;
}

static int current_context_matches(const WaveRequestParams *params)
{
    PlaybackQueueInfo queue;
    /* Termination/unknown-session stop network refills, not access to the
     * current queue. Opening its player does not resume the Rotor session. */
    return s_context_known && s_session.active &&
           context_equal(&s_context_params, params) &&
           playback_queue_get_info(&queue) == 0 &&
           queue.source == PLAYBACK_QUEUE_SOURCE_FLOW &&
           queue.source_generation == (int)s_session.generation &&
           queue.count > 0 && queue.current_index >= 0 &&
           queue.current_index < queue.count;
}

WavePrepareResult wave_prepare(const WaveRequestParams *params)
{
    WavePrepareResult result = { WAVE_PREPARE_FAILED, 0 };
    if (s_stopping || !params_valid(params)) return result;
    if (current_context_matches(params)) {
        wave_cancel(s_request.id);
        result.status = WAVE_PREPARE_EXISTING;
        return result;
    }
    result.status = WAVE_PREPARE_PENDING;
    if (s_request.kind == WAVE_JOB_FIRST && s_request.state != WAVE_REQUEST_NONE &&
        s_request.state != WAVE_REQUEST_FAILED &&
        context_equal(&s_request.params, params)) {
        result.request_id = s_request.id;
        return result;
    }

    /* The successor has separate storage even while its predecessor exits. */
    if (s_job.kind != WAVE_JOB_NONE) s_cancel = 1;
    s_request_sequence = next_generation(s_request_sequence);
    s_request.id = s_request_sequence;
    s_request.kind = WAVE_JOB_FIRST;
    s_request.params = *params;
    s_request.state = WAVE_REQUEST_QUEUED;
    result.request_id = s_request.id;
    return result;
}

WavePrepareResult wave_prepare_restore(void)
{
    WavePrepareResult result = { WAVE_PREPARE_FAILED, 0 };
    PlaybackQueueInfo queue;
    char session_id[YM_ROTOR_SESSION_ID_SIZE];
    if (s_stopping || playback_queue_get_info(&queue) != 0 ||
        queue.source != PLAYBACK_QUEUE_SOURCE_FLOW || queue.count <= 0 ||
        playback_queue_get_flow_session_id(session_id, sizeof(session_id)) != 0 ||
        !session_id[0]) return result;
    if (s_request.kind == WAVE_JOB_CLONE &&
        s_request.state != WAVE_REQUEST_NONE && s_request.state != WAVE_REQUEST_FAILED &&
        s_request.source_generation == queue.source_generation &&
        strcmp(s_request.session_id, session_id) == 0) {
        result.status = WAVE_PREPARE_PENDING;
        result.request_id = s_request.id;
        return result;
    }
    if (s_job.kind != WAVE_JOB_NONE) s_cancel = 1;
    memset(&s_request, 0, sizeof(s_request));
    s_request.history_count = playback_queue_get_history_refs(
        s_request.history, WAVE_REFILL_QUEUE_MAX);
    if (s_request.history_count <= 0) return result;
    s_request_sequence = next_generation(s_request_sequence);
    s_request.id = s_request_sequence;
    s_request.kind = WAVE_JOB_CLONE;
    s_request.state = WAVE_REQUEST_QUEUED;
    s_request.source_generation = queue.source_generation;
    snprintf(s_request.session_id, sizeof(s_request.session_id), "%s", session_id);
    result.status = WAVE_PREPARE_PENDING;
    result.request_id = s_request.id;
    return result;
}

void wave_cancel(unsigned int request_id)
{
    if (!request_id || request_id != s_request.id) return;
    if ((s_job.kind == WAVE_JOB_FIRST || s_job.kind == WAVE_JOB_CLONE) &&
        s_job.generation == request_id)
        s_cancel = 1;
    memset(&s_request, 0, sizeof(s_request));
}

int wave_poll(unsigned int request_id, TrackEntry *first)
{
    if (!request_id || request_id != s_request.id ||
        s_request.state == WAVE_REQUEST_NONE ||
        s_request.state == WAVE_REQUEST_FAILED) return -1;
    if (s_request.state != WAVE_REQUEST_READY) return 0;
    if (first) *first = s_job.items[0].track;
    return 1;
}

int wave_commit(unsigned int request_id, TrackEntry *first)
{
    TrackRef refs[WAVE_REFILL_QUEUE_MAX + WAVE_BATCH_MAX];
    unsigned int generation;
    int i, prefix = 0;
    if (wave_poll(request_id, NULL) != 1 || s_tid >= 0 ||
        s_job.kind != s_request.kind || s_job.generation != request_id)
        return -1;
    if (s_job.kind == WAVE_JOB_CLONE) {
        PlaybackQueueInfo queue;
        WaveQueueItem current;
        if (playback_queue_get_info(&queue) != 0 ||
            queue.source != PLAYBACK_QUEUE_SOURCE_FLOW ||
            queue.source_generation != s_request.source_generation ||
            playback_queue_get_current_flow(&current) != 0 ||
            strcmp(current.track_id, s_job.track_to_start_from) != 0 ||
            strcmp(s_job.items[0].track.id, s_job.track_to_start_from) != 0)
            return -1;
        prefix = s_request.history_count - 1;
        for (i = 0; i < prefix; ++i) {
            snprintf(refs[i].id, sizeof(refs[i].id), "%s", s_request.history[i].id);
            refs[i].album_id = s_request.history[i].album_id;
        }
    }
    generation = next_generation(s_generation);
    for (i = 0; i < s_job.count; i++) {
        snprintf(refs[prefix + i].id, sizeof(refs[prefix + i].id), "%s", s_job.items[i].track.id);
        refs[prefix + i].album_id = s_job.items[i].track.album_id;
    }
    PlaybackQueueDraft *draft = playback_queue_prepare(refs, prefix + s_job.count, prefix,
                                    PLAYBACK_QUEUE_SOURCE_FLOW,
                                    0, (int)generation);
    if (!draft) {
        s_request.state = WAVE_REQUEST_FAILED;
        return -1;
    }
    playback_queue_commit(draft);
    for (i = 0; i < s_job.count; i++)
        playback_queue_set_flow_meta(prefix + i, s_job.items[i].track.album_id,
                                     s_job.result_session.batch_id);
    if (playback_queue_set_flow_session_id(
            s_job.result_session.radio_session_id) != 0) {
        s_request.state = WAVE_REQUEST_FAILED;
        return -1;
    }
    memset(&s_session, 0, sizeof(s_session));
    s_generation = generation;
    s_session.active = 1;
    s_session.generation = generation;
    s_context_params = s_job.kind == WAVE_JOB_CLONE
        ? s_job.result_session.context : s_request.params;
    s_context_known = 1;
    snprintf(s_session.radio_session_id, sizeof(s_session.radio_session_id),
             "%s", s_job.result_session.radio_session_id);
    s_unknown_session = 0;
    s_refill_backoff_us = 0;
    s_refill_retry_after_us = 0;
    if (first) *first = s_job.items[0].track;
    logLine("wave: request=%u committed count=%d generation=%u\n",
            request_id, s_job.count, generation);
    memset(&s_request, 0, sizeof(s_request));
    memset(&s_job, 0, sizeof(s_job));
    return 0;
}

static void wave_consume_refill(void)
{
    PlaybackQueueInfo info;
    WaveQueueItem flow[WAVE_BATCH_MAX];
    int i;
    if (s_cancel || s_job.generation != s_generation || !s_session.active ||
        playback_queue_get_info(&info) != 0 ||
        info.source != PLAYBACK_QUEUE_SOURCE_FLOW ||
        info.source_generation != (int)s_generation) return;
    if (s_job.result_session.unknown_session) {
        s_unknown_session = 1;
        s_session.terminated = 1;
    } else if (s_job.result != 0) {
        s_refill_retry_after_us = wave_refill_backoff_next();
    } else {
        s_session.terminated = s_job.result_session.terminated;
        for (i = 0; i < s_job.count; i++) {
            snprintf(flow[i].track_id, sizeof(flow[i].track_id), "%s",
                     s_job.items[i].track.id);
            flow[i].album_id = s_job.items[i].track.album_id;
            snprintf(flow[i].batch_id, sizeof(flow[i].batch_id), "%s",
                     s_job.result_session.batch_id);
        }
        if (s_job.count > 0 &&
            playback_queue_append_flow_items(flow, s_job.count) == 0) {
            s_refill_backoff_us = 0;
            s_refill_retry_after_us = 0;
        } else if (!s_session.terminated) {
            s_refill_retry_after_us = wave_refill_backoff_next();
        }
    }
}

void wave_service(int refill_enabled)
{
    PlaybackQueueInfo info;
    WaveQueueItem flow[WAVE_REFILL_QUEUE_MAX];
    int count;
    int i;
    int done;
    if (s_stopping) return;
    done = wave_worker_done();
    if (done <= 0) {
        if (done < 0 && s_request.state != WAVE_REQUEST_NONE)
            s_request.state = WAVE_REQUEST_FAILED;
        return;
    }

    if (s_job.kind == WAVE_JOB_FIRST || s_job.kind == WAVE_JOB_CLONE) {
        if (!s_cancel && s_job.generation == s_request.id &&
            s_request.state == WAVE_REQUEST_LOADING) {
            s_request.state = s_job.result == 0 && s_job.count > 0
                ? WAVE_REQUEST_READY : WAVE_REQUEST_FAILED;
        }
        if (!s_cancel && s_job.generation == s_request.id &&
            s_request.state == WAVE_REQUEST_READY) return;
        memset(&s_job, 0, sizeof(s_job));
    } else if (s_job.kind == WAVE_JOB_REFILL) {
        wave_consume_refill();
        s_session.refill_in_flight = 0;
        memset(&s_job, 0, sizeof(s_job));
    }

    if (s_request.state == WAVE_REQUEST_QUEUED) {
        s_job.kind = s_request.kind;
        s_job.generation = s_request.id;
        s_job.params = s_request.params;
        if (s_job.kind == WAVE_JOB_CLONE) {
            snprintf(s_job.session_id, sizeof(s_job.session_id), "%s", s_request.session_id);
            snprintf(s_job.track_to_start_from, sizeof(s_job.track_to_start_from), "%s",
                     s_request.history[s_request.history_count - 1].id);
            for (i = 0; i < s_request.history_count; ++i) {
                if (ym_api_rotor_composite_id(s_request.history[i].id,
                                              s_request.history[i].album_id,
                                              s_job.queue[i], sizeof(s_job.queue[i])) != 0) {
                    memset(&s_job, 0, sizeof(s_job));
                    s_request.state = WAVE_REQUEST_FAILED;
                    return;
                }
                s_job.queue_count++;
            }
        }
        s_request.state = WAVE_REQUEST_LOADING;
        wave_launch_worker();
        return;
    }
    if (s_request.state != WAVE_REQUEST_NONE || !refill_enabled) return;
    if (!s_session.active || s_session.terminated || s_unknown_session ||
        (u64)sceKernelGetSystemTimeWide() < s_refill_retry_after_us) return;
    if (playback_queue_get_info(&info) != 0 ||
        info.source != PLAYBACK_QUEUE_SOURCE_FLOW ||
        info.source_generation != (int)s_generation || info.order_count <= 0 ||
        info.order_count - info.order_pos - 1 > WAVE_REFILL_AHEAD) return;
    count = playback_queue_get_flow_items(flow, WAVE_REFILL_QUEUE_MAX);
    if (count <= 0) return;
    s_job.kind = WAVE_JOB_REFILL;
    s_job.generation = s_generation;
    snprintf(s_job.session_id, sizeof(s_job.session_id), "%s",
             s_session.radio_session_id);
    for (i = 0; i < count; i++) {
        if (ym_api_rotor_composite_id(flow[i].track_id, flow[i].album_id,
                                      s_job.queue[i], sizeof(s_job.queue[i])) != 0)
            break;
        s_job.queue_count++;
    }
    if (!s_job.queue_count) {
        memset(&s_job, 0, sizeof(s_job));
        return;
    }
    s_session.refill_in_flight = 1;
    wave_launch_worker();
}

int wave_quiesce(void)
{
    SceKernelThreadRunStatus status;
    s_stopping = 1;
    s_cancel = 1;
    if (s_tid >= 0 && s_job_started) {
        status.size = sizeof(status);
        if (sceKernelReferThreadRunStatus(s_tid, &status) != 0 ||
            status.status != PSP_THREAD_STOPPED) {
            logLine("wave: worker still active; resources retained\n");
            return -1;
        }
    }
    return 0;
}

int wave_is_active(void)
{
    return s_session.active && !s_unknown_session;
}

void wave_report_started(unsigned int generation, const char *token)
{
    if (wave_is_active() && generation == s_session.generation &&
        !s_session.radio_started && token && token[0] &&
        playback_reporter_enqueue_radio_started(s_session.radio_session_id, token) == 0)
        s_session.radio_started = 1;
}

unsigned int wave_current_generation(void)
{
    return s_generation;
}

int wave_get_session(WaveSession *out)
{
    if (!out || !s_session.active) return -1;
    *out = s_session;
    return 0;
}

int wave_get_session_id(char *out, size_t out_size)
{
    if (!out || !out_size || !wave_is_active() ||
        !s_session.radio_session_id[0]) return -1;
    snprintf(out, out_size, "%s", s_session.radio_session_id);
    return 0;
}
