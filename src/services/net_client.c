#include "services/net_client.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/logger.h"
#include "services/net_http.h"
#include "services/resource_policy.h"
#include "services/net_tls.h"
#include "services/net_stack.h"
#include "services/api_parser.h"
#include "services/ym_api.h"
#include "services/list_index.h"
#include "services/track_meta_store.h"
#include "services/track_hydrator.h"
#include "core/fs.h"
#include "app/app_state.h"

static int s_net_error = 0;
static int s_process_exit_required = 0;
static int s_logged_net_state = -1;
static int s_net_started = 0;
static int s_net_wait_logged = 0;
static int s_net_info_logged = 0;

#define TRACK_BOOT_INITIAL_CAPACITY 256

/* Ids read per disk access when loading the order from the index. */
#define LIST_INDEX_LOAD_BLOCK 64

typedef struct {
    char token[256];
    int uid;
    int playlist_kind;
    int revision;      // track-cache key: current playlist revision at open time
    int generation;
    char uuid[48];
    struct AppState *state;
} TrackBootstrapJob;

typedef struct {
    NetPlaylistLoadKind kind;
    char token[256];
    int uid;
    int generation;
} PlaylistLoadJob;

typedef struct {
    int ready;
    NetPlaylistLoadKind kind;
    int generation;
    int rc;
    int count;
    int error_code;
    PlaylistEntry *entries;
} PlaylistLoadResult;

typedef struct {
    NetArtistLoadKind kind;
    char token[256];
    int uid;
    int artist_id;
    int generation;
} ArtistLoadJob;

typedef struct {
    int ready;
    NetArtistLoadKind kind;
    int generation;
    int rc;
    int count;
    int error_code;
    ArtistEntry artists[MAX_LIKED_ARTISTS];
    ArtistBriefInfo brief;
} ArtistLoadResult;

typedef struct {
    struct AppState *state;
    int generation;
    int playlist_kind;
    /* Revision-keyed ordered id-index written while the response streams. */
    ListIndexWriter index_writer;
    int index_active;
    ListIndexId *staged_ids;
    int staged_count;
    int staged_capacity;
    YmPlaylistTracksParser ym_parser;
} TrackBootstrapParser;

static SceUID s_track_boot_thread = -1;
static SceUID s_track_boot_sema = -1;
static SceLwMutexWorkarea s_track_boot_job_mutex;
static int s_track_boot_job_mutex_initialized = 0;
static SceLwMutexWorkarea s_track_store_mutex;
static int s_track_store_mutex_initialized = 0;
static volatile int s_track_boot_running = 0;
static volatile int s_track_boot_cancel = 0;
static volatile int s_track_boot_has_job = 0;
static TrackBootstrapJob s_track_boot_job;

static SceUID s_playlist_load_thread = -1;
static SceUID s_playlist_load_sema = -1;
static SceLwMutexWorkarea s_playlist_load_mutex;
static int s_playlist_load_mutex_initialized = 0;
static volatile int s_playlist_load_running = 0;
static volatile int s_playlist_load_cancel = 0;
static int s_playlist_load_has_job = 0;
static int s_playlist_load_busy = 0;
static PlaylistLoadJob s_playlist_load_job;
static PlaylistLoadResult s_playlist_load_result;

static SceUID s_artist_load_thread = -1;
static SceUID s_artist_load_sema = -1;
static SceLwMutexWorkarea s_artist_load_mutex;
static int s_artist_load_mutex_initialized = 0;
static volatile int s_artist_load_running = 0;
static volatile int s_artist_load_cancel = 0;
static int s_artist_load_has_job = 0;
static int s_artist_load_busy = 0;
static ArtistLoadJob s_artist_load_job;
static ArtistLoadResult s_artist_load_result;

static void track_boot_log_mem(const char *stage, int playlist_kind, int loaded_count, int published_count)
{
    int max_free = sceKernelMaxFreeMemSize();
    logLine("mem: track_boot %s kind=%d loaded=%d published=%d max_free=%d\n",
            stage ? stage : "unknown",
            playlist_kind,
            loaded_count,
            published_count,
            max_free);
}

static void track_boot_job_lock(void)
{
    if (s_track_boot_job_mutex_initialized) {
        sceKernelLockLwMutex(&s_track_boot_job_mutex, 1, NULL);
    }
}

static void track_boot_job_unlock(void)
{
    if (s_track_boot_job_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_track_boot_job_mutex, 1);
    }
}

static void playlist_load_lock(void)
{
    if (s_playlist_load_mutex_initialized) {
        sceKernelLockLwMutex(&s_playlist_load_mutex, 1, NULL);
    }
}

static void playlist_load_unlock(void)
{
    if (s_playlist_load_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_playlist_load_mutex, 1);
    }
}

static void artist_load_lock(void)
{
    if (s_artist_load_mutex_initialized) {
        sceKernelLockLwMutex(&s_artist_load_mutex, 1, NULL);
    }
}

static void artist_load_unlock(void)
{
    if (s_artist_load_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_artist_load_mutex, 1);
    }
}

void net_client_track_store_lock(void)
{
    if (s_track_store_mutex_initialized) {
        sceKernelLockLwMutex(&s_track_store_mutex, 1, NULL);
    }
}

void net_client_track_store_unlock(void)
{
    if (s_track_store_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_track_store_mutex, 1);
    }
}

/* The AppState whose track window the hydrator sink fills. Set when a track
   bootstrap starts; AppState is a static singleton in main, so the pointer
   outlives every worker. */
static struct AppState *s_track_state = NULL;

int net_client_track_window_ready_locked(const struct AppState *state, int pos, int span)
{
    int end;
    int i;

    if (!state || state->track_store.count <= 0 || pos < 0 ||
        pos >= state->track_store.count) {
        return 0;
    }
    end = pos + span;
    if (end > state->track_store.count) {
        end = state->track_store.count;
    }
    for (i = pos; i < end; i++) {
        int slot = i - state->track_store.window_start;
        if (slot < 0 || slot >= TRACK_WINDOW_CAPACITY ||
            !state->track_store.window_valid[slot]) {
            return 0;
        }
    }
    return 1;
}

static int track_boot_is_stale(const struct AppState *state, int generation, int playlist_kind)
{
    if (!state) {
        return 1;
    }
    if (state->track_boot.generation != generation) {
        return 1;
    }
    if (state->track_boot.target_playlist_kind != playlist_kind) {
        return 1;
    }
    return 0;
}

static void track_boot_publish_error(struct AppState *state, int generation, int playlist_kind, int error_code)
{
    if (!state) {
        return;
    }
    net_client_track_store_lock();
    if (track_boot_is_stale(state, generation, playlist_kind)) {
        net_client_track_store_unlock();
        return;
    }
    state->track_boot.status = TRACK_BOOT_ERROR;
    state->track_boot.error_code = error_code;
    state->track_ui.track_bootstrap_indicator_visible = 0;
    net_client_track_store_unlock();
}

static int track_boot_ensure_id_capacity(struct AppState *state, int required)
{
    if (!state) {
        return -1;
    }
    if (required <= state->track_store.ids_capacity) {
        return 0;
    }

    int new_capacity = state->track_store.ids_capacity > 0
                           ? state->track_store.ids_capacity
                           : TRACK_BOOT_INITIAL_CAPACITY;
    while (new_capacity < required) {
        new_capacity *= 2;
    }

    ListIndexId *new_ids = (ListIndexId *)realloc(state->track_store.ids,
                                                  (size_t)new_capacity * sizeof(ListIndexId));
    if (!new_ids) {
        return -1;
    }

    state->track_store.ids = new_ids;
    state->track_store.ids_capacity = new_capacity;
    return 0;
}

/* Fill a window slot if the position currently falls inside the window.
   Caller holds the track-store lock. */
static void track_window_set_locked(struct AppState *state, int pos, const TrackEntry *entry)
{
    int slot = pos - state->track_store.window_start;
    if (slot >= 0 && slot < TRACK_WINDOW_CAPACITY) {
        memcpy(&state->track_store.window[slot], entry, sizeof(TrackEntry));
        state->track_store.window_valid[slot] = 1;
    }
}

static YmApiStreamDecision track_boot_on_track_id(const char *track_id, void *user_data)
{
    TrackBootstrapParser *parser = (TrackBootstrapParser *)user_data;
    int new_capacity;
    ListIndexId *new_ids;

    if (!parser || !parser->state || !track_id || !track_id[0]) {
        return YM_API_STREAM_ERROR;
    }
    if (track_boot_is_stale(parser->state, parser->generation,
                            parser->playlist_kind)) {
        return YM_API_STREAM_ERROR;
    }
    if (parser->staged_count >= parser->staged_capacity) {
        new_capacity = parser->staged_capacity > 0
                           ? parser->staged_capacity * 2
                           : TRACK_BOOT_INITIAL_CAPACITY;
        new_ids = (ListIndexId *)realloc(parser->staged_ids,
                                         (size_t)new_capacity * sizeof(ListIndexId));
        if (!new_ids) return YM_API_STREAM_ERROR;
        parser->staged_ids = new_ids;
        parser->staged_capacity = new_capacity;
    }
    memset(parser->staged_ids[parser->staged_count], 0, sizeof(ListIndexId));
    snprintf(parser->staged_ids[parser->staged_count], sizeof(ListIndexId),
             "%s", track_id);
    ++parser->staged_count;

    /* Частичная публикация каждые 64 id: экран входит по первым строкам
     * (гейт ждёт окно, не весь список), хвост докачивается в фоне.
     * Без этого список на 2500 треков висит до конца всего стрима. */
    if ((parser->staged_count % LIST_INDEX_LOAD_BLOCK) == 0) {
        net_client_track_store_lock();
        if (!track_boot_is_stale(parser->state, parser->generation,
                                 parser->playlist_kind) &&
            track_boot_ensure_id_capacity(parser->state,
                                          parser->staged_count) == 0) {
            memcpy(parser->state->track_store.ids, parser->staged_ids,
                   (size_t)parser->staged_count * sizeof(ListIndexId));
            parser->state->track_store.count = parser->staged_count;
            parser->state->track_boot.loaded_count = parser->staged_count;
        }
        net_client_track_store_unlock();
    }

    if (parser->index_active &&
        list_index_writer_append(&parser->index_writer, track_id) != 0) {
        logLine("track_boot: index write-through failed, disabling\n");
        list_index_writer_abort(&parser->index_writer);
        parser->index_active = 0;
    }

    return YM_API_STREAM_CONTINUE;
}

static void track_hydrator_sink_cb(const TrackEntry *entry, int position, int generation)
{
    struct AppState *state = s_track_state;

    if (!state || !entry) {
        return;
    }
    net_client_track_store_lock();
    if (state->track_boot.generation == generation) {
        track_window_set_locked(state, position, entry);
    }
    net_client_track_store_unlock();
}

void net_client_track_window_focus(struct AppState *state, const char *token, int focus)
{
    net_client_track_window_focus_span(state, token, focus,
                                       TRACK_WINDOW_CAPACITY);
}

void net_client_track_window_focus_span(struct AppState *state, const char *token,
                                        int focus, int want_span)
{
    static int s_req_start = -1;
    static int s_req_gen = -1;
    static int s_req_span = -1;
    static u64 s_req_us = 0;
    ListIndexId slice[TRACK_WINDOW_CAPACITY];
    int count;
    int start;
    int span;
    int missing = 0;
    int gen;
    int i;

    if (!state || !token || !token[0]) {
        return;
    }

    span = want_span;

    net_client_track_store_lock();
    count = state->track_store.count;
    if (count <= 0) {
        net_client_track_store_unlock();
        return;
    }
    if (focus < 0) {
        focus = 0;
    }
    if (focus >= count) {
        focus = count - 1;
    }
    if (span <= 0) {
        span = TRACK_LIST_VISIBLE_ROWS;
    }
    if (span > TRACK_WINDOW_CAPACITY) {
        span = TRACK_WINDOW_CAPACITY;
    }

    start = focus - span / 2;
    if (start > count - span) {
        start = count - span;
    }
    if (start < 0) {
        start = 0;
    }

    if (start != state->track_store.window_start) {
        TrackRuntimeStorage *ts = &state->track_store;
        int shift = start - ts->window_start;
        if (shift > 0 && shift < TRACK_WINDOW_CAPACITY) {
            memmove(&ts->window[0], &ts->window[shift],
                    (size_t)(TRACK_WINDOW_CAPACITY - shift) * sizeof(TrackEntry));
            memmove(&ts->window_valid[0], &ts->window_valid[shift],
                    (size_t)(TRACK_WINDOW_CAPACITY - shift));
            memset(&ts->window_valid[TRACK_WINDOW_CAPACITY - shift], 0, (size_t)shift);
        } else if (shift < 0 && -shift < TRACK_WINDOW_CAPACITY) {
            memmove(&ts->window[-shift], &ts->window[0],
                    (size_t)(TRACK_WINDOW_CAPACITY + shift) * sizeof(TrackEntry));
            memmove(&ts->window_valid[-shift], &ts->window_valid[0],
                    (size_t)(TRACK_WINDOW_CAPACITY + shift));
            memset(&ts->window_valid[0], 0, (size_t)(-shift));
        } else {
            memset(ts->window_valid, 0, sizeof(ts->window_valid));
        }
        ts->window_start = start;
    }

    if (start + span > count) {
        span = count - start;
    }
    if (span <= 0) {
        net_client_track_store_unlock();
        return;
    }
    for (i = 0; i < span; i++) {
        if (!state->track_store.window_valid[i]) {
            missing = 1;
            break;
        }
    }
    gen = state->track_boot.generation;
    if (missing) {
        memcpy(slice, state->track_store.ids + start,
               (size_t)span * sizeof(ListIndexId));
    }
    net_client_track_store_unlock();

    if (!missing) {
        return;
    }

    /* Dedup identical requests; allow a retry every 2 s so a lost/failed
       hydration can never wedge the gate or leave permanent "..." rows. */
    {
        u64 now = sceKernelGetSystemTimeWide();
        if (start == s_req_start && gen == s_req_gen && span == s_req_span &&
            (now - s_req_us) < 2000000ULL) {
            return;
        }
        s_req_start = start;
        s_req_gen = gen;
        s_req_span = span;
        s_req_us = now;
    }
    track_hydrator_request_window(token, slice, start, span, gen);
}

static int track_boot_on_chunk(const char *data, int size, void *user_data)
{
    TrackBootstrapParser *parser = (TrackBootstrapParser *)user_data;
    int feed_rc;

    if (!parser || !data || size <= 0) {
        return -1;
    }

    feed_rc = ym_api_playlist_tracks_parser_feed(&parser->ym_parser,
                                                 data,
                                                 (size_t)size);
    if (feed_rc != 0) {
        return -1;
    }

    resource_policy_cooperate(RESOURCE_CLASS_PLAYLIST_BOOTSTRAP, size);

    return 0;
}

/* Publish the id order as complete (shared by cache-hit and network). The
   bootstrap indicator stays on until the transition gate confirms the entry
   window is hydrated. */
static void track_boot_publish_full_ready(TrackBootstrapJob *job, const char *src)
{
    net_client_track_store_lock();
    if (!track_boot_is_stale(job->state, job->generation, job->playlist_kind)) {
        job->state->track_boot.loaded_count = job->state->track_store.count;
        job->state->track_store.ids_complete = 1;
        job->state->track_boot.status = TRACK_BOOT_FULL_READY;
        track_boot_log_mem("full_ready", job->playlist_kind,
                           job->state->track_store.count,
                           job->state->track_store.count);
        logLine("track_boot: full ready (%s) kind=%d count=%d\n",
                src, job->playlist_kind, job->state->track_store.count);
    }
    net_client_track_store_unlock();
}

/* Load the id order from the disk index. Metadata is NOT touched here — rows
   hydrate on demand from the store / POST /tracks, so a metadata eviction
   never forces a blob refetch. Returns 0 on success, -1 on miss/corruption
   (caller refetches). */
static int track_boot_load_from_cache(TrackBootstrapJob *job)
{
    ListIndexReader reader;
    int pos = 0;
    int rc = 0;

    if (list_index_open(&reader, job->uuid, job->revision) != 0) {
        return -1;
    }

    while (pos < reader.count) {
        ListIndexId ids[LIST_INDEX_LOAD_BLOCK];
        int got = list_index_read_range(&reader, pos, LIST_INDEX_LOAD_BLOCK, ids);

        if (got <= 0) {
            rc = -1;
            break;
        }

        net_client_track_store_lock();
        if (track_boot_is_stale(job->state, job->generation, job->playlist_kind) ||
            track_boot_ensure_id_capacity(job->state, pos + got) != 0) {
            net_client_track_store_unlock();
            rc = -1;
            break;
        }
        memcpy(job->state->track_store.ids + pos, ids,
               (size_t)got * sizeof(ListIndexId));
        job->state->track_store.count = pos + got;
        job->state->track_boot.loaded_count = pos + got;
        net_client_track_store_unlock();

        pos += got;
    }

    list_index_close(&reader);
    return rc;
}

static int track_boot_worker(SceSize args __attribute__((unused)), void *argp __attribute__((unused)))
{
    logLine("track_boot: worker started\n");

    while (s_track_boot_running) {
        if (sceKernelWaitSema(s_track_boot_sema, 1, NULL) < 0) {
            break;
        }
        if (!s_track_boot_running) {
            break;
        }

        TrackBootstrapJob job;
        memset(&job, 0, sizeof(job));

        track_boot_job_lock();
        if (s_track_boot_has_job) {
            memcpy(&job, &s_track_boot_job, sizeof(job));
            s_track_boot_has_job = 0;
        }
        track_boot_job_unlock();

        if (!job.state || !job.token[0] || job.uid <= 0 || job.playlist_kind <= 0) {
            continue;
        }
        if (track_boot_is_stale(job.state, job.generation, job.playlist_kind)) {
            continue;
        }

        char url[256];

        /* HIT: this exact revision is indexed and fully hydratable — rebuild
           from disk, zero network. */
        if (job.uuid[0]) {
            if (track_boot_load_from_cache(&job) == 0) {
                track_boot_publish_full_ready(&job, "cache");
                continue;
            }
            /* Miss / corrupt index: drop the partial order and refetch. */
            net_client_track_store_lock();
            job.state->track_store.count = 0;
            job.state->track_store.ids_complete = 0;
            memset(job.state->track_store.window_valid, 0,
                   sizeof(job.state->track_store.window_valid));
            net_client_track_store_unlock();
        }

        TrackBootstrapParser parser;
        memset(&parser, 0, sizeof(parser));
        parser.state = job.state;
        parser.generation = job.generation;
        parser.playlist_kind = job.playlist_kind;

        int url_rc = ym_api_playlist_tracks_build_url(job.uid, job.playlist_kind,
                                                      url, sizeof(url));
        if (url_rc != 0) {
            track_boot_publish_error(job.state, job.generation, job.playlist_kind, -2);
            continue;
        }

        int parser_rc = ym_api_playlist_tracks_parser_init(&parser.ym_parser,
                                                           track_boot_on_track_id,
                                                           &parser);
        if (parser_rc != 0) {
            track_boot_publish_error(job.state, job.generation, job.playlist_kind, -2);
            continue;
        }

        /* On a miss, write the id-index while the short references stream. */
        if (job.uuid[0] &&
            list_index_writer_open(&parser.index_writer, job.uuid, job.revision) == 0) {
            parser.index_active = 1;
        }

        net_tls_cancel_bind(&s_track_boot_cancel);
        int rc = http_stream(&(HttpRequest){
                .method = HTTP_GET, .url = url, .token = job.token,
                .accept = "application/json"
            }, &(HttpSink){ .on_chunk = track_boot_on_chunk, .user = &parser });
        net_tls_cancel_unbind();
        ym_api_playlist_tracks_parser_destroy(&parser.ym_parser);

        if (rc != 0) {
            if (parser.index_active) {
                list_index_writer_abort(&parser.index_writer);
            }
            free(parser.staged_ids);
            track_boot_publish_error(job.state, job.generation, job.playlist_kind, -2);
            continue;
        }
        if (track_boot_is_stale(job.state, job.generation, job.playlist_kind)) {
            if (parser.index_active) {
                list_index_writer_abort(&parser.index_writer);
            }
            free(parser.staged_ids);
            continue;
        }

        if (parser.index_active &&
            list_index_writer_commit(&parser.index_writer) != 0) {
            parser.index_active = 0;
        }

        net_client_track_store_lock();
        if (track_boot_is_stale(job.state, job.generation, job.playlist_kind)) {
            net_client_track_store_unlock();
            free(parser.staged_ids);
            continue;
        }
        free(job.state->track_store.ids);
        job.state->track_store.ids = parser.staged_ids;
        job.state->track_store.ids_capacity = parser.staged_capacity;
        job.state->track_store.count = parser.staged_count;
        parser.staged_ids = NULL;
        net_client_track_store_unlock();

        track_boot_publish_full_ready(&job, parser.index_active ? "net+cache" : "net");
    }

    logLine("track_boot: worker stopped\n");
    return 0;
}

static int playlist_load_worker(SceSize args __attribute__((unused)), void *argp __attribute__((unused)))
{
    logLine("playlist_load: worker started\n");

    while (s_playlist_load_running) {
        if (sceKernelWaitSema(s_playlist_load_sema, 1, NULL) < 0) {
            break;
        }
        if (!s_playlist_load_running) {
            break;
        }

        PlaylistLoadJob job;
        memset(&job, 0, sizeof(job));

        playlist_load_lock();
        if (s_playlist_load_has_job) {
            memcpy(&job, &s_playlist_load_job, sizeof(job));
            s_playlist_load_has_job = 0;
            s_playlist_load_busy = 1;
        }
        playlist_load_unlock();

        if (!job.token[0] || job.uid <= 0) {
            playlist_load_lock();
            s_playlist_load_busy = 0;
            playlist_load_unlock();
            continue;
        }

        PlaylistLoadResult result;
        memset(&result, 0, sizeof(result));
        result.ready = 1;
        result.kind = job.kind;
        result.generation = job.generation;
        result.rc = -1;
        result.error_code = -2;

        YmApiContext api_ctx;
        api_ctx.oauth_token = job.token;
        api_ctx.timeout_ms = 0;

        logLine("playlist_load: fetch kind=%d uid=%d generation=%d\n",
                job.kind, job.uid, job.generation);

        net_tls_cancel_bind(&s_playlist_load_cancel);
        if (job.kind == NET_PLAYLIST_LOAD_MY) {
            int status = 0;
            if (ym_api_playlists_list(&api_ctx, job.uid,
                                      &result.entries,
                                      &result.count, &status) == 0) {
                PlaylistEntry liked;
                if (ym_api_playlist_metadata(&api_ctx, job.uid,
                                             PLAYLIST_KIND_LIKED_TRACKS,
                                             &liked, &status) != 0) {
                    free(result.entries);
                    result.entries = NULL;
                    result.count = 0;
                    result.error_code = status;
                    goto playlist_load_publish;
                }
                PlaylistEntry *grown = (PlaylistEntry *)realloc(
                    result.entries,
                    (size_t)(result.count + 1) * sizeof(*result.entries));
                if (!grown) {
                    free(result.entries);
                    result.entries = NULL;
                    result.count = 0;
                    result.error_code = NET_LOAD_ERR_INTERNAL;
                    goto playlist_load_publish;
                }
                result.entries = grown;
                memmove(result.entries + 1, result.entries,
                        (size_t)result.count * sizeof(*result.entries));
                result.entries[0] = liked;
                result.count++;
                result.rc = 0;
                result.error_code = 0;
            } else {
                result.error_code = status;
            }
        } else if (job.kind == NET_PLAYLIST_LOAD_LIKED) {
            int status = 0;
            if (ym_api_liked_playlists(&api_ctx, job.uid,
                                       &result.entries,
                                       &result.count, &status) == 0) {
                result.rc = 0;
                result.error_code = 0;
            } else {
                result.error_code = status;
            }
        } else {
            result.error_code = NET_LOAD_ERR_INTERNAL;
        }

playlist_load_publish:
        net_tls_cancel_unbind();
        playlist_load_lock();
        free(s_playlist_load_result.entries);
        memcpy(&s_playlist_load_result, &result, sizeof(result));
        s_playlist_load_busy = 0;
        playlist_load_unlock();

        logLine("playlist_load: done kind=%d rc=%d count=%d generation=%d\n",
                result.kind, result.rc, result.count, result.generation);
    }

    logLine("playlist_load: worker stopped\n");
    return 0;
}

static int artist_load_worker(SceSize args __attribute__((unused)), void *argp __attribute__((unused)))
{
    logLine("artist_load: worker started\n");

    while (s_artist_load_running) {
        if (sceKernelWaitSema(s_artist_load_sema, 1, NULL) < 0) {
            break;
        }
        if (!s_artist_load_running) {
            break;
        }

        ArtistLoadJob job;
        memset(&job, 0, sizeof(job));

        artist_load_lock();
        if (s_artist_load_has_job) {
            memcpy(&job, &s_artist_load_job, sizeof(job));
            s_artist_load_has_job = 0;
            s_artist_load_busy = 1;
        }
        artist_load_unlock();

        if (!job.token[0]) {
            artist_load_lock();
            s_artist_load_busy = 0;
            artist_load_unlock();
            continue;
        }

        ArtistLoadResult result;
        memset(&result, 0, sizeof(result));
        result.ready = 1;
        result.kind = job.kind;
        result.generation = job.generation;
        result.rc = -1;
        result.error_code = -2;

        net_tls_cancel_bind(&s_artist_load_cancel);
        if (job.kind == NET_ARTIST_LOAD_LIKED && job.uid > 0) {
            result.rc = net_client_fetch_liked_artists(job.token,
                                                       job.uid,
                                                       result.artists,
                                                       MAX_LIKED_ARTISTS,
                                                       &result.count);
            result.error_code = result.rc;
        } else if (job.kind == NET_ARTIST_LOAD_BRIEF && job.artist_id > 0) {
            result.rc = net_client_fetch_artist_brief_info(job.token,
                                                           job.artist_id,
                                                           &result.brief);
            result.error_code = result.rc;
        } else {
            result.error_code = -3;
        }
        net_tls_cancel_unbind();

        memset(job.token, 0, sizeof(job.token));

        artist_load_lock();
        memcpy(&s_artist_load_result, &result, sizeof(result));
        s_artist_load_busy = 0;
        artist_load_unlock();

        logLine("artist_load: done kind=%d rc=%d count=%d generation=%d\n",
                result.kind, result.rc, result.count, result.generation);
    }

    logLine("artist_load: worker stopped\n");
    return 0;
}

void net_client_get_apctl_info(NetApctlInfo *out)
{
    NetStackSnapshot link_before;
    NetStackSnapshot link_after;
    union SceNetApctlInfo info;
    union SceNetApctlInfo infoLen;
    int rc;
    int ssid_len = 0;

    if (!out) {
        return;
    }

    out->profile[0]  = '\0';
    out->ssid[0]     = '\0';
    out->bssid[0]    = '\0';
    out->ip[0]       = '\0';
    out->subnet[0]   = '\0';
    out->gateway[0]  = '\0';
    out->dns1[0]     = '\0';
    out->dns2[0]     = '\0';
    out->strength      = 0;
    out->channel       = 0;
    out->security_type = 0;
    out->valid         = 0;

    if (net_stack_get_snapshot(&link_before) < 0) return;
    if (link_before.apctl_state != PSP_NET_APCTL_STATE_GOT_IP) {
        return;
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROFILE_NAME, &info);
    if (rc >= 0) {
        int i;
        for (i = 0; i < 63 && info.name[i]; i++) {
            out->profile[i] = info.name[i];
        }
        out->profile[i] = '\0';
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID_LENGTH, &infoLen);
    if (rc >= 0) {
        ssid_len = (int)infoLen.ssidLength;
    }
    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID, &info);
    if (rc >= 0) {
        int copy_len = ssid_len;
        if (copy_len < 0)  { copy_len = 0; }
        if (copy_len > 32) { copy_len = 32; }
        if (copy_len > 0) {
            int i;
            for (i = 0; i < copy_len; i++) {
                out->ssid[i] = info.ssid[i];
            }
        }
        out->ssid[copy_len] = '\0';
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_BSSID, &info);
    if (rc >= 0) {
        snprintf(out->bssid, sizeof(out->bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
            info.bssid[0], info.bssid[1], info.bssid[2],
            info.bssid[3], info.bssid[4], info.bssid[5]);
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SECURITY_TYPE, &info);
    if (rc >= 0) {
        out->security_type = info.securityType;
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_STRENGTH, &info);
    if (rc >= 0) {
        out->strength = info.strength;
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_CHANNEL, &info);
    if (rc >= 0) {
        out->channel = info.channel;
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info);
    if (rc >= 0) {
        int i;
        for (i = 0; i < 15 && info.ip[i]; i++) {
            out->ip[i] = info.ip[i];
        }
        out->ip[i] = '\0';
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SUBNETMASK, &info);
    if (rc >= 0) {
        int i;
        for (i = 0; i < 15 && info.subNetMask[i]; i++) {
            out->subnet[i] = info.subNetMask[i];
        }
        out->subnet[i] = '\0';
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_GATEWAY, &info);
    if (rc >= 0) {
        int i;
        for (i = 0; i < 15 && info.gateway[i]; i++) {
            out->gateway[i] = info.gateway[i];
        }
        out->gateway[i] = '\0';
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PRIMDNS, &info);
    if (rc >= 0) {
        int i;
        for (i = 0; i < 15 && info.primaryDns[i]; i++) {
            out->dns1[i] = info.primaryDns[i];
        }
        out->dns1[i] = '\0';
    }

    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SECDNS, &info);
    if (rc >= 0) {
        int i;
        for (i = 0; i < 15 && info.secondaryDns[i]; i++) {
            out->dns2[i] = info.secondaryDns[i];
        }
        out->dns2[i] = '\0';
    }
    if (net_stack_get_snapshot(&link_after) < 0) return;
    if (link_after.apctl_state == PSP_NET_APCTL_STATE_GOT_IP &&
        link_after.generation == link_before.generation)
        out->valid = 1;
}

int net_client_get_apctl_strength(unsigned int *out_strength,
                                  unsigned int *out_generation)
{
    NetStackSnapshot before;
    NetStackSnapshot after;
    union SceNetApctlInfo info;
    if (!out_strength || !out_generation ||
        net_stack_get_snapshot(&before) < 0 ||
        before.apctl_state != PSP_NET_APCTL_STATE_GOT_IP)
        return -1;
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_STRENGTH, &info) < 0)
        return -1;
    if (net_stack_get_snapshot(&after) < 0 ||
        after.apctl_state != PSP_NET_APCTL_STATE_GOT_IP ||
        after.generation != before.generation)
        return -1;
    *out_strength = info.strength;
    *out_generation = after.generation;
    return 0;
}

int net_client_init(int profile_id)
{
    int rc;

    if (s_net_started) {
        return 0;
    }

    logLine("net: init start\n");

    // Конфигурация сети с профилем Wi-Fi
    NetTlsNetworkConfig network_config = {
        .pool_size = 128 * 1024,
        .callout_prio = 42,
        .callout_stack = 4 * 1024,
        .netintr_prio = 42,
        .netintr_stack = 4 * 1024,
        .apctl_stack_size = 0x1800,
        .apctl_priority = 48,
        .wifi_profile_id = profile_id
    };

    // Инициализация сети PSP
    rc = net_stack_init(&network_config);
    if (rc < 0) {
        logLine("net: network init failed %d\n", rc);
        s_net_error = 1;
        if (rc == NET_STACK_ERR_FATAL_LIVE_RUNTIME)
            s_process_exit_required = 1;
        return rc;
    }
    logLine("net: network init ok\n");

    // Конфигурация TLS
    NetTlsConfig tls_config = {
        .verify_mode = MBEDTLS_SSL_VERIFY_NONE,  // Для разработки
        .debug_threshold = 0,  // Отладка mbedTLS (0 = отключено, можно включить для отладки)
        .enable_verbose_logging = 0  // Отключить детальное логирование в hot path (логирует только ошибки)
    };

    // Инициализация TLS конфигурации
    rc = net_tls_config_init(&tls_config);
    if (rc < 0) {
        logLine("net: tls config init failed %d\n", rc);
        /* APCTL and its handler are already live. Their teardown/quiescence
         * contract is not documented, so leave the entire graph intact and
         * require top-level process exit. */
        s_process_exit_required = 1;
        s_net_error = 1;
        return rc;
    }
    logLine("net: tls config init ok\n");

    s_net_started = 1;
    s_net_error = 0;
    s_process_exit_required = 0;
    s_logged_net_state = -1;
    s_net_wait_logged = 0;
    s_net_info_logged = 0;
    logLine("net: init end ok\n");
    return 0;
}

int net_client_poll(void)
{
    int state;
    NetStackSnapshot snapshot;
    if (!s_net_started) {
        return -1;
    }
    if (!s_net_wait_logged) {
        logLine("net: wait for connection\n");
        s_net_wait_logged = 1;
    }
    if (net_stack_get_snapshot(&snapshot) < 0) {
        s_net_error = 1;
        s_process_exit_required = 1;
        return -1;
    }
    state = snapshot.apctl_state;
    if (state != s_logged_net_state) {
        s_logged_net_state = state;
        static const char *const s_apctl_names[] = {
            "DISCONNECTED", "SCANNING", "JOINING",
            "GETTING_IP", "GOT_IP", "EAP_AUTH", "KEY_EXCHANGE"
        };
        const char *sname = (state >= 0 && state <= 6) ? s_apctl_names[state] : "UNKNOWN";
        logLine("net: state %d (%s)\n", state, sname);
    }
    if (state == PSP_NET_APCTL_STATE_GOT_IP) {
        if (!s_net_info_logged) {
            logLine("net: got IP\n");
            logLine("net: wait end ok\n");
            s_net_info_logged = 1;
        }
    } else {
        s_net_info_logged = 0;
    }
    return state;
}

int net_client_is_ready(void)
{
    NetStackSnapshot snapshot;
    if (net_stack_get_snapshot(&snapshot) < 0) return 0;
    return s_net_started &&
           snapshot.apctl_state == PSP_NET_APCTL_STATE_GOT_IP &&
           net_tls_config_is_ready();
}

int net_client_runtime_started(void)
{
    return s_net_started;
}

int net_client_has_error(void)
{
    return s_net_error || net_stack_is_stuck();
}

int net_client_requires_process_exit(void)
{
    return s_process_exit_required;
}

int net_client_get_state(void)
{
    NetStackSnapshot snapshot;
    if (net_stack_get_snapshot(&snapshot) < 0) return -1;
    return snapshot.apctl_state;
}

int net_client_shutdown(void)
{
    int quiesced = 1;
    if (!s_net_started) {
        return 0;
    }
    
    logLine("net: shutdown start\n");
    if (net_tls_quiesce_wait(3000000U) < 0) {
        logLine("net: live TLS owner; complete runtime retained for process exit\n");
        quiesced = 0;
    }
    if (net_stack_stop_supervisor() < 0) quiesced = 0;
    /* The top-level caller exits immediately after this quiescence phase.
     * Network/TLS/DNS/handler state has process lifetime and is OS-reclaimed. */
    logLine("net: consumers quiesced; network graph retained for OS reclaim\n");
    return quiesced ? 0 : -1;
}

int net_client_playlist_load_init(void)
{
    if (s_playlist_load_running) {
        return 0;
    }

    memset(&s_playlist_load_job, 0, sizeof(s_playlist_load_job));
    memset(&s_playlist_load_result, 0, sizeof(s_playlist_load_result));
    s_playlist_load_has_job = 0;
    s_playlist_load_busy = 0;
    s_playlist_load_cancel = 0;

    if (sceKernelCreateLwMutex(&s_playlist_load_mutex, "playlist_load", 0, 0, NULL) >= 0) {
        s_playlist_load_mutex_initialized = 1;
    }

    s_playlist_load_sema = sceKernelCreateSema("playlist_load", 0, 0, 16, NULL);
    if (s_playlist_load_sema < 0) {
        if (s_playlist_load_mutex_initialized) {
            sceKernelDeleteLwMutex(&s_playlist_load_mutex);
            s_playlist_load_mutex_initialized = 0;
        }
        return -1;
    }

    s_playlist_load_running = 1;
    s_playlist_load_thread = sceKernelCreateThread("playlist_load", playlist_load_worker, 0x18, 0x10000, 0, NULL);
    if (s_playlist_load_thread < 0) {
        s_playlist_load_running = 0;
        sceKernelDeleteSema(s_playlist_load_sema);
        s_playlist_load_sema = -1;
        if (s_playlist_load_mutex_initialized) {
            sceKernelDeleteLwMutex(&s_playlist_load_mutex);
            s_playlist_load_mutex_initialized = 0;
        }
        return -1;
    }

    if (sceKernelStartThread(s_playlist_load_thread, 0, NULL) < 0) {
        s_playlist_load_running = 0;
        sceKernelDeleteThread(s_playlist_load_thread);
        s_playlist_load_thread = -1;
        sceKernelDeleteSema(s_playlist_load_sema);
        s_playlist_load_sema = -1;
        if (s_playlist_load_mutex_initialized) {
            sceKernelDeleteLwMutex(&s_playlist_load_mutex);
            s_playlist_load_mutex_initialized = 0;
        }
        return -1;
    }

    return 0;
}

int net_client_playlist_load_quiesce(void)
{
    s_playlist_load_cancel = 1;
    s_playlist_load_running = 0;
    if (s_playlist_load_sema >= 0) {
        sceKernelSignalSema(s_playlist_load_sema, 1);
    }
    if (s_playlist_load_thread >= 0) {
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_playlist_load_thread, &timeout_us) < 0) {
            logLine("playlist_load: worker still active; resources retained\n");
            return -1;
        }
    }
    return 0;
}

int net_client_playlist_load_shutdown(void)
{
    if (net_client_playlist_load_quiesce() < 0) return -1;
    if (s_playlist_load_thread >= 0) {
        sceKernelDeleteThread(s_playlist_load_thread);
        s_playlist_load_thread = -1;
    }
    if (s_playlist_load_sema >= 0) {
        sceKernelDeleteSema(s_playlist_load_sema);
        s_playlist_load_sema = -1;
    }
    if (s_playlist_load_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_playlist_load_mutex);
        s_playlist_load_mutex_initialized = 0;
    }

    s_playlist_load_has_job = 0;
    s_playlist_load_busy = 0;
    memset(&s_playlist_load_job, 0, sizeof(s_playlist_load_job));
    free(s_playlist_load_result.entries);
    memset(&s_playlist_load_result, 0, sizeof(s_playlist_load_result));
    return 0;
}

int net_client_playlist_load_start(NetPlaylistLoadKind kind, const char *token, int uid, int generation)
{
    if (!token || !token[0] || uid <= 0) {
        return -1;
    }
    if (!s_playlist_load_running || s_playlist_load_thread < 0 || s_playlist_load_sema < 0) {
        return -1;
    }
    if (kind != NET_PLAYLIST_LOAD_MY && kind != NET_PLAYLIST_LOAD_LIKED) {
        return -1;
    }

    PlaylistLoadJob job;
    memset(&job, 0, sizeof(job));
    job.kind = kind;
    strncpy(job.token, token, sizeof(job.token) - 1);
    job.uid = uid;
    job.generation = generation;

    playlist_load_lock();
    if (s_playlist_load_has_job || s_playlist_load_busy) {
        playlist_load_unlock();
        return 1;
    }
    memcpy(&s_playlist_load_job, &job, sizeof(job));
    s_playlist_load_has_job = 1;
    playlist_load_unlock();

    sceKernelSignalSema(s_playlist_load_sema, 1);
    return 0;
}

/* Index of the entry whose uuid matches, or 0 (uuid empty / not found).
   Lets the cursor survive a list reload by identity instead of ordinal. */
static int playlist_index_by_uuid(const PlaylistEntry *list, int count, const char *uuid)
{
    if (uuid && uuid[0]) {
        for (int i = 0; i < count; i++) {
            if (strcmp(list[i].uuid, uuid) == 0) {
                return i;
            }
        }
    }
    return 0;
}

void net_client_playlist_load_poll(struct AppState *state)
{
    if (!state) {
        return;
    }

    PlaylistLoadResult result;
    memset(&result, 0, sizeof(result));

    playlist_load_lock();
    if (s_playlist_load_result.ready) {
        memcpy(&result, &s_playlist_load_result, sizeof(result));
        memset(&s_playlist_load_result, 0, sizeof(s_playlist_load_result));
    }
    playlist_load_unlock();

    if (!result.ready) {
        return;
    }

    if (result.kind == NET_PLAYLIST_LOAD_MY) {
        if (state->playlists_load.generation != result.generation ||
            state->playlists_load.status != PLAYLIST_LOAD_LOADING) {
            free(result.entries);
            return;
        }
        if (result.rc == 0) {
            char keep_uuid[48];
            keep_uuid[0] = '\0';
            if (state->playlist_selected >= 0 && state->playlist_selected < state->playlist_count) {
                snprintf(keep_uuid, sizeof(keep_uuid), "%s", state->playlists[state->playlist_selected].uuid);
            }
            free(state->playlists);
            state->playlists = result.entries;
            state->playlist_count = result.count;
            state->playlist_capacity = result.count;
            result.entries = NULL;
            /* Restore cursor by playlist identity, not ordinal, so a refresh keeps
               it on the same playlist (scroll preserved) instead of jumping to top. */
            state->playlist_selected = playlist_index_by_uuid(state->playlists, result.count, keep_uuid);
            state->playlists_load.status = PLAYLIST_LOAD_READY;
            state->playlists_load.error_code = 0;
        } else {
            free(result.entries);
            state->playlists_load.status = PLAYLIST_LOAD_ERROR;
            state->playlists_load.error_code = result.error_code;
        }
    } else if (result.kind == NET_PLAYLIST_LOAD_LIKED) {
        if (state->liked_playlists_load.generation != result.generation ||
            state->liked_playlists_load.status != PLAYLIST_LOAD_LOADING) {
            free(result.entries);
            return;
        }
        if (result.rc == 0) {
            char keep_uuid[48];
            keep_uuid[0] = '\0';
            if (state->liked_playlist_selected >= 0 && state->liked_playlist_selected < state->liked_playlist_count) {
                snprintf(keep_uuid, sizeof(keep_uuid), "%s", state->liked_playlists[state->liked_playlist_selected].uuid);
            }
            free(state->liked_playlists);
            state->liked_playlists = result.entries;
            state->liked_playlist_count = result.count;
            state->liked_playlist_capacity = result.count;
            result.entries = NULL;
            state->liked_playlist_selected = playlist_index_by_uuid(state->liked_playlists, result.count, keep_uuid);
            state->liked_playlists_load.status = PLAYLIST_LOAD_READY;
            state->liked_playlists_load.error_code = 0;
        } else {
            free(result.entries);
            state->liked_playlists_load.status = PLAYLIST_LOAD_ERROR;
            state->liked_playlists_load.error_code = result.error_code;
        }
    } else {
        free(result.entries);
    }
}

int net_client_artist_load_init(void)
{
    if (s_artist_load_running) {
        return 0;
    }

    memset(&s_artist_load_job, 0, sizeof(s_artist_load_job));
    memset(&s_artist_load_result, 0, sizeof(s_artist_load_result));
    s_artist_load_has_job = 0;
    s_artist_load_busy = 0;
    s_artist_load_cancel = 0;

    if (sceKernelCreateLwMutex(&s_artist_load_mutex, "artist_load", 0, 0, NULL) >= 0) {
        s_artist_load_mutex_initialized = 1;
    }

    s_artist_load_sema = sceKernelCreateSema("artist_load", 0, 0, 16, NULL);
    if (s_artist_load_sema < 0) {
        if (s_artist_load_mutex_initialized) {
            sceKernelDeleteLwMutex(&s_artist_load_mutex);
            s_artist_load_mutex_initialized = 0;
        }
        return -1;
    }

    s_artist_load_running = 1;
    s_artist_load_thread = sceKernelCreateThread("artist_load", artist_load_worker, 0x18, 0x10000, 0, NULL);
    if (s_artist_load_thread < 0) {
        s_artist_load_running = 0;
        sceKernelDeleteSema(s_artist_load_sema);
        s_artist_load_sema = -1;
        if (s_artist_load_mutex_initialized) {
            sceKernelDeleteLwMutex(&s_artist_load_mutex);
            s_artist_load_mutex_initialized = 0;
        }
        return -1;
    }

    if (sceKernelStartThread(s_artist_load_thread, 0, NULL) < 0) {
        s_artist_load_running = 0;
        sceKernelDeleteThread(s_artist_load_thread);
        s_artist_load_thread = -1;
        sceKernelDeleteSema(s_artist_load_sema);
        s_artist_load_sema = -1;
        if (s_artist_load_mutex_initialized) {
            sceKernelDeleteLwMutex(&s_artist_load_mutex);
            s_artist_load_mutex_initialized = 0;
        }
        return -1;
    }

    return 0;
}

int net_client_artist_load_quiesce(void)
{
    s_artist_load_cancel = 1;
    s_artist_load_running = 0;
    if (s_artist_load_sema >= 0) {
        sceKernelSignalSema(s_artist_load_sema, 1);
    }
    if (s_artist_load_thread >= 0) {
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_artist_load_thread, &timeout_us) < 0) {
            logLine("artist_load: worker still active; resources retained\n");
            return -1;
        }
    }
    return 0;
}

int net_client_artist_load_shutdown(void)
{
    if (net_client_artist_load_quiesce() < 0) return -1;
    if (s_artist_load_thread >= 0) {
        sceKernelDeleteThread(s_artist_load_thread);
        s_artist_load_thread = -1;
    }
    if (s_artist_load_sema >= 0) {
        sceKernelDeleteSema(s_artist_load_sema);
        s_artist_load_sema = -1;
    }
    if (s_artist_load_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_artist_load_mutex);
        s_artist_load_mutex_initialized = 0;
    }

    s_artist_load_has_job = 0;
    s_artist_load_busy = 0;
    memset(&s_artist_load_job, 0, sizeof(s_artist_load_job));
    memset(&s_artist_load_result, 0, sizeof(s_artist_load_result));
    return 0;
}

static int artist_load_start(const ArtistLoadJob *job)
{
    if (!job || !job->token[0]) {
        return -1;
    }
    if (!s_artist_load_running || s_artist_load_thread < 0 || s_artist_load_sema < 0) {
        return -1;
    }

    artist_load_lock();
    if (s_artist_load_has_job || s_artist_load_busy) {
        artist_load_unlock();
        return 1;
    }
    memcpy(&s_artist_load_job, job, sizeof(*job));
    s_artist_load_has_job = 1;
    artist_load_unlock();

    sceKernelSignalSema(s_artist_load_sema, 1);
    return 0;
}

int net_client_liked_artists_load_start(const char *token, int uid, int generation)
{
    if (!token || !token[0] || uid <= 0) {
        return -1;
    }

    ArtistLoadJob job;
    memset(&job, 0, sizeof(job));
    job.kind = NET_ARTIST_LOAD_LIKED;
    strncpy(job.token, token, sizeof(job.token) - 1);
    job.uid = uid;
    job.generation = generation;
    return artist_load_start(&job);
}

int net_client_artist_brief_load_start(const char *token, int artist_id, int generation)
{
    if (!token || !token[0] || artist_id <= 0) {
        return -1;
    }

    ArtistLoadJob job;
    memset(&job, 0, sizeof(job));
    job.kind = NET_ARTIST_LOAD_BRIEF;
    strncpy(job.token, token, sizeof(job.token) - 1);
    job.artist_id = artist_id;
    job.generation = generation;
    return artist_load_start(&job);
}

static int artist_load_take_result(ArtistLoadResult *out, NetArtistLoadKind kind)
{
    if (!out) {
        return 0;
    }

    artist_load_lock();
    if (!s_artist_load_result.ready || s_artist_load_result.kind != kind) {
        artist_load_unlock();
        return 0;
    }
    memcpy(out, &s_artist_load_result, sizeof(*out));
    memset(&s_artist_load_result, 0, sizeof(s_artist_load_result));
    artist_load_unlock();
    return 1;
}

int net_client_liked_artists_load_poll(int *out_generation, int *out_rc,
                                       ArtistEntry *out_entries, int max_count, int *out_count)
{
    ArtistLoadResult result;
    memset(&result, 0, sizeof(result));
    if (!artist_load_take_result(&result, NET_ARTIST_LOAD_LIKED)) {
        return 0;
    }

    if (out_generation) *out_generation = result.generation;
    if (out_rc) *out_rc = result.rc;
    if (out_count) *out_count = result.count;
    if (out_entries && max_count > 0 && result.count > 0) {
        int copy_count = result.count < max_count ? result.count : max_count;
        memcpy(out_entries, result.artists, (size_t)copy_count * sizeof(ArtistEntry));
        if (out_count) *out_count = copy_count;
    }
    return 1;
}

int net_client_artist_brief_load_poll(int *out_generation, int *out_rc, ArtistBriefInfo *out)
{
    ArtistLoadResult result;
    memset(&result, 0, sizeof(result));
    if (!artist_load_take_result(&result, NET_ARTIST_LOAD_BRIEF)) {
        return 0;
    }

    if (out_generation) *out_generation = result.generation;
    if (out_rc) *out_rc = result.rc;
    if (out) {
        memcpy(out, &result.brief, sizeof(*out));
    }
    return 1;
}

int net_client_track_bootstrap_init(void)
{
    if (s_track_boot_running) {
        return 0;
    }

    track_meta_store_init();
    track_hydrator_init(track_hydrator_sink_cb);
    s_track_boot_cancel = 0;

    /* One-time eviction of the retired raw-JSON track cache. */
    while (fs_remove_siblings("data/cache/tracks", "tracks_", NULL) > 0) {
    }

    if (sceKernelCreateLwMutex(&s_track_boot_job_mutex, "track_boot_job", 0, 0, NULL) >= 0) {
        s_track_boot_job_mutex_initialized = 1;
    }
    if (sceKernelCreateLwMutex(&s_track_store_mutex, "track_store", 0, 0, NULL) >= 0) {
        s_track_store_mutex_initialized = 1;
    }

    s_track_boot_sema = sceKernelCreateSema("track_boot", 0, 0, 255, NULL);
    if (s_track_boot_sema < 0) {
        return -1;
    }

    s_track_boot_running = 1;
    s_track_boot_thread = sceKernelCreateThread("track_boot", track_boot_worker, 0x18, 0x18000, 0, NULL);
    if (s_track_boot_thread < 0) {
        s_track_boot_running = 0;
        sceKernelDeleteSema(s_track_boot_sema);
        s_track_boot_sema = -1;
        return -1;
    }

    if (sceKernelStartThread(s_track_boot_thread, 0, NULL) < 0) {
        s_track_boot_running = 0;
        sceKernelDeleteThread(s_track_boot_thread);
        s_track_boot_thread = -1;
        sceKernelDeleteSema(s_track_boot_sema);
        s_track_boot_sema = -1;
        return -1;
    }

    return 0;
}

int net_client_track_bootstrap_quiesce(void)
{
    int hydrator_rc;
    s_track_boot_cancel = 1;
    s_track_boot_running = 0;
    if (s_track_boot_sema >= 0) {
        sceKernelSignalSema(s_track_boot_sema, 1);
    }
    hydrator_rc = track_hydrator_quiesce();
    if (s_track_boot_thread >= 0) {
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_track_boot_thread, &timeout_us) < 0) {
            logLine("track_boot: worker still active; resources retained\n");
            return -1;
        }
    }
    return hydrator_rc;
}

int net_client_track_bootstrap_shutdown(void)
{
    if (net_client_track_bootstrap_quiesce() < 0) return -1;
    if (s_track_boot_thread >= 0) {
        sceKernelDeleteThread(s_track_boot_thread);
        s_track_boot_thread = -1;
    }
    if (s_track_boot_sema >= 0) {
        sceKernelDeleteSema(s_track_boot_sema);
        s_track_boot_sema = -1;
    }
    if (s_track_boot_job_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_track_boot_job_mutex);
        s_track_boot_job_mutex_initialized = 0;
    }
    if (track_hydrator_shutdown() < 0) return -1;
    track_meta_store_shutdown();
    if (s_track_store_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_track_store_mutex);
        s_track_store_mutex_initialized = 0;
    }
    s_track_boot_has_job = 0;
    memset(&s_track_boot_job, 0, sizeof(s_track_boot_job));
    return 0;
}

int net_client_track_bootstrap_start(const char *token, int uid, int playlist_kind, int revision, int generation, const char *uuid, struct AppState *state)
{
    if (!token || !token[0] || uid <= 0 || playlist_kind <= 0 || !state) {
        return -1;
    }
    if (!s_track_boot_running || s_track_boot_thread < 0 || s_track_boot_sema < 0) {
        return -1;
    }

    TrackBootstrapJob job;
    memset(&job, 0, sizeof(job));
    strncpy(job.token, token, sizeof(job.token) - 1);
    job.uid = uid;
    job.playlist_kind = playlist_kind;
    job.revision = revision;
    job.generation = generation;
    if (uuid && uuid[0]) {
        strncpy(job.uuid, uuid, sizeof(job.uuid) - 1);
        job.uuid[sizeof(job.uuid) - 1] = '\0';
    }
    job.state = state;

    net_client_track_store_lock();
    if (track_boot_is_stale(state, generation, playlist_kind)) {
        net_client_track_store_unlock();
        return -1;
    }
    state->track_boot.status = TRACK_BOOT_LOADING;
    state->track_boot.loaded_count = 0;
    state->track_boot.error_code = 0;
    state->track_store.ids_complete = 0;
    memset(state->track_store.window_valid, 0, sizeof(state->track_store.window_valid));
    state->track_store.window_start = 0;
    s_track_state = state;
    track_boot_log_mem("bootstrap_start", playlist_kind, 0, 0);
    net_client_track_store_unlock();

    track_boot_job_lock();
    memcpy(&s_track_boot_job, &job, sizeof(job));
    s_track_boot_has_job = 1;
    track_boot_job_unlock();

    sceKernelSignalSema(s_track_boot_sema, 1);
    return 0;
}

/* GET url, enforce HTTP 200, save body to log_path, fill *response.
 * Returns 0 on success — caller owns response and must call net_http_response_free.
 * Returns -1 on network error, non-200, or empty body — response already freed. */
static int s_get_and_log(const char *url, const char *token, int buf_size,
                          const char *log_path, NetHttpResponse *response)
{
    int ret;
    memset(response, 0, sizeof(*response));
    ret = http_request(&(HttpRequest){
        .method = HTTP_GET, .url = url, .token = token, .initial_buffer = buf_size
    }, response);
    if (ret != 0) {
        logLine("net: GET failed ret=%d\n", ret);
        return -1;
    }
    logLine("net: GET ok status=%d body=%d\n", response->status_code, response->body_size);
    if (response->status_code != 200 || !response->body || response->body_size <= 0) {
        logLine("net: non-200 or empty status=%d\n", response->status_code);
        net_http_response_free(response);
        return -1;
    }
    if (log_path) {
        fs_ensure_dir(log_path);
        SceUID fd = fs_open(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            fs_write(fd, response->body, response->body_size);
            fs_close(fd);
            logLine("net: saved %s (%d bytes)\n", log_path, response->body_size);
        }
    }
    return 0;
}

int net_client_fetch_user_info(const char *token, UserInfo *info)
{
    NetHttpResponse response;
    int ret = -1;

    if (!info || !token) {
        return -1;
    }

    logLine("net: fetch user info start\n");
    if (s_get_and_log("https://api.music.yandex.net/account/status", token,
                      HTTP_TLS_BUFFER_SMALL, "data/logs/account_status_response.json",
                      &response) != 0) {
        return -1;
    }
    ret = (api_parser_account_status(response.body, (size_t)response.body_size, info) == 0) ? 0 : -1;
    if (ret != 0) {
        logLine("net: failed to parse account status\n");
    }
    net_http_response_free(&response);
    return ret;
}


int net_client_build_cover_url(const char *cover_uri, const char *size, char *out_url, size_t out_size)
{
    if (!cover_uri || !cover_uri[0] || !out_url || out_size == 0) {
        if (out_url && out_size > 0) {
            out_url[0] = '\0';
        }
        return -1;
    }

    if (!size) {
        size = "30x30";  // Default size for thumbnails
    }

    char *out = out_url;
    size_t remaining = out_size - 1;

    // Add https:// prefix if missing
    if (strncmp(cover_uri, "https://", 8) != 0 && strncmp(cover_uri, "http://", 7) != 0) {
        if (remaining < 8) {
            goto overflow;
        }
        memcpy(out, "https://", 8);
        out += 8;
        remaining -= 8;
    }

    // Replace %% with size, write directly to out_url
    const char *p = cover_uri;
    while (*p) {
        if (p[0] == '%' && p[1] == '%') {
            size_t size_len = strlen(size);
            if (size_len > remaining) {
                goto overflow;
            }
            memcpy(out, size, size_len);
            out += size_len;
            remaining -= size_len;
            p += 2;
        } else {
            if (remaining == 0) {
                goto overflow;
            }
            *out++ = *p++;
            remaining--;
        }
    }
    *out = '\0';
    return 0;

overflow:
    out_url[0] = '\0';
    return -1;
}

int net_client_probe_liked_albums(const char *token, int uid)
{
    NetHttpResponse response;
    char url[256];

    if (!token || uid <= 0) {
        return -1;
    }
    snprintf(url, sizeof(url), "https://api.music.yandex.net/users/%d/likes/albums?rich=True", uid);
    url[sizeof(url) - 1] = '\0';
    if (s_get_and_log(url, token, HTTP_TLS_BUFFER_MEDIUM,
                      "data/logs/liked_albums_rich_response.json", &response) == 0) {
        net_http_response_free(&response);
    }
    snprintf(url, sizeof(url), "https://api.music.yandex.net/users/%d/likes/albums?rich=False", uid);
    url[sizeof(url) - 1] = '\0';
    if (s_get_and_log(url, token, HTTP_TLS_BUFFER_MEDIUM,
                      "data/logs/liked_albums_short_response.json", &response) == 0) {
        net_http_response_free(&response);
    }
    return 0;
}

int net_client_fetch_liked_artists(const char *token, int uid, ArtistEntry *out, int max_count, int *out_count)
{
    NetHttpResponse response;
    char url[256];

    if (!token || uid <= 0 || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    logLine("net: fetch liked artists start (uid=%d)\n", uid);
    snprintf(url, sizeof(url), "https://api.music.yandex.net/users/%d/likes/artists?with-timestamps=True", uid);
    url[sizeof(url) - 1] = '\0';
    if (s_get_and_log(url, token, HTTP_TLS_BUFFER_MEDIUM,
                      "data/logs/liked_artists_response.json", &response) != 0) {
        return -1;
    }
    int ret = (api_parser_liked_artists(response.body, (size_t)response.body_size, out, max_count, out_count) == 0) ? 0 : -1;
    if (ret == 0) {
        logLine("net: parsed %d liked artists\n", *out_count);
    } else {
        logLine("net: failed to parse liked artists\n");
    }
    net_http_response_free(&response);
    return ret;
}

int net_client_fetch_artist_brief_info(const char *token, int artist_id, ArtistBriefInfo *out)
{
    NetHttpResponse response;
    char url[128];

    if (!token || artist_id <= 0 || !out) {
        return -1;
    }

    logLine("net: fetch artist brief-info start (id=%d)\n", artist_id);
    snprintf(url, sizeof(url), "https://api.music.yandex.net/artists/%d/brief-info", artist_id);
    url[sizeof(url) - 1] = '\0';

    /* Save log file per artist_id so multiple artists don't overwrite each other */
    char log_path[64];
    snprintf(log_path, sizeof(log_path), "data/logs/artist_brief_info_%d_response.json", artist_id);
    log_path[sizeof(log_path) - 1] = '\0';

    if (s_get_and_log(url, token, HTTP_TLS_BUFFER_MEDIUM, log_path, &response) != 0) {
        return -1;
    }
    int ret = (api_parser_artist_brief_info(response.body, (size_t)response.body_size, out) == 0) ? 0 : -1;
    if (ret == 0) {
        logLine("net: artist brief-info ok: albums=%d also=%d popular_tracks=%d\n",
                out->album_count, out->also_album_count, out->popular_track_count);
    } else {
        logLine("net: failed to parse artist brief-info\n");
    }
    net_http_response_free(&response);
    return ret;
}
