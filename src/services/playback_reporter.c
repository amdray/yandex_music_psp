/* Playback reporter worker. See playback_reporter.h for the contract. */
#include "services/playback_reporter.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <time.h>

#include "core/fs.h"
#include "core/logger.h"
#include "core/time_format.h"
#include "services/ym_api_rotor.h"
#include "services/ym_api_types.h"

#define REPORTER_THREAD_PRIORITY 0x18
#define REPORTER_STACK_SIZE (64 * 1024)
#define REPORTER_MAX_ATTEMPTS 4 /* first try + 3 retries */

typedef struct {
    int store_only;
    PlaybackReportType type;
    char token[256];
    int uid;
    char radio_session_id[YM_ROTOR_SESSION_ID_SIZE];
    char batch_id[YM_ROTOR_BATCH_ID_SIZE];
    char track_id[40];
    int album_id;
    char play_id[40];
    int total_played_ms;
    int position_ms;
    int track_len_ms;
    char timestamp[YM_ROTOR_TIMESTAMP_SIZE];
    char client_now[YM_ROTOR_TIMESTAMP_SIZE];
} ReporterEvent;

typedef struct ReporterNode {
    ReporterEvent event;
    struct ReporterNode *next;
} ReporterNode;

static ReporterNode *s_head;
static ReporterNode *s_tail;
static SceLwMutexWorkarea s_mutex;
static int s_mutex_initialized = 0;
static SceUID s_sema = -1;
static SceUID s_thread = -1;
static volatile int s_running = 0;
static unsigned int s_play_counter = 0;
static int s_cached_uid = 0;
static unsigned int s_account_revision;
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

int playback_report_context_start(PlaybackReportContext *ctx,
                                   const char *track_id, int album_id,
                                   const char *batch_id,
                                   int duration_ms,
                                   unsigned int generation,
                                   PlaybackQueueSource source,
                                   const char *token)
{
    if (!ctx || !token || !token[0]) return -1;
    memset(ctx, 0, sizeof(*ctx));
    reporter_lock();
    if (strcmp(token, s_cached_token) == 0) ctx->uid = s_cached_uid;
    reporter_unlock();
    if (ctx->uid <= 0) return -1;
    snprintf(ctx->token, sizeof(ctx->token), "%s", token);
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
    return 0;
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
    written = snprintf(out, out_size, "%08X-%016llX-%08X",
                       (unsigned int)time(NULL),
                       (unsigned long long)now_us, counter);
    if (written <= 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static int reporter_push(const ReporterEvent *event)
{
    ReporterNode *node;
    if (!event || !s_running) return -1;
    node = malloc(sizeof(*node));
    if (!node) {
        logLine("reporter: allocation failed type=%d\n", (int)event->type);
        return -1;
    }
    node->event = *event;
    node->next = NULL;
    reporter_lock();
    if (!s_running) {
        reporter_unlock();
        free(node);
        return -1;
    }
    if (s_tail) s_tail->next = node;
    else s_head = node;
    s_tail = node;
    reporter_unlock();
    sceKernelSignalSema(s_sema, 1);
    return 0;
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
    event->track_len_ms = (ctx->duration_ms > 0)
        ? ctx->duration_ms : 0;
    return 0;
}

int playback_reporter_enqueue_play_audio(const PlaybackReportContext *ctx,
                                         int total_played_ms, int position_ms,
                                         int store_only)
{
    ReporterEvent event;

    if (!ctx || ctx->uid <= 0 ||
        reporter_fill_common(&event, ctx, ctx->token, ctx->uid) != 0) {
        return -1;
    }
    if (!event.play_id[0]) {
        return -1;
    }
    event.type = PLAYBACK_REPORT_PLAY_AUDIO;
    event.store_only = store_only;
    event.total_played_ms = total_played_ms;
    event.position_ms = position_ms;
    if (ym_time_format(event.timestamp, sizeof(event.timestamp), YM_TIME_UTC) < 0) {
        return -1;
    }
    snprintf(event.client_now, sizeof(event.client_now), "%s", event.timestamp);
    return reporter_push(&event);
}

int playback_reporter_enqueue_feedback(PlaybackReportType type,
                                       const PlaybackReportContext *ctx,
                                       const char *token,
                                       const char *radio_session_id,
                                       int total_played_ms,
                                       int track_len_ms)
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
    event.total_played_ms = (total_played_ms > 0) ? total_played_ms : 0;
    if (track_len_ms > 0) {
        event.track_len_ms = track_len_ms;
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

static int reporter_send_once(const ReporterEvent *event)
{
    YmApiContext api_ctx;
    int uid = event->uid;

    api_ctx.oauth_token = event->token;
    api_ctx.timeout_ms = 0;

    switch (event->type) {
        case PLAYBACK_REPORT_PLAY_AUDIO:
            return ym_api_rotor_play_audio(&api_ctx, uid,
                                           event->track_id, event->album_id,
                                           event->play_id, 0,
                                           event->timestamp, event->client_now,
                                           event->track_len_ms, event->total_played_ms,
                                           event->position_ms);
        case PLAYBACK_REPORT_RADIO_STARTED:
            return ym_api_rotor_feedback_radio_started(&api_ctx,
                                                       event->radio_session_id);
        case PLAYBACK_REPORT_TRACK_STARTED:
            return ym_api_rotor_feedback_track_started(&api_ctx,
                                                       event->radio_session_id,
                                                       event->batch_id,
                                                       event->track_id,
                                                       event->album_id,
                                                       event->track_len_ms);
        case PLAYBACK_REPORT_TRACK_FINISHED:
            return ym_api_rotor_feedback_track_finished(&api_ctx,
                                                        event->radio_session_id,
                                                        event->batch_id,
                                                        event->track_id,
                                                        event->album_id,
                                                        event->total_played_ms,
                                                        event->track_len_ms);
        case PLAYBACK_REPORT_SKIP:
            return ym_api_rotor_feedback_skip(&api_ctx,
                                              event->radio_session_id,
                                              event->batch_id,
                                              event->track_id,
                                              event->album_id,
                                              event->total_played_ms,
                                              event->track_len_ms);
        case PLAYBACK_REPORT_LIKE:
            return ym_api_rotor_feedback_like(&api_ctx,
                                              event->radio_session_id,
                                              event->batch_id,
                                              event->track_id,
                                              event->album_id,
                                              event->track_len_ms);
        case PLAYBACK_REPORT_UNLIKE:
            return ym_api_rotor_feedback_unlike(&api_ctx,
                                                event->radio_session_id,
                                                event->batch_id,
                                                event->track_id,
                                                event->album_id,
                                                event->track_len_ms);
        default:
            return -1;
    }
}

/* The journal contains no credentials. Files are separated by account uid. */
typedef struct {
    char track_id[40];
    int album_id;
    char play_id[40];
    int total_played_ms;
    int position_ms;
    int track_len_ms;
    char timestamp[YM_ROTOR_TIMESTAMP_SIZE];
} PlayRecord;

typedef struct {
    unsigned int magic;
    unsigned int record_size;
    PlayRecord records[];
} PlayJournal;

#define PLAY_JOURNAL_MAGIC 0x504C5931u
static PlayJournal *s_journal;
static int s_journal_count;
static int s_journal_uid;
static int s_journal_dirty;
static int s_persist_failed;

static int journal_flush(void)
{
    char path[64];
    if (!s_journal_dirty) return 0;
    snprintf(path, sizeof(path), "config/plays_%d.bin", s_journal_uid);
    if (fs_write_atomic(path, s_journal,
                        sizeof(*s_journal) + (size_t)s_journal_count * sizeof(PlayRecord)) < 0) {
        logLine("reporter: journal write failed uid=%d count=%d\n",
                s_journal_uid, s_journal_count);
        return -1;
    }
    s_journal_dirty = 0;
    return 0;
}

static int journal_load(int uid)
{
    char path[64];
    SceIoStat st;
    SceUID fd;
    PlayJournal *journal;
    int rc, bytes, offset = 0;
    if (uid <= 0) return -1;
    if (uid == s_journal_uid) return 0;
    if (journal_flush() < 0) return -1;
    snprintf(path, sizeof(path), "config/plays_%d.bin", uid);
    rc = fs_getstat(path, &st);
    /* SCE_ERROR_ERRNO_FILE_NOT_FOUND: a new account has no journal yet. */
    if (rc == (int)0x80010002) {
        journal = malloc(sizeof(*journal));
        if (!journal) return -1;
        journal->magic = PLAY_JOURNAL_MAGIC;
        journal->record_size = sizeof(PlayRecord);
        bytes = sizeof(*journal);
    } else {
        if (rc < 0 || st.st_size < (SceOff)sizeof(*journal) || st.st_size > INT_MAX ||
            (st.st_size - sizeof(*journal)) % sizeof(PlayRecord) != 0) return -1;
        bytes = (int)st.st_size;
        journal = malloc((size_t)bytes);
        if (!journal) return -1;
        fd = fs_open(path, PSP_O_RDONLY, 0);
        if (fd < 0) { free(journal); return -1; }
        while (offset < bytes) {
            rc = fs_read(fd, (char *)journal + offset, (size_t)(bytes - offset));
            if (rc <= 0) break;
            offset += rc;
        }
        rc = fs_close(fd);
        if (offset != bytes || rc < 0 || journal->magic != PLAY_JOURNAL_MAGIC ||
            journal->record_size != sizeof(PlayRecord)) { free(journal); return -1; }
        for (int i = 0; i < (bytes - (int)sizeof(*journal)) / (int)sizeof(PlayRecord); i++) {
            const PlayRecord *r = &journal->records[i];
            if (!memchr(r->track_id, 0, sizeof(r->track_id)) ||
                !memchr(r->play_id, 0, sizeof(r->play_id)) ||
                !memchr(r->timestamp, 0, sizeof(r->timestamp)) ||
                !r->track_id[0] || !r->play_id[0] || r->total_played_ms < 0 ||
                r->position_ms < 0 || r->track_len_ms < 0) {
                free(journal);
                return -1;
            }
        }
    }
    free(s_journal);
    s_journal = journal;
    s_journal_uid = uid;
    s_journal_count = (bytes - (int)sizeof(*journal)) / (int)sizeof(PlayRecord);
    logLine("reporter: journal loaded uid=%d count=%d\n", uid, s_journal_count);
    return 0;
}

static int journal_put(const ReporterEvent *event)
{
    int at;
    PlayRecord *record;
    if (journal_load(event->uid) < 0) return -1;
    for (at = 0; at < s_journal_count; at++) {
        record = &s_journal->records[at];
        if (strcmp(record->play_id, event->play_id) == 0 &&
            (record->total_played_ms == 0) == (event->total_played_ms == 0)) break;
    }
    if (at == s_journal_count) {
        size_t bytes = sizeof(*s_journal) + ((size_t)s_journal_count + 1) * sizeof(PlayRecord);
        PlayJournal *grown;
        if (bytes > INT_MAX) return -1;
        grown = realloc(s_journal, bytes);
        if (!grown) return -1;
        s_journal = grown;
        s_journal_count++;
    }
    record = &s_journal->records[at];
    memset(record, 0, sizeof(*record));
    memcpy(record->track_id, event->track_id, sizeof(record->track_id));
    record->album_id = event->album_id;
    memcpy(record->play_id, event->play_id, sizeof(record->play_id));
    record->total_played_ms = event->total_played_ms;
    record->position_ms = event->position_ms;
    record->track_len_ms = event->track_len_ms;
    memcpy(record->timestamp, event->timestamp, sizeof(record->timestamp));
    s_journal_dirty = 1;
    return journal_flush();
}

static void journal_send(const char *token)
{
    int at = 0;
    while (s_running && at < s_journal_count) {
        const PlayRecord *record = &s_journal->records[at];
        ReporterEvent event;
        int rc;
        memset(&event, 0, sizeof(event));
        event.type = PLAYBACK_REPORT_PLAY_AUDIO;
        event.uid = s_journal_uid;
        snprintf(event.token, sizeof(event.token), "%s", token);
        memcpy(event.track_id, record->track_id, sizeof(event.track_id));
        event.album_id = record->album_id;
        memcpy(event.play_id, record->play_id, sizeof(event.play_id));
        event.total_played_ms = record->total_played_ms;
        event.position_ms = record->position_ms;
        event.track_len_ms = record->track_len_ms;
        memcpy(event.timestamp, record->timestamp, sizeof(event.timestamp));
        if (ym_time_format(event.client_now, sizeof(event.client_now), YM_TIME_UTC) < 0) break;
        rc = reporter_send_once(&event);
        memset(event.token, 0, sizeof(event.token));
        if (rc != YM_ROTOR_OK && rc != YM_ROTOR_ERR_BAD_REPORT) {
            logLine("reporter: retained play='%s' total_ms=%d result=%d\n",
                    record->play_id, record->total_played_ms, rc);
            break;
        }
        logLine("reporter: resolved play='%s' track='%s' album=%d total_ms=%d position_ms=%d result=%d\n",
                record->play_id, record->track_id, record->album_id,
                record->total_played_ms, record->position_ms, rc);
        s_journal_count--;
        memmove(&s_journal->records[at], &s_journal->records[at + 1],
                (size_t)(s_journal_count - at) * sizeof(PlayRecord));
        s_journal_dirty = 1;
        if (journal_flush() < 0) break;
    }
}

static int reporter_worker(SceSize args, void *argp)
{
    static const SceUInt backoff_us[REPORTER_MAX_ATTEMPTS] = {0, 1000000u, 2000000u, 4000000u};
    ReporterNode *deferred = NULL;
    unsigned int loaded_account_revision = 0;
    (void)args;
    (void)argp;
    logLine("reporter: worker started\n");
    for (;;) {
        ReporterNode *nodes;
        char token[256];
        int uid;
        unsigned int account_revision;
        if (sceKernelWaitSema(s_sema, 1, NULL) < 0) break;
        reporter_lock();
        nodes = s_head;
        s_head = s_tail = NULL;
        uid = s_cached_uid;
        account_revision = s_account_revision;
        memcpy(token, s_cached_token, sizeof(token));
        reporter_unlock();
        if (deferred) {
            ReporterNode *tail = deferred;
            while (tail->next) tail = tail->next;
            tail->next = nodes;
            nodes = deferred;
            deferred = NULL;
        }
        if (s_running && uid > 0 && account_revision != loaded_account_revision && journal_load(uid) == 0) {
            loaded_account_revision = account_revision;
            journal_send(token);
        }
        memset(token, 0, sizeof(token));
        while (nodes) {
            ReporterNode *node = nodes;
            ReporterEvent *event = &node->event;
            nodes = node->next;
            if (event->type == PLAYBACK_REPORT_PLAY_AUDIO) {
                if (journal_put(event) < 0) {
                    logLine("reporter: persistence failed play='%s'; retained in RAM\n", event->play_id);
                    node->next = nodes;
                    deferred = node;
                    break;
                }
                if (s_running && !event->store_only) journal_send(event->token);
            } else if (s_running) {
                int rc = YM_ROTOR_ERR_RETRY;
                for (int attempt = 0; attempt < REPORTER_MAX_ATTEMPTS && s_running; attempt++) {
                    if (attempt > 0) sceKernelDelayThread(backoff_us[attempt]);
                    if (!s_running) break;
                    rc = reporter_send_once(event);
                    if (rc != YM_ROTOR_ERR_RETRY) break;
                }
                logLine("reporter: feedback type=%d result=%d\n", (int)event->type, rc);
            }
            memset(event->token, 0, sizeof(event->token));
            free(node);
        }
        reporter_lock();
        int done = !s_running && !s_head;
        reporter_unlock();
        if (done) break;
    }
    s_persist_failed = deferred != NULL || journal_flush() < 0;
    /* Keep failed events owned until shutdown reports the failure. */
    s_head = deferred;
    logLine("reporter: worker stopped persist_failed=%d\n", s_persist_failed);
    return 0;
}

int playback_reporter_init(void)
{
    if (s_thread >= 0) return 0;
    memset(&s_mutex, 0, sizeof(s_mutex));
    if (sceKernelCreateLwMutex(&s_mutex, "playback_reporter", 0, 0, NULL) < 0) return -1;
    s_mutex_initialized = 1;
    s_sema = sceKernelCreateSema("playback_reporter", 0, 0, 1, NULL);
    if (s_sema < 0) goto failed;
    s_thread = sceKernelCreateThread("playback_reporter", reporter_worker,
                                    REPORTER_THREAD_PRIORITY, REPORTER_STACK_SIZE,
                                    PSP_THREAD_ATTR_USER, NULL);
    if (s_thread < 0) goto failed;
    s_running = 1;
    if (sceKernelStartThread(s_thread, 0, NULL) < 0) {
        s_running = 0;
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
        goto failed;
    }
    return 0;
failed:
    if (s_sema >= 0) sceKernelDeleteSema(s_sema);
    s_sema = -1;
    sceKernelDeleteLwMutex(&s_mutex);
    s_mutex_initialized = 0;
    return -1;
}

void playback_reporter_set_account(int uid, const char *token)
{
    if (!s_running || uid <= 0 || !token || !token[0]) return;
    reporter_lock();
    s_cached_uid = uid;
    s_account_revision++;
    snprintf(s_cached_token, sizeof(s_cached_token), "%s", token);
    reporter_unlock();
    sceKernelSignalSema(s_sema, 1);
}

int playback_reporter_quiesce(void)
{
    SceUInt timeout = 20000000;
    if (s_thread < 0) return s_persist_failed ? -1 : 0;
    reporter_lock();
    s_running = 0;
    reporter_unlock();
    sceKernelSignalSema(s_sema, 1);
    if (sceKernelWaitThreadEnd(s_thread, &timeout) < 0) {
        logLine("reporter: quiesce timeout\n");
        return -1;
    }
    sceKernelDeleteThread(s_thread);
    s_thread = -1;
    return s_persist_failed ? -1 : 0;
}

void playback_reporter_shutdown(void)
{
    if (playback_reporter_quiesce() < 0) return;
    if (s_sema >= 0) sceKernelDeleteSema(s_sema);
    s_sema = -1;
    if (s_mutex_initialized) sceKernelDeleteLwMutex(&s_mutex);
    s_mutex_initialized = 0;
    free(s_journal);
    s_journal = NULL;
    s_journal_count = s_journal_uid = s_journal_dirty = 0;
    s_cached_uid = 0;
    memset(s_cached_token, 0, sizeof(s_cached_token));
}
