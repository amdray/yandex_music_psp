#include "services/track_hydrator.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/net_tls.h"
#include "services/track_meta_store.h"

typedef struct {
    TrackRef refs[TRACK_HYDRATOR_JOB_MAX];
    int positions[TRACK_HYDRATOR_JOB_MAX];
    char token[256];
    int count;
    unsigned int generation;
    unsigned int serial;
    int pending;
    int result;
} HydratorJob;
static SceUID s_thread = -1, s_sema = -1;
static SceLwMutexWorkarea s_mutex;
static int s_mutex_initialized;
static volatile int s_running, s_cancel_requested;
static TrackHydratorSink s_sink;
static HydratorJob s_jobs[TRACK_HYDRATOR_CONSUMERS];
static void hydrator_lock(void) { sceKernelLockLwMutex(&s_mutex, 1, NULL); }
static void hydrator_unlock(void) { sceKernelUnlockLwMutex(&s_mutex, 1); }

typedef struct { int result; unsigned int view_generation; } HydratorPublication;
static YmApiStreamDecision hydrator_on_track(const TrackRef *ref, const TrackEntry *entry, void *user)
{
    HydratorPublication *publication = user;
    int rc = track_meta_store_put(ref, entry, publication->view_generation);
    if (rc != 0) publication->result = rc;
    return YM_API_STREAM_CONTINUE;
}

static int hydrator_run_job(HydratorJob *job, TrackHydratorConsumer consumer)
{
    TrackRef missing[TRACK_HYDRATOR_JOB_MAX];
    int count = 0, status = 0;
    HydratorPublication publication = {0, consumer == TRACK_HYDRATOR_SCREEN ? job->generation : 0};
    TrackEntry entry;
    for (int i = 0; i < job->count; ++i) {
        int rc = track_meta_store_load_for(job->refs[i].id, job->refs[i].album_id,
                                          &entry, publication.view_generation);
        if (rc == TRACK_META_ERROR) {
            logLine("hydrator: store read failed consumer=%d generation=%u serial=%u track=%s album=%d\n",
                consumer, job->generation, job->serial, job->refs[i].id, job->refs[i].album_id);
            return -1;
        }
        if (rc == TRACK_META_MISSING) {
            int j;
            for (j = 0; j < count; ++j)
                if (missing[j].album_id == job->refs[i].album_id &&
                    strcmp(missing[j].id, job->refs[i].id) == 0) break;
            if (j == count) missing[count++] = job->refs[i];
        }
    }
    if (count) {
        YmApiContext ctx = {job->token, 0};
        net_tls_cancel_bind(&s_cancel_requested);
        int rc = ym_api_tracks_hydrate_refs(&ctx, missing, count,
                                             hydrator_on_track, &publication, &status);
        net_tls_cancel_unbind();
        if (rc < 0 || publication.result) {
            logLine("hydrator: batch failed consumer=%d generation=%u serial=%u requested=%d api=%d status=%d store_failed=%d\n",
                consumer, job->generation, job->serial, count, rc, status, publication.result);
            return rc < 0 && status == NET_LOAD_ERR_TRANSPORT && !publication.result
                ? TRACK_HYDRATOR_TRANSPORT_ERROR : -1;
        }
    }
    for (int i = 0; i < job->count; ++i) {
        if (track_meta_store_get_for(job->refs[i].id, job->refs[i].album_id, &entry) != 0) {
            logLine("hydrator: unresolved consumer=%d generation=%u serial=%u position=%d track=%s album=%d\n",
                consumer, job->generation, job->serial,
                job->positions[i], job->refs[i].id, job->refs[i].album_id);
            return -1;
        }
        if (consumer == TRACK_HYDRATOR_SCREEN && s_sink)
            s_sink(&job->refs[i], &entry, job->positions[i], job->generation);
    }
    return 0;
}

static int hydrator_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (s_running) {
        if (sceKernelWaitSema(s_sema, 1, NULL) < 0) break;
        for (;;) {
            HydratorJob job;
            int consumer;
            hydrator_lock();
            for (consumer = 0; consumer < TRACK_HYDRATOR_CONSUMERS; ++consumer)
                if (s_jobs[consumer].pending) break;
            if (!s_running || consumer == TRACK_HYDRATOR_CONSUMERS) {
                hydrator_unlock();
                break;
            }
            job = s_jobs[consumer];
            s_jobs[consumer].pending = 0;
            track_meta_store_set_active_job(job.refs, job.count);
            hydrator_unlock();
            logLine("hydrator: start consumer=%d generation=%u serial=%u count=%d track=%s album=%d\n",
                consumer, job.generation, job.serial, job.count, job.refs[0].id, job.refs[0].album_id);
            int rc = hydrator_run_job(&job, (TrackHydratorConsumer)consumer);
            hydrator_lock();
            int cancelled = s_jobs[consumer].serial != job.serial;
            if (!cancelled) {
                s_jobs[consumer].result = rc;
            }
            track_meta_store_set_active_job(NULL, 0);
            hydrator_unlock();
            if (cancelled)
                logLine("hydrator: cancelled consumer=%d generation=%u serial=%u count=%d\n",
                        consumer, job.generation, job.serial, job.count);
            else
                logLine("hydrator: consumer=%d generation=%u count=%d result=%d serial=%u\n",
                        consumer, job.generation, job.count, rc, job.serial);
            logger_flush();
        }
    }
    return 0;
}
int track_hydrator_init(TrackHydratorSink sink)
{
    if (s_running) {
        return 0;
    }
    if (!sink) {
        return -1;
    }
    s_sink = sink;

    if (sceKernelCreateLwMutex(&s_mutex, "hydrator", 0, 0, NULL) >= 0) {
        s_mutex_initialized = 1;
    } else return -1;

    s_sema = sceKernelCreateSema("hydrator", 0, 0, 255, NULL);
    if (s_sema < 0) {
        sceKernelDeleteLwMutex(&s_mutex);
        s_mutex_initialized = 0;
        return -1;
    }

    memset(s_jobs, 0, sizeof(s_jobs));
    s_cancel_requested = 0;

    s_running = 1;
    s_thread = sceKernelCreateThread("hydrator", hydrator_worker, 0x18, 0x18000, 0, NULL);
    if (s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0) {
        s_running = 0;
        if (s_thread >= 0) {
            sceKernelDeleteThread(s_thread);
            s_thread = -1;
        }
        sceKernelDeleteSema(s_sema);
        s_sema = -1;
        sceKernelDeleteLwMutex(&s_mutex);
        s_mutex_initialized = 0;
        return -1;
    }
    return 0;
}

int track_hydrator_quiesce(void)
{
    if (!s_running && s_thread < 0) return 0;
    s_cancel_requested = 1;
    s_running = 0;
    if (s_sema >= 0) {
        sceKernelSignalSema(s_sema, 1);
    }
    if (s_thread >= 0) {
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_thread, &timeout_us) < 0) {
            logLine("hydrator: worker still active; resources retained\n");
            return -1;
        }
    }
    return 0;
}

int track_hydrator_shutdown(void)
{
    if (track_hydrator_quiesce() < 0) return -1;
    if (s_thread >= 0) {
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_sema >= 0) {
        sceKernelDeleteSema(s_sema);
        s_sema = -1;
    }
    if (s_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_mutex);
        s_mutex_initialized = 0;
    }
    s_sink = NULL;
    return 0;
}

int track_hydrator_request(TrackHydratorConsumer consumer, const char *token,
                           const TrackRef *refs, const int *positions,
                           int count, unsigned int generation)
{
    if (!s_running || consumer < 0 || consumer >= TRACK_HYDRATOR_CONSUMERS ||
        !token || !token[0] || !refs || count <= 0 || count > TRACK_HYDRATOR_JOB_MAX)
        return -1;
    hydrator_lock();
    HydratorJob *job = &s_jobs[consumer];
    /* Rendering may consume part of an active screen batch between frames. */
    if (consumer == TRACK_HYDRATOR_SCREEN && positions &&
        job->generation == generation && job->result == 1) {
        int covered = 1;
        for (int i = 0; i < count && covered; ++i) {
            int j;
            for (j = 0; j < job->count; ++j)
                if (job->positions[j] == positions[i] &&
                    job->refs[j].album_id == refs[i].album_id &&
                    strcmp(job->refs[j].id, refs[i].id) == 0) break;
            if (j == job->count) covered = 0;
        }
        if (covered) { hydrator_unlock(); return 1; }
    }
    int same = job->count == count && job->generation == generation &&
        memcmp(job->refs, refs, (size_t)count * sizeof(*refs)) == 0;
    if (same && positions)
        same = memcmp(job->positions, positions, (size_t)count * sizeof(*positions)) == 0;
    if (same && job->result == 0) {
        TrackEntry entry;
        for (int i = 0; i < count; ++i) {
            if (track_meta_store_get_for(refs[i].id, refs[i].album_id, &entry) != 0) {
                same = 0;
                break;
            }
        }
    }
    if (!same) {
        track_meta_store_set_working_set(consumer, refs, count);
        memcpy(job->refs, refs, (size_t)count * sizeof(*refs));
        if (positions) memcpy(job->positions, positions, (size_t)count * sizeof(*positions));
        else memset(job->positions, 0, sizeof(job->positions));
        snprintf(job->token, sizeof(job->token), "%s", token);
        job->count = count;
        job->generation = generation;
        ++job->serial;
        job->pending = 1;
        job->result = 1;
        logLine("hydrator: demand consumer=%d generation=%u serial=%u count=%d track=%s album=%d\n",
            consumer, generation, job->serial, count, refs[0].id, refs[0].album_id);
    }
    int result = job->result;
    hydrator_unlock();
    if (!same) sceKernelSignalSema(s_sema, 1);
    return result;
}

void track_hydrator_cancel(TrackHydratorConsumer consumer)
{
    if (!s_running || consumer < 0 || consumer >= TRACK_HYDRATOR_CONSUMERS) return;
    hydrator_lock();
    logLine("hydrator: cancel demand consumer=%d generation=%u serial=%u count=%d\n",
        consumer, s_jobs[consumer].generation, s_jobs[consumer].serial, s_jobs[consumer].count);
    s_jobs[consumer].count = 0;
    s_jobs[consumer].pending = 0;
    ++s_jobs[consumer].serial;
    hydrator_unlock();
    track_meta_store_set_working_set(consumer, NULL, 0);
}
