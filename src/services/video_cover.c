#include "services/video_cover.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/net_http.h"
#include "services/net_tls.h"

#ifndef VIDEO_COVER_PROXY_URL
#define VIDEO_COVER_PROXY_URL "https://192.168.100.204:8765/convert"
#endif

typedef struct {
    char track_id[40];
    char video_uri[192];
    unsigned int token;
} VideoCoverRequest;

static SceUID s_thread = -1;
static SceUID s_sema = -1;
static SceLwMutexWorkarea s_mutex;
static int s_mutex_ready = 0;
static volatile int s_running = 0;
static volatile int s_cancel_requested = 0;
static unsigned int s_token = 0;
static unsigned int s_dispatched_token = 0;
static VideoCoverRequest s_request;
static VideoCoverState s_state = VIDEO_COVER_IDLE;

static void lock_state(void)
{
    if (s_mutex_ready) sceKernelLockLwMutex(&s_mutex, 1, NULL);
}

static void unlock_state(void)
{
    if (s_mutex_ready) sceKernelUnlockLwMutex(&s_mutex, 1);
}

static unsigned int uri_hash(const char *text)
{
    unsigned int hash = 2166136261u;
    while (text && *text) {
        hash ^= (unsigned char)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static void build_path(const char *video_uri, const char *suffix,
                       char *out, size_t out_size)
{
    snprintf(out, out_size, "data/cache/video/%08x%s",
             uri_hash(video_uri), suffix ? suffix : "");
    out[out_size - 1] = '\0';
}

static int file_is_psmf(const char *path)
{
    unsigned char header[16];
    SceUID fd = fs_open(path, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    int read_count = fs_read(fd, header, sizeof(header));
    fs_close(fd);
    return read_count == (int)sizeof(header) && memcmp(header, "PSMF0014", 8) == 0 &&
           header[8] == 0 && header[9] == 0 && header[10] == 8 && header[11] == 0;
}

static int request_is_current(const VideoCoverRequest *request)
{
    int current;
    lock_state();
    current = s_running && request && request->token == s_request.token &&
              strcmp(request->track_id, s_request.track_id) == 0 &&
              strcmp(request->video_uri, s_request.video_uri) == 0;
    unlock_state();
    return current;
}

static void set_state_if_current(const VideoCoverRequest *request, VideoCoverState state)
{
    lock_state();
    if (s_running && request && request->token == s_request.token) s_state = state;
    unlock_state();
}

static int download_progress(int written, int content_length, void *user)
{
    VideoCoverRequest *request = (VideoCoverRequest *)user;
    (void)written;
    (void)content_length;
    if (!request_is_current(request)) return NET_HTTP_STREAM_CANCELLED;
    set_state_if_current(request, VIDEO_COVER_DOWNLOADING);
    return 0;
}

static int video_cover_worker(SceSize args __attribute__((unused)),
                              void *argp __attribute__((unused)))
{
    while (s_running) {
        VideoCoverRequest request;
        if (sceKernelWaitSema(s_sema, 1, NULL) < 0 || !s_running) break;

        lock_state();
        if (!s_running) {
            unlock_state();
            break;
        }
        if (!s_request.token || s_request.token == s_dispatched_token ||
            !s_request.track_id[0] || !s_request.video_uri[0]) {
            unlock_state();
            continue;
        }
        request = s_request;
        s_dispatched_token = request.token;
        s_cancel_requested = 0;
        unlock_state();

        char final_path[96];
        char part_path[96];
        char payload[256];
        build_path(request.video_uri, ".pmf", final_path, sizeof(final_path));
        build_path(request.video_uri, ".part", part_path, sizeof(part_path));

        if (file_is_psmf(final_path)) {
            set_state_if_current(&request, VIDEO_COVER_READY);
            continue;
        }

        fs_remove(part_path);
        int payload_len = snprintf(payload, sizeof(payload),
                                   "{\"url\":\"%s\"}", request.video_uri);
        if (payload_len <= 0 || payload_len >= (int)sizeof(payload)) {
            set_state_if_current(&request, VIDEO_COVER_ERROR);
            continue;
        }

        set_state_if_current(&request, VIDEO_COVER_CONVERTING);
        net_tls_cancel_bind(&s_cancel_requested);
        int rc = http_stream(
            &(HttpRequest){
                .method = HTTP_POST,
                .url = VIDEO_COVER_PROXY_URL,
                .accept = "video/psmf",
                .content_type = "application/json",
                .payload = payload,
                .payload_size = payload_len,
            },
            &(HttpSink){
                .file_path = part_path,
                .on_progress = download_progress,
                .user = &request,
            });
        net_tls_cancel_unbind();

        if (!request_is_current(&request)) {
            fs_remove(part_path);
            continue;
        }
        if (rc != 0 || !file_is_psmf(part_path)) {
            fs_remove(part_path);
            set_state_if_current(&request, VIDEO_COVER_ERROR);
            logLine("video_cover: proxy failed rc=%d track=%s\n", rc, request.track_id);
            continue;
        }

        fs_remove(final_path);
        if (fs_rename(part_path, final_path) != 0 || !file_is_psmf(final_path)) {
            fs_remove(part_path);
            set_state_if_current(&request, VIDEO_COVER_ERROR);
            continue;
        }
        set_state_if_current(&request, VIDEO_COVER_READY);
        logLine("video_cover: ready track=%s path=%s\n", request.track_id, final_path);
    }
    return 0;
}

int video_cover_init(void)
{
    if (s_running) return 0;
    memset(&s_request, 0, sizeof(s_request));
    s_state = VIDEO_COVER_IDLE;
    s_token = 0;
    s_dispatched_token = 0;
    if (sceKernelCreateLwMutex(&s_mutex, "video_cover", 0, 0, NULL) < 0) return -1;
    s_mutex_ready = 1;
    s_sema = sceKernelCreateSema("video_cover", 0, 0, 1, NULL);
    if (s_sema < 0) {
        sceKernelDeleteLwMutex(&s_mutex);
        s_mutex_ready = 0;
        return -1;
    }
    s_running = 1;
    s_thread = sceKernelCreateThread("video_cover", video_cover_worker,
                                    0x1A, 0x10000, 0, NULL);
    if (s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0) {
        s_running = 0;
        if (s_thread >= 0) {
            sceKernelDeleteThread(s_thread);
            s_thread = -1;
        }
        sceKernelDeleteSema(s_sema);
        s_sema = -1;
        sceKernelDeleteLwMutex(&s_mutex);
        s_mutex_ready = 0;
        return -1;
    }
    return 0;
}

int video_cover_quiesce(void)
{
    if (!s_running && s_thread < 0) return 0;
    lock_state();
    s_running = 0;
    s_cancel_requested = 1;
    unlock_state();
    if (s_sema >= 0) sceKernelSignalSema(s_sema, 1);
    if (s_thread >= 0) {
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_thread, &timeout_us) < 0) {
            logLine("video_cover: worker still active; resources retained\n");
            return -1;
        }
    }
    return 0;
}

int video_cover_shutdown(void)
{
    if (video_cover_quiesce() < 0) return -1;
    if (s_thread >= 0) {
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_sema >= 0) {
        sceKernelDeleteSema(s_sema);
        s_sema = -1;
    }
    if (s_mutex_ready) {
        sceKernelDeleteLwMutex(&s_mutex);
        s_mutex_ready = 0;
    }
    return 0;
}

void video_cover_clear(void)
{
    lock_state();
    s_cancel_requested = 1;
    s_token++;
    memset(&s_request, 0, sizeof(s_request));
    s_state = VIDEO_COVER_IDLE;
    unlock_state();
}

int video_cover_request(const char *track_id, const char *video_uri)
{
    if (!s_running || s_sema < 0) return -1;
    if (!track_id || !track_id[0] || !video_uri || !video_uri[0]) {
        video_cover_clear();
        return 0;
    }
    lock_state();
    if (strcmp(track_id, s_request.track_id) == 0 &&
        strcmp(video_uri, s_request.video_uri) == 0 &&
        (s_state == VIDEO_COVER_CONVERTING ||
         s_state == VIDEO_COVER_DOWNLOADING || s_state == VIDEO_COVER_READY)) {
        unlock_state();
        return 0;
    }
    s_cancel_requested = 1;
    s_token++;
    memset(&s_request, 0, sizeof(s_request));
    snprintf(s_request.track_id, sizeof(s_request.track_id), "%s", track_id);
    snprintf(s_request.video_uri, sizeof(s_request.video_uri), "%s", video_uri);
    s_request.token = s_token;
    s_state = VIDEO_COVER_CONVERTING;
    unlock_state();
    sceKernelSignalSema(s_sema, 1);
    return 1;
}

VideoCoverState video_cover_state_for(const char *track_id, const char *video_uri)
{
    VideoCoverState result = VIDEO_COVER_IDLE;
    if (!track_id || !video_uri) return result;
    lock_state();
    if (strcmp(track_id, s_request.track_id) == 0 &&
        strcmp(video_uri, s_request.video_uri) == 0) result = s_state;
    unlock_state();
    return result;
}

int video_cover_ready_path(const char *track_id, const char *video_uri,
                           char *out, size_t out_size)
{
    if (!out || out_size == 0 ||
        video_cover_state_for(track_id, video_uri) != VIDEO_COVER_READY) return 0;
    build_path(video_uri, ".pmf", out, out_size);
    return file_is_psmf(out);
}
