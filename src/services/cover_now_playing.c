#include "services/cover_now_playing.h"

#include <malloc.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/audio_player.h"
#include "services/cover_storage.h"
#include "services/image_loader.h"
#include "services/net_client.h"
#include "services/net_http.h"
#include "services/net_tls.h"

#define COVER_W 200
#define COVER_H 200
#define COVER_BUF_SIZE (COVER_W * COVER_H * 4)
#define COVER_SIZE_NOW_PLAYING "200x200"
#define COVER_SLOT_COUNT 2

typedef enum {
    COVER_NOW_PLAYING_IDLE = 0,
    COVER_NOW_PLAYING_LOADING,
    COVER_NOW_PLAYING_READY,
    COVER_NOW_PLAYING_ERROR
} CoverNowPlayingState;

typedef enum {
    COVER_JOB_NONE = 0,
    COVER_JOB_CURRENT,
    COVER_JOB_PREFETCH
} CoverJobKind;

typedef struct {
    int album_id;
    char cover_uri[256];
    unsigned int token;
} CoverRequest;

typedef struct {
    NowPlayingCover cover;
    void *buffer;
} CoverSlot;

/* Exactly two decoded covers are retained: the published cover and one
 * neighbour. A current request always outranks speculative prefetch work. */
static CoverSlot s_slots[COVER_SLOT_COUNT];
static int s_published_slot = -1;
static CoverNowPlayingState s_state = COVER_NOW_PLAYING_IDLE;
static CoverRequest s_current_request;
static CoverRequest s_prefetch_request;
static unsigned int s_next_token = 0;
static int s_current_queued = 0;
static int s_prefetch_queued = 0;
static int s_wakeup_pending = 0;
static CoverJobKind s_worker_job = COVER_JOB_NONE;
static volatile int s_cancel_requested = 0;
static SceUID s_worker_thread = -1;
static SceUID s_worker_sema = -1;
static volatile int s_worker_running = 0;
static SceLwMutexWorkarea s_state_mutex;
static int s_state_mutex_initialized = 0;

static void state_lock(void)
{
    if (s_state_mutex_initialized) {
        sceKernelLockLwMutex(&s_state_mutex, 1, NULL);
    }
}

static void state_unlock(void)
{
    if (s_state_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_state_mutex, 1);
    }
}

static int request_matches(const CoverRequest *request, int album_id, const char *cover_uri)
{
    return request && request->token != 0 && request->album_id == album_id &&
           strcmp(request->cover_uri, cover_uri) == 0;
}

static void request_set(CoverRequest *request, int album_id, const char *cover_uri)
{
    memset(request, 0, sizeof(*request));
    request->album_id = album_id;
    request->token = ++s_next_token;
    strncpy(request->cover_uri, cover_uri, sizeof(request->cover_uri) - 1);
    request->cover_uri[sizeof(request->cover_uri) - 1] = '\0';
}

static int find_slot_unlocked(int album_id, const char *cover_uri)
{
    int i;
    for (i = 0; i < COVER_SLOT_COUNT; ++i) {
        if (s_slots[i].cover.loaded && s_slots[i].cover.album_id == album_id &&
            strcmp(s_slots[i].cover.cover_uri, cover_uri) == 0) {
            return i;
        }
    }
    return -1;
}

static int job_is_current_unlocked(CoverJobKind kind, const CoverRequest *request)
{
    if (!s_worker_running || !request || request->token == 0) return 0;
    if (kind == COVER_JOB_CURRENT) {
        return s_state == COVER_NOW_PLAYING_LOADING &&
               request_matches(&s_current_request, request->album_id, request->cover_uri) &&
               s_current_request.token == request->token;
    }
    if (kind == COVER_JOB_PREFETCH) {
        return request_matches(&s_prefetch_request, request->album_id, request->cover_uri) &&
               s_prefetch_request.token == request->token;
    }
    return 0;
}

static int playback_allows_cover_work(void)
{
    AudioPlayerSnapshot snapshot;

    audio_player_get_snapshot(&snapshot);
    /* PLAYING is published before the decoder submits its first PCM block.
     * A positive position is published only after that output completes, so
     * this gate keeps cover I/O behind actual audible playback. */
    return snapshot.state == AUDIO_PLAYER_PLAYING && snapshot.position_ms > 0;
}

static void signal_worker_if_allowed(void)
{
    int should_signal = 0;

    if (s_worker_sema < 0 || !playback_allows_cover_work()) return;

    state_lock();
    if (!s_wakeup_pending && (s_current_queued || s_prefetch_queued)) {
        s_wakeup_pending = 1;
        should_signal = 1;
    }
    state_unlock();

    if (should_signal) sceKernelSignalSema(s_worker_sema, 1);
}

static void set_current_error_if_current(const CoverRequest *request)
{
    state_lock();
    if (job_is_current_unlocked(COVER_JOB_CURRENT, request)) {
        s_state = COVER_NOW_PLAYING_ERROR;
    }
    state_unlock();
}

static int commit_decoded(CoverJobKind kind, const CoverRequest *request,
                          void *rgba_data, int w, int h, int stride_bytes)
{
    int slot;
    int row;
    int row_bytes = w * 4;

    if (!rgba_data || w <= 0 || h <= 0 || w > COVER_W || h > COVER_H ||
        stride_bytes < row_bytes) {
        return -1;
    }

    state_lock();
    if (!job_is_current_unlocked(kind, request)) {
        state_unlock();
        return 1;
    }

    slot = find_slot_unlocked(request->album_id, request->cover_uri);
    if (slot < 0) {
        slot = s_published_slot == 0 ? 1 : 0;
    }
    if (!s_slots[slot].buffer) {
        state_unlock();
        return -1;
    }

    for (row = 0; row < h; ++row) {
        memcpy((unsigned char *)s_slots[slot].buffer + (size_t)row * (size_t)row_bytes,
               (unsigned char *)rgba_data + (size_t)row * (size_t)stride_bytes,
               (size_t)row_bytes);
    }

    memset(&s_slots[slot].cover, 0, sizeof(s_slots[slot].cover));
    s_slots[slot].cover.rgba_data = s_slots[slot].buffer;
    s_slots[slot].cover.w = w;
    s_slots[slot].cover.h = h;
    s_slots[slot].cover.stride_bytes = row_bytes;
    s_slots[slot].cover.loaded = 1;
    s_slots[slot].cover.album_id = request->album_id;
    memcpy(s_slots[slot].cover.cover_uri, request->cover_uri,
           sizeof(s_slots[slot].cover.cover_uri));

    if (kind == COVER_JOB_CURRENT) {
        s_published_slot = slot;
        s_state = COVER_NOW_PLAYING_READY;
    }
    state_unlock();
    return 0;
}

static int worker_thread(SceSize args __attribute__((unused)), void *argp __attribute__((unused)))
{
    logLine("cover_now_playing: worker thread started\n");

    while (s_worker_running) {
        CoverRequest request;
        CoverJobKind kind = COVER_JOB_NONE;
        char cover_path[64];
        char cover_url[256];
        void *decoded_rgba = NULL;
        int w = 0;
        int h = 0;
        int stride_bytes = 0;
        int downloaded = 0;
        int commit_rc;
        SceUID fd;

        if (sceKernelWaitSema(s_worker_sema, 1, NULL) < 0 || !s_worker_running) break;

        state_lock();
        if (!s_worker_running) {
            state_unlock();
            break;
        }
        s_wakeup_pending = 0;
        memset(&request, 0, sizeof(request));
        if (s_current_queued) {
            request = s_current_request;
            s_current_queued = 0;
            kind = COVER_JOB_CURRENT;
        } else if (s_prefetch_queued) {
            request = s_prefetch_request;
            s_prefetch_queued = 0;
            kind = COVER_JOB_PREFETCH;
        }
        s_worker_job = kind;
        s_cancel_requested = 0;
        state_unlock();

        if (kind == COVER_JOB_NONE) continue;

        cover_storage_build_path(COVER_ENTITY_ALBUM, request.album_id,
                                 COVER_SIZE_NOW_PLAYING, cover_path, sizeof(cover_path));

        fd = fs_open(cover_path, PSP_O_RDONLY, 0);
        if (fd >= 0) {
            fs_close(fd);
        } else {
            if (net_client_build_cover_url(request.cover_uri, COVER_SIZE_NOW_PLAYING,
                                           cover_url, sizeof(cover_url)) != 0) {
                logLine("cover_now_playing: cover URL exceeds buffer album_id=%d\n",
                        request.album_id);
                if (kind == COVER_JOB_CURRENT) set_current_error_if_current(&request);
                goto job_done;
            }

            logLine("cover_now_playing: %s download album_id=%d url=%s\n",
                    kind == COVER_JOB_CURRENT ? "current" : "prefetch",
                    request.album_id, cover_url);
            net_tls_cancel_bind(&s_cancel_requested);
            if (http_stream(&(HttpRequest){ .method = HTTP_GET, .url = cover_url },
                            &(HttpSink){ .file_path = cover_path }) != 0) {
                fs_remove(cover_path);
                logLine("cover_now_playing: download failed album_id=%d\n", request.album_id);
                if (kind == COVER_JOB_CURRENT) set_current_error_if_current(&request);
                goto job_done;
            }
            downloaded = 1;
        }

        state_lock();
        commit_rc = job_is_current_unlocked(kind, &request) ? 0 : 1;
        state_unlock();
        if (commit_rc != 0) goto job_done;

        if (image_load_rgba8888(cover_path, &decoded_rgba, &w, &h, &stride_bytes) != 0) {
            logLine("cover_now_playing: decode failed album_id=%d path=%s\n",
                    request.album_id, cover_path);
            fs_remove(cover_path);
            if (kind == COVER_JOB_CURRENT) set_current_error_if_current(&request);
            goto job_done;
        }

        commit_rc = commit_decoded(kind, &request, decoded_rgba, w, h, stride_bytes);
        image_free_rgba8888(decoded_rgba);
        decoded_rgba = NULL;
        if (commit_rc == 0) {
            logLine("cover_now_playing: %s ready album_id=%d %dx%d%s\n",
                    kind == COVER_JOB_CURRENT ? "current" : "prefetch",
                    request.album_id, w, h, downloaded ? " downloaded" : " cached");
        } else if (commit_rc < 0 && kind == COVER_JOB_CURRENT) {
            set_current_error_if_current(&request);
        }

job_done:
        if (decoded_rgba) image_free_rgba8888(decoded_rgba);
        net_tls_cancel_unbind();
        state_lock();
        s_worker_job = COVER_JOB_NONE;
        state_unlock();
        signal_worker_if_allowed();
    }

    logLine("cover_now_playing: worker thread stopped\n");
    sceKernelExitThread(0);
    return 0;
}

int cover_now_playing_init(void)
{
    int i;
    int rc;

    memset(s_slots, 0, sizeof(s_slots));
    memset(&s_current_request, 0, sizeof(s_current_request));
    memset(&s_prefetch_request, 0, sizeof(s_prefetch_request));
    s_published_slot = -1;
    s_state = COVER_NOW_PLAYING_IDLE;
    s_next_token = 0;
    s_current_queued = 0;
    s_prefetch_queued = 0;
    s_wakeup_pending = 0;
    s_worker_job = COVER_JOB_NONE;
    s_cancel_requested = 0;

    for (i = 0; i < COVER_SLOT_COUNT; ++i) {
        s_slots[i].buffer = memalign(16, COVER_BUF_SIZE);
        if (!s_slots[i].buffer) break;
    }
    if (i != COVER_SLOT_COUNT) {
        int j;
        for (j = 0; j < COVER_SLOT_COUNT; ++j) {
            free(s_slots[j].buffer);
            s_slots[j].buffer = NULL;
        }
        logLine("cover_now_playing: two-slot buffer allocation failed\n");
        return -1;
    }

    if (sceKernelCreateLwMutex(&s_state_mutex, "np_cover_state", 0, 0, NULL) < 0)
        goto fail;
    s_state_mutex_initialized = 1;
    s_worker_sema = sceKernelCreateSema("np_cover_worker", 0, 0, 1, NULL);
    if (s_worker_sema < 0) goto fail;
    s_worker_running = 1;
    s_worker_thread = sceKernelCreateThread("np_cover_worker", worker_thread,
                                            0x18, 0x10000, 0, NULL);
    if (s_worker_thread < 0) goto fail;
    rc = sceKernelStartThread(s_worker_thread, 0, NULL);
    if (rc < 0) {
        logLine("cover_now_playing: worker start failed 0x%08X\n", rc);
        sceKernelDeleteThread(s_worker_thread);
        s_worker_thread = -1;
        goto fail;
    }
    logLine("cover_now_playing: initialized slots=%d bytes=%d\n",
            COVER_SLOT_COUNT, COVER_SLOT_COUNT * COVER_BUF_SIZE);
    return 0;

fail:
    s_worker_running = 0;
    if (s_worker_sema >= 0) {
        sceKernelDeleteSema(s_worker_sema);
        s_worker_sema = -1;
    }
    if (s_state_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_state_mutex);
        s_state_mutex_initialized = 0;
    }
    for (i = 0; i < COVER_SLOT_COUNT; ++i) {
        free(s_slots[i].buffer);
        s_slots[i].buffer = NULL;
    }
    return -1;
}

int cover_now_playing_quiesce(void)
{
    state_lock();
    s_worker_running = 0;
    s_cancel_requested = 1;
    state_unlock();
    if (s_worker_sema >= 0) sceKernelSignalSema(s_worker_sema, 1);

    if (s_worker_thread >= 0) {
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_worker_thread, &timeout_us) < 0) {
            logLine("cover_now_playing: worker still active; resources retained\n");
            return -1;
        }
    }
    return 0;
}

int cover_now_playing_shutdown(void)
{
    int i;
    if (cover_now_playing_quiesce() < 0) return -1;
    if (s_worker_thread >= 0) {
        sceKernelDeleteThread(s_worker_thread);
        s_worker_thread = -1;
    }
    if (s_worker_sema >= 0) {
        sceKernelDeleteSema(s_worker_sema);
        s_worker_sema = -1;
    }
    if (s_state_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_state_mutex);
        s_state_mutex_initialized = 0;
    }
    for (i = 0; i < COVER_SLOT_COUNT; ++i) {
        free(s_slots[i].buffer);
        s_slots[i].buffer = NULL;
        memset(&s_slots[i].cover, 0, sizeof(s_slots[i].cover));
    }
    logLine("cover_now_playing: shutdown\n");
    return 0;
}

void cover_now_playing_clear(void)
{
    state_lock();
    s_cancel_requested = 1;
    memset(&s_current_request, 0, sizeof(s_current_request));
    memset(&s_prefetch_request, 0, sizeof(s_prefetch_request));
    s_current_queued = 0;
    s_prefetch_queued = 0;
    s_published_slot = -1;
    s_state = COVER_NOW_PLAYING_IDLE;
    state_unlock();
}

int cover_now_playing_request_load(int album_id, const char *cover_uri)
{
    int slot;
    unsigned int token;

    if (!cover_uri || !cover_uri[0] || album_id == 0) return 0;

    state_lock();
    if (request_matches(&s_current_request, album_id, cover_uri)) {
        int result = s_state == COVER_NOW_PLAYING_LOADING ? 1 : 0;
        state_unlock();
        return result;
    }

    s_cancel_requested = 1;
    request_set(&s_current_request, album_id, cover_uri);
    token = s_current_request.token;
    memset(&s_prefetch_request, 0, sizeof(s_prefetch_request));
    s_prefetch_queued = 0;

    slot = find_slot_unlocked(album_id, cover_uri);
    if (slot >= 0) {
        s_published_slot = slot;
        s_current_queued = 0;
        s_state = COVER_NOW_PLAYING_READY;
        state_unlock();
        logLine("cover_now_playing: promoted cached album_id=%d token=%u\n", album_id, token);
        return 0;
    }

    s_current_queued = 1;
    s_state = COVER_NOW_PLAYING_LOADING;
    state_unlock();

    signal_worker_if_allowed();
    logLine("cover_now_playing: current queued album_id=%d token=%u\n", album_id, token);
    return 1;
}

void cover_now_playing_prefetch(int album_id, const char *cover_uri)
{
    unsigned int token;

    if (!cover_uri || !cover_uri[0] || album_id == 0) return;

    state_lock();
    if (find_slot_unlocked(album_id, cover_uri) >= 0 ||
        request_matches(&s_current_request, album_id, cover_uri) ||
        request_matches(&s_prefetch_request, album_id, cover_uri)) {
        state_unlock();
        return;
    }

    if (s_worker_job == COVER_JOB_PREFETCH) s_cancel_requested = 1;
    request_set(&s_prefetch_request, album_id, cover_uri);
    token = s_prefetch_request.token;
    s_prefetch_queued = 1;
    state_unlock();

    signal_worker_if_allowed();
    logLine("cover_now_playing: prefetch queued album_id=%d token=%u\n", album_id, token);
}

const NowPlayingCover *cover_now_playing_get_for(int album_id, const char *cover_uri)
{
    const NowPlayingCover *cover = NULL;

    if (!cover_uri || !cover_uri[0] || album_id == 0) return NULL;

    state_lock();
    if (s_published_slot >= 0 &&
        s_slots[s_published_slot].cover.loaded &&
        s_slots[s_published_slot].cover.album_id == album_id &&
        strcmp(s_slots[s_published_slot].cover.cover_uri, cover_uri) == 0) {
        cover = &s_slots[s_published_slot].cover;
    }
    state_unlock();
    return cover;
}

int cover_now_playing_is_loading(void)
{
    int loading;
    state_lock();
    loading = s_state == COVER_NOW_PLAYING_LOADING;
    state_unlock();
    return loading;
}

void cover_now_playing_process_pending(void)
{
    signal_worker_if_allowed();
}
