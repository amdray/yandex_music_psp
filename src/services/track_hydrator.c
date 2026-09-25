#include "services/track_hydrator.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/net_tls.h"
#include "services/track_meta_store.h"

typedef struct {
    int pending;
    char token[256];
    ListIndexId ids[TRACK_HYDRATOR_JOB_MAX];
    int album_ids[TRACK_HYDRATOR_JOB_MAX];
    int positions[TRACK_HYDRATOR_JOB_MAX];
    int count;
    int generation;
} HydratorJob;

static SceUID s_thread = -1;
static SceUID s_sema = -1;
static SceLwMutexWorkarea s_mutex;
static int s_mutex_initialized = 0;
static volatile int s_running = 0;
static volatile int s_cancel_requested = 0;

static TrackHydratorSink s_sink = NULL;
static HydratorJob s_track_job;   /* playback: served first */
static HydratorJob s_window_job;  /* list window */

static void hydrator_lock(void)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_mutex, 1, NULL);
    }
}

static void hydrator_unlock(void)
{
    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_mutex, 1);
    }
}

/* Context for mapping hydrated entries back to list positions. */
typedef struct {
    HydratorJob *job;
    int delivered;
} HydrateDeliver;

static YmApiStreamDecision hydrator_on_track(const TrackEntry *entry, void *user_data)
{
    HydrateDeliver *ctx = (HydrateDeliver *)user_data;
    int i;

    if (!ctx || !entry) {
        return YM_API_STREAM_ERROR;
    }

    track_meta_store_put(entry);

    for (i = 0; i < ctx->job->count; i++) {
        if (strcmp(ctx->job->ids[i], entry->id) == 0 &&
            (ctx->job->album_ids[i] == 0 ||
             ctx->job->album_ids[i] == entry->album_id)) {
            if (s_sink) {
                s_sink(entry, ctx->job->positions[i], ctx->job->generation);
            }
            ctx->delivered++;
            break;
        }
    }
    return YM_API_STREAM_CONTINUE;
}

static void hydrator_run_job(HydratorJob *job)
{
    ListIndexId misses[TRACK_HYDRATOR_JOB_MAX];
    int miss_album_ids[TRACK_HYDRATOR_JOB_MAX];
    int miss_pos[TRACK_HYDRATOR_JOB_MAX];
    int miss_count = 0;
    int i;

    /* Store first: most window slots are hits after one blob/likes pass. */
    for (i = 0; i < job->count; i++) {
        TrackEntry entry;
        if (track_meta_store_get_for(job->ids[i], job->album_ids[i],
                                     &entry) == 0) {
            if (s_sink) {
                s_sink(&entry, job->positions[i], job->generation);
            }
        } else {
            memcpy(misses[miss_count], job->ids[i], sizeof(ListIndexId));
            miss_album_ids[miss_count] = job->album_ids[i];
            miss_pos[miss_count] = job->positions[i];
            miss_count++;
        }
    }

    if (miss_count > 0) {
        YmApiContext ctx;
        HydrateDeliver deliver;
        HydratorJob net_job;
        int status = 0;

        /* Re-shape the job to just the misses so the callback maps ids to
           positions without touching the original arrays. */
        memset(&net_job, 0, sizeof(net_job));
        memcpy(net_job.ids, misses, (size_t)miss_count * sizeof(ListIndexId));
        memcpy(net_job.album_ids, miss_album_ids,
               (size_t)miss_count * sizeof(int));
        memcpy(net_job.positions, miss_pos, (size_t)miss_count * sizeof(int));
        net_job.count = miss_count;
        net_job.generation = job->generation;

        deliver.job = &net_job;
        deliver.delivered = 0;

        ctx.oauth_token = job->token;
        ctx.timeout_ms = 0;

        int hydrate_rc;
        net_tls_cancel_bind(&s_cancel_requested);
        {
            TrackRef refs[TRACK_HYDRATOR_JOB_MAX];
            for (i = 0; i < miss_count; ++i) {
                memset(&refs[i], 0, sizeof(refs[i]));
                snprintf(refs[i].id, sizeof(refs[i].id), "%s",
                         net_job.ids[i]);
                refs[i].album_id = net_job.album_ids[i];
            }
            hydrate_rc = ym_api_tracks_hydrate_refs(
                &ctx, refs, miss_count, hydrator_on_track, &deliver, &status);
        }
        net_tls_cancel_unbind();
        if (hydrate_rc != 0) {
            logLine("hydrator: net hydrate failed status=%d gen=%d\n",
                    status, job->generation);
        } else {
            logLine("hydrator: hydrated %d/%d misses gen=%d\n",
                    deliver.delivered, miss_count, job->generation);
        }
    }
}

static int hydrator_worker(SceSize args __attribute__((unused)),
                           void *argp __attribute__((unused)))
{
    logLine("hydrator: worker started\n");

    while (s_running) {
        HydratorJob job;

        if (sceKernelWaitSema(s_sema, 1, NULL) < 0) {
            break;
        }
        if (!s_running) {
            break;
        }

        hydrator_lock();
        if (s_track_job.pending) {
            memcpy(&job, &s_track_job, sizeof(job));
            s_track_job.pending = 0;
        } else if (s_window_job.pending) {
            memcpy(&job, &s_window_job, sizeof(job));
            s_window_job.pending = 0;
        } else {
            hydrator_unlock();
            continue;
        }
        hydrator_unlock();

        if (job.count > 0 && job.token[0]) {
            hydrator_run_job(&job);
        }
    }

    logLine("hydrator: worker stopped\n");
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
    }

    s_sema = sceKernelCreateSema("hydrator", 0, 0, 255, NULL);
    if (s_sema < 0) {
        return -1;
    }

    memset(&s_track_job, 0, sizeof(s_track_job));
    memset(&s_window_job, 0, sizeof(s_window_job));
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

int track_hydrator_request_window(const char *token,
                                  const ListIndexId *ids,
                                  int start,
                                  int count,
                                  int generation)
{
    int i;

    if (!s_running || !token || !token[0] || !ids ||
        count <= 0 || count > TRACK_HYDRATOR_JOB_MAX) {
        return -1;
    }

    hydrator_lock();
    s_window_job.pending = 1;
    snprintf(s_window_job.token, sizeof(s_window_job.token), "%s", token);
    memcpy(s_window_job.ids, ids, (size_t)count * sizeof(ListIndexId));
    memset(s_window_job.album_ids, 0, (size_t)count * sizeof(int));
    for (i = 0; i < count; i++) {
        s_window_job.positions[i] = start + i;
    }
    s_window_job.count = count;
    s_window_job.generation = generation;
    hydrator_unlock();

    sceKernelSignalSema(s_sema, 1);
    return 0;
}

int track_hydrator_request_track(const char *token,
                                 const char *track_id,
                                 int album_id,
                                 int position,
                                 int generation)
{
    if (!s_running || !token || !token[0] ||
        !track_id || !track_id[0] ||
        strlen(track_id) >= LIST_INDEX_ID_SIZE) {
        return -1;
    }

    hydrator_lock();
    s_track_job.pending = 1;
    snprintf(s_track_job.token, sizeof(s_track_job.token), "%s", token);
    memset(s_track_job.ids[0], 0, sizeof(ListIndexId));
    strcpy(s_track_job.ids[0], track_id);
    s_track_job.album_ids[0] = album_id;
    s_track_job.positions[0] = position;
    s_track_job.count = 1;
    s_track_job.generation = generation;
    hydrator_unlock();

    sceKernelSignalSema(s_sema, 1);
    return 0;
}
