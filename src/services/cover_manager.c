#include "services/cover_manager.h"

#include <malloc.h>
#include <pspgu.h>
#include <pspthreadman.h>
#include <psptypes.h>
#include <stdlib.h>
#include <string.h>

#include <pspkernel.h>

#include "core/fs.h"
#include "core/logger.h"
#include "hal/hal_gfx_config.h"
#include "hal/hal_gpu.h"
#include "services/cover_cache.h"
#include "services/cover_storage.h"
#include "services/image_loader.h"
#include "services/cover_http_client.h"
#include "services/net_client.h"
#include "services/net_tls.h"
#include "services/resource_policy.h"

#define LOAD_QUEUE_SIZE 32
#define PENDING_UPLOADS_SIZE 32

typedef enum {
    LOAD_QUEUE_FREE = 0,
    LOAD_QUEUE_QUEUED = 1,
    LOAD_QUEUE_PROCESSING = 2
} LoadQueueState;

typedef struct {
    CoverEntityType entity_type;  // Тип сущности (плейлист или альбом)
    int entity_id;                 // ID плейлиста или альбома
    char cover_uri[256];
    char cover_size[16];  // "30x30" или "200x200"
    CoverPriority priority;
    u32 request_tick;
    LoadQueueState state;
} LoadRequest;

typedef struct {
    CoverEntityType entity_type;  // Тип сущности
    int entity_id;                 // ID сущности
    char cover_size[16];           // Размер ("30x30", "200x200")
    void *rgba_data;
    int w, h;
    int stride_bytes;
    int pending;
} PendingUpload;

static CoverHttpClient s_cover_http;
static CoverCache *s_cache = NULL;
static LoadRequest s_load_queue[LOAD_QUEUE_SIZE];
static PendingUpload s_pending_uploads[PENDING_UPLOADS_SIZE];
static SceLwMutexWorkarea s_queue_mutex;
static int s_queue_mutex_initialized = 0;
static SceLwMutexWorkarea s_pending_mutex;
static int s_pending_mutex_initialized = 0;
static SceUID s_worker_sema = -1;
static SceUID s_worker_thread = -1;
static volatile int s_worker_running = 0;
static volatile int s_cancel_requested = 0;
static u32 s_current_tick = 0;
static CoverLoadedCallback s_loaded_callback = NULL;
static void *s_callback_user_data = NULL;
static int s_last_visible_start = -1;
static int s_last_visible_end = -1;
// Сохраняем последние playlist_kind для сброса ref_count при выходе с экрана
static int s_last_visible_playlist_kinds[PLAYLIST_VISIBLE_SLOTS] = {0, 0, 0, 0, 0};
static int s_last_visible_count = 0;
/* Set to 1 by playback_controller when a critical fetch is in progress.
 * Worker re-queues the item and sleeps until resume signals the sema. */
static volatile int s_network_paused = 0;
/* Browser cover work should run only while browsing screens are active. */
static volatile int s_browsing_active = 1;

static void queue_lock(void)
{
    if (s_queue_mutex_initialized) {
        sceKernelLockLwMutex(&s_queue_mutex, 1, NULL);
    }
}

static void queue_unlock(void)
{
    if (s_queue_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_queue_mutex, 1);
    }
}

static void pending_lock(void)
{
    if (s_pending_mutex_initialized) {
        sceKernelLockLwMutex(&s_pending_mutex, 1, NULL);
    }
}

static void pending_unlock(void)
{
    if (s_pending_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_pending_mutex, 1);
    }
}

static void queue_release(int slot)
{
    if (slot < 0) {
        return;
    }
    queue_lock();
    s_load_queue[slot].state = LOAD_QUEUE_FREE;
    queue_unlock();
}

static int queue_has_queued_requests(void)
{
    int queued = 0;
    queue_lock();
    for (int i = 0; i < LOAD_QUEUE_SIZE; i++) {
        if (s_load_queue[i].state == LOAD_QUEUE_QUEUED) {
            queued = 1;
            break;
        }
    }
    queue_unlock();
    return queued;
}

static void close_cover_connection_if_batch_complete(void)
{
    if (!queue_has_queued_requests()) {
        cover_http_client_shutdown(&s_cover_http);
    }
}

static int worker_thread(SceSize args __attribute__((unused)), void *argp __attribute__((unused)))
{
    logLine("cover_manager: worker thread started\n");

    while (s_worker_running) {
        if (sceKernelWaitSema(s_worker_sema, 1, NULL) < 0) {
            break;
        }

        if (!s_browsing_active) {
            cover_http_client_shutdown(&s_cover_http);
            continue;
        }

        // Find highest priority task
        int best_slot = -1;
        CoverPriority best_priority = COVER_PRIORITY_BACKGROUND;
        u32 oldest_tick = 0xFFFFFFFF;

        queue_lock();
        for (int i = 0; i < LOAD_QUEUE_SIZE; i++) {
            if (s_load_queue[i].state != LOAD_QUEUE_QUEUED) {
                continue;
            }
            // Prefer higher priority (lower enum value)
            if (s_load_queue[i].priority < best_priority ||
                (s_load_queue[i].priority == best_priority &&
                 s_load_queue[i].request_tick < oldest_tick)) {
                best_slot = i;
                best_priority = s_load_queue[i].priority;
                oldest_tick = s_load_queue[i].request_tick;
            }
        }

        CoverEntityType entity_type = COVER_ENTITY_PLAYLIST;
        int entity_id = 0;
        char cover_uri[256] = {0};
        char cover_size_local[16] = {0};
        int queue_slot = -1;

        if (best_slot >= 0) {
            entity_type = s_load_queue[best_slot].entity_type;
            entity_id = s_load_queue[best_slot].entity_id;
            strncpy(cover_uri, s_load_queue[best_slot].cover_uri, sizeof(cover_uri) - 1);
            cover_uri[sizeof(cover_uri) - 1] = '\0';
            strncpy(cover_size_local, s_load_queue[best_slot].cover_size, sizeof(cover_size_local) - 1);
            cover_size_local[sizeof(cover_size_local) - 1] = '\0';
            s_load_queue[best_slot].state = LOAD_QUEUE_PROCESSING;
            queue_slot = best_slot;
        }
        queue_unlock();

        if (queue_slot < 0) {
            cover_http_client_shutdown(&s_cover_http);
            continue;
        }

        void *rgba_data = NULL;
        int w = 0, h = 0;
        int stride_bytes = 0;

        const char *entity_type_str = (entity_type == COVER_ENTITY_PLAYLIST) ? "playlist"
                                    : (entity_type == COVER_ENTITY_ARTIST)   ? "artist"
                                                                              : "album";
        logLine("cover_manager: worker processing %s id=%d priority=%d\n",
                entity_type_str, entity_id, best_priority);

        // Calibrated from current cover cache path format:
        // example "data/cache/covers/a2147483647_200x200" = 37 chars (theoretical max entity_id).
        // Real 2026-04-03 snapshot of data/cache/covers only had 30x30 entries (max 33 chars);
        // no 200x200 files existed yet to sample, hence the theoretical-max reasoning above.
        // 64 is enough for the current entity-id and size contract.
        char temp_file_rel[64];
        const char *size = cover_size_local[0] ? cover_size_local : "30x30";
        cover_storage_build_path(entity_type, entity_id, size, temp_file_rel, sizeof(temp_file_rel));

        // Check if file already exists (was downloaded previously but not in cache)
        SceUID fd_check = fs_open(temp_file_rel, PSP_O_RDONLY, 0);
        int need_download = 1;
        if (fd_check >= 0) {
            fs_close(fd_check);
            need_download = 0;
            logLine("cover_manager: worker file exists, skipping download %s id=%d\n", 
                    entity_type_str, entity_id);
        }

        /* If playback fetch is in progress, re-queue this item and block
         * on the semaphore until cover_manager_resume_network() wakes us. */
        if (need_download && s_network_paused) {
            logLine("cover_manager: network paused, re-queuing %s id=%d\n",
                    entity_type_str, entity_id);
            queue_lock();
            s_load_queue[queue_slot].state = LOAD_QUEUE_QUEUED;
            queue_unlock();
            cover_http_client_shutdown(&s_cover_http);
            continue;  /* go back to sceKernelWaitSema at top of loop */
        }

        if (need_download && !resource_policy_may_start(RESOURCE_CLASS_BROWSER_COVER)) {
            logLine("cover_manager: policy blocked download %s id=%d\n",
                    entity_type_str, entity_id);
            queue_lock();
            s_load_queue[queue_slot].state = LOAD_QUEUE_QUEUED;
            queue_unlock();
            cover_http_client_shutdown(&s_cover_http);
            continue;
        }

        // Download to file if needed (persistent keep-alive connection to avatars.yandex.net)
        if (need_download) {
            u64 t_dl0 = sceKernelGetSystemTimeWide();
            net_tls_cancel_bind(&s_cancel_requested);
            int dl_rc = cover_http_client_download(&s_cover_http, cover_uri, temp_file_rel);
            net_tls_cancel_unbind();
            u64 t_dl1 = sceKernelGetSystemTimeWide();
            logLine("cover_manager: download took %u us rc=%d\n", (unsigned)(t_dl1 - t_dl0), dl_rc);
            if (dl_rc != 0) {
                logLine("cover_manager: worker download failed %s id=%d\n", entity_type_str, entity_id);
                queue_release(queue_slot);
                close_cover_connection_if_batch_complete();
                continue;
            }
        }

        // Check if shutdown requested after download (can take 6+ seconds)
        if (!s_worker_running) {
            logLine("cover_manager: worker shutdown requested after download\n");
            queue_release(queue_slot);
            break;
        }

        // Decode to RAM (format detected by signature in image_load_rgba8888)
        u64 t_dec0 = sceKernelGetSystemTimeWide();
        int dec_rc = image_load_rgba8888(temp_file_rel, &rgba_data, &w, &h, &stride_bytes);
        u64 t_dec1 = sceKernelGetSystemTimeWide();
        logLine("cover_manager: decode took %u us rc=%d\n", (unsigned)(t_dec1 - t_dec0), dec_rc);
        if (dec_rc != 0) {
            logLine("cover_manager: worker decode failed %s id=%d\n", entity_type_str, entity_id);
            fs_remove(temp_file_rel);
            queue_release(queue_slot);
            close_cover_connection_if_batch_complete();
            continue;
        }

        // Check if shutdown requested after decode (can take ~100ms for large images)
        if (!s_worker_running) {
            logLine("cover_manager: worker shutdown requested after decode\n");
            image_free_rgba8888(rgba_data);
            queue_release(queue_slot);
            break;
        }

        // File is kept in cache, don't remove it

        // Add to pending queue
        int slot = -1;
        pending_lock();
        for (int j = 0; j < PENDING_UPLOADS_SIZE; j++) {
            if (!s_pending_uploads[j].pending) {
                slot = j;
                break;
            }
        }

        if (slot >= 0) {
            s_pending_uploads[slot].entity_type = entity_type;
            s_pending_uploads[slot].entity_id = entity_id;
            strncpy(s_pending_uploads[slot].cover_size, size,
                    sizeof(s_pending_uploads[slot].cover_size));
            s_pending_uploads[slot].cover_size[sizeof(s_pending_uploads[slot].cover_size) - 1] = '\0';
            s_pending_uploads[slot].rgba_data = rgba_data;
            s_pending_uploads[slot].w = w;
            s_pending_uploads[slot].h = h;
            s_pending_uploads[slot].stride_bytes = stride_bytes;
            s_pending_uploads[slot].pending = 1;
            logLine("cover_manager: worker SET PENDING slot=%d %s id=%d %dx%d\n",
                    slot, entity_type_str, entity_id, w, h);
            // Log memory after large cover decode (>= 200x200)
            if (w >= 200 || h >= 200) {
                int maxFree = sceKernelMaxFreeMemSize();
                logLine("mem: after cover decode %dx%d max_free=%d\n", w, h, maxFree);
            }
        } else {
            logLine("cover_manager: worker pending queue full, dropping %s id=%d\n",
                    entity_type_str, entity_id);
            image_free_rgba8888(rgba_data);
        }
        pending_unlock();
        resource_policy_cooperate(RESOURCE_CLASS_BROWSER_COVER, 1);
        queue_release(queue_slot);
        close_cover_connection_if_batch_complete();
    }

    /* This worker owns the persistent cover connection. Release its TLS
     * registry entry before reporting the worker joined, even when queued work
     * remains during process quiescence. */
    cover_http_client_shutdown(&s_cover_http);
    logLine("cover_manager: worker thread stopped\n");
    sceKernelExitThread(0);
    return 0;
}

void cover_manager_pause_network(void)
{
    s_network_paused = 1;
    logLine("cover_manager: network paused\n");
}

void cover_manager_set_browsing_active(int active)
{
    int new_active = active ? 1 : 0;
    if (s_browsing_active == new_active) {
        return;
    }

    s_browsing_active = new_active;
    logLine("cover_manager: browsing %s\n", s_browsing_active ? "active" : "paused");

    if (!s_browsing_active || s_worker_sema < 0) {
        return;
    }

    queue_lock();
    int n = 0;
    for (int i = 0; i < LOAD_QUEUE_SIZE; i++) {
        if (s_load_queue[i].state == LOAD_QUEUE_QUEUED) {
            n++;
        }
    }
    queue_unlock();
    if (n > 0) {
        sceKernelSignalSema(s_worker_sema, n);
    }
}

int cover_manager_is_browsing_active(void)
{
    return s_browsing_active ? 1 : 0;
}

void cover_manager_resume_network(void)
{
    s_network_paused = 0;
    logLine("cover_manager: network resumed\n");
    if (s_worker_sema < 0) {
        return;
    }
    /* Signal once per queued item so the worker can drain the backlog. */
    queue_lock();
    int n = 0;
    for (int i = 0; i < LOAD_QUEUE_SIZE; i++) {
        if (s_load_queue[i].state == LOAD_QUEUE_QUEUED) {
            n++;
        }
    }
    queue_unlock();
    if (n > 0) {
        sceKernelSignalSema(s_worker_sema, n);
    }
}

int cover_manager_init(void)
{
    int rc;
    cover_http_client_init(&s_cover_http);
    s_cache = NULL;
    cover_cache_init(&s_cache, 50);
    if (!s_cache) return -1;
    s_browsing_active = 1;

    if (sceKernelCreateLwMutex(&s_queue_mutex, "cover_queue", 0, 0, NULL) < 0)
        goto fail;
    s_queue_mutex_initialized = 1;
    if (sceKernelCreateLwMutex(&s_pending_mutex, "cover_pending", 0, 0, NULL) < 0)
        goto fail;
    s_pending_mutex_initialized = 1;
    s_worker_sema = sceKernelCreateSema("cover_worker", 0, 0, 255, NULL);
    if (s_worker_sema < 0) goto fail;

    s_last_visible_start = -1;
    s_last_visible_end = -1;
    s_cancel_requested = 0;

    s_worker_running = 1;
    s_worker_thread = sceKernelCreateThread("cover_worker", worker_thread, 0x18, 0x10000, 0, NULL);
    if (s_worker_thread < 0) goto fail;
    rc = sceKernelStartThread(s_worker_thread, 0, NULL);
    if (rc < 0) {
        logLine("cover_manager: worker start failed 0x%08X\n", rc);
        sceKernelDeleteThread(s_worker_thread);
        s_worker_thread = -1;
        goto fail;
    }
    logLine("cover_manager: initialized\n");
    return 0;

fail:
    s_worker_running = 0;
    if (s_worker_sema >= 0) {
        sceKernelDeleteSema(s_worker_sema);
        s_worker_sema = -1;
    }
    if (s_pending_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_pending_mutex);
        s_pending_mutex_initialized = 0;
    }
    if (s_queue_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_queue_mutex);
        s_queue_mutex_initialized = 0;
    }
    cover_cache_shutdown(s_cache);
    s_cache = NULL;
    return -1;
}

int cover_manager_quiesce(void)
{
    s_cancel_requested = 1;
    s_worker_running = 0;
    if (s_worker_sema >= 0) {
        sceKernelSignalSema(s_worker_sema, 1);
    }

    if (s_worker_thread >= 0) {
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_worker_thread, &timeout_us) < 0) {
            logLine("cover_manager: worker still active; resources retained\n");
            return -1;
        }
    }
    return 0;
}

int cover_manager_shutdown(void)
{
    if (cover_manager_quiesce() < 0) return -1;
    if (s_worker_thread >= 0) {
        sceKernelDeleteThread(s_worker_thread);
        s_worker_thread = -1;
    }

    if (s_worker_sema >= 0) {
        sceKernelDeleteSema(s_worker_sema);
        s_worker_sema = -1;
    }

    // Free pending uploads
    pending_lock();
    for (int i = 0; i < PENDING_UPLOADS_SIZE; i++) {
        if (s_pending_uploads[i].pending && s_pending_uploads[i].rgba_data) {
            image_free_rgba8888(s_pending_uploads[i].rgba_data);
            s_pending_uploads[i].pending = 0;
        }
    }
    pending_unlock();

    if (s_queue_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_queue_mutex);
        s_queue_mutex_initialized = 0;
    }
    if (s_pending_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_pending_mutex);
        s_pending_mutex_initialized = 0;
    }

    cover_cache_shutdown(s_cache);
    s_cache = NULL;
    cover_http_client_shutdown(&s_cover_http);
    logLine("cover_manager: shutdown\n");
    return 0;
}

void cover_manager_set_visible_range(int start_idx, int end_idx,
                                     const PlaylistEntry *playlists, int playlist_count)
{
    if (!s_cache) {
        return;
    }

    // Специальный случай: сброс ref_count при выходе с экрана (start_idx == 0 && end_idx == 0 && playlists == NULL)
    if (start_idx == 0 && end_idx == 0 && playlists == NULL) {
        // Сбрасываем ref_count для всех элементов предыдущего диапазона
        for (int i = 0; i < s_last_visible_count; i++) {
            if (s_last_visible_playlist_kinds[i] > 0) {
                cover_cache_unref(s_cache, COVER_ENTITY_PLAYLIST, s_last_visible_playlist_kinds[i], NULL);
            }
        }
        s_last_visible_count = 0;
        s_last_visible_start = -1;
        s_last_visible_end = -1;
        return;
    }

    if (!playlists || start_idx < 0 || end_idx > playlist_count) {
        return;
    }

    // Decrease ref_count for items that are no longer visible
    // (items that were in old range but not in new range)
    if (s_last_visible_start >= 0 && s_last_visible_end > s_last_visible_start) {
        for (int i = s_last_visible_start; i < s_last_visible_end; i++) {
            if (i < start_idx || i >= end_idx) {
                // This item is no longer visible
                cover_cache_unref(s_cache, COVER_ENTITY_PLAYLIST, playlists[i].playlist_id, NULL);
            }
        }
    }

    // Increase ref_count for newly visible items
    for (int i = start_idx; i < end_idx; i++) {
        if (i < s_last_visible_start || i >= s_last_visible_end) {
            // This item is newly visible
            cover_cache_ref(s_cache, COVER_ENTITY_PLAYLIST, playlists[i].playlist_id, NULL);
        }
    }

    // Сохраняем playlist_kind для текущего видимого диапазона (для сброса при выходе)
    s_last_visible_count = 0;
    for (int i = start_idx; i < end_idx && s_last_visible_count < PLAYLIST_VISIBLE_SLOTS; i++) {
        s_last_visible_playlist_kinds[s_last_visible_count++] = playlists[i].playlist_id;
    }

    // Update last visible range
    s_last_visible_start = start_idx;
    s_last_visible_end = end_idx;
}

void cover_manager_request_cover(CoverEntityType entity_type, int entity_id,
                                 const char *cover_uri, CoverPriority priority, const char *cover_size)
{
    const char *size = cover_size && cover_size[0] ? cover_size : "30x30";

    if (!s_cache || entity_id == 0 || !cover_uri || !cover_uri[0]) {
        return;
    }

    if (cover_cache_get(s_cache, entity_type, entity_id, size)) {
        return;
    }

    // Observed server URLs fit within 100 bytes; reject any result beyond this 256-byte buffer.
    char processed_uri[256];
    if (net_client_build_cover_url(cover_uri, size, processed_uri, sizeof(processed_uri)) != 0) {
        logLine("cover_manager: cover URL exceeds buffer type=%d id=%d size=%s\n",
                entity_type, entity_id, size);
        return;
    }

    // Check if already queued
    queue_lock();
    for (int i = 0; i < LOAD_QUEUE_SIZE; i++) {
        if (s_load_queue[i].state != LOAD_QUEUE_FREE &&
            s_load_queue[i].entity_type == entity_type &&
            s_load_queue[i].entity_id == entity_id &&
            strcmp(s_load_queue[i].cover_size[0] ? s_load_queue[i].cover_size : "30x30",
                   size) == 0) {
            queue_unlock();
            return;  // Already queued
        }
    }
    
    // Log only when actually adding new request
    const char *type_str = (entity_type == COVER_ENTITY_PLAYLIST) ? "playlist" : "album";
    logLine("cover_manager: requesting cover %s id=%d uri=%s priority=%d size=%s\n", 
            type_str, entity_id, processed_uri, priority, size);

    int slot = -1;
    for (int i = 0; i < LOAD_QUEUE_SIZE; i++) {
        if (s_load_queue[i].state == LOAD_QUEUE_FREE) {
            slot = i;
            break;
        }
    }

    if (slot >= 0) {
        s_current_tick++;
        s_load_queue[slot].entity_type = entity_type;
        s_load_queue[slot].entity_id = entity_id;
        size_t uri_len = strlen(processed_uri);
        if (uri_len >= sizeof(s_load_queue[slot].cover_uri)) {
            uri_len = sizeof(s_load_queue[slot].cover_uri) - 1;
        }
        memcpy(s_load_queue[slot].cover_uri, processed_uri, uri_len);
        s_load_queue[slot].cover_uri[uri_len] = '\0';
        size_t size_len = strlen(size);
        if (size_len >= sizeof(s_load_queue[slot].cover_size)) {
            size_len = sizeof(s_load_queue[slot].cover_size) - 1;
        }
        memcpy(s_load_queue[slot].cover_size, size, size_len);
        s_load_queue[slot].cover_size[size_len] = '\0';
        s_load_queue[slot].priority = priority;
        s_load_queue[slot].request_tick = s_current_tick;
        s_load_queue[slot].state = LOAD_QUEUE_QUEUED;

        if (s_worker_sema >= 0 && s_browsing_active) {
            sceKernelSignalSema(s_worker_sema, 1);
        }
    }
    queue_unlock();
}

static const CoverCacheEntry *cover_manager_get_cover(CoverEntityType entity_type, int entity_id,
                                                      const char *cover_size)
{
    if (!s_cache) {
        return NULL;
    }
    return cover_cache_get(s_cache, entity_type, entity_id, cover_size);
}

int cover_manager_draw_cover(CoverEntityType entity_type, int entity_id, const char *cover_size,
                              int dst_x, int dst_y, int max_w, int max_h)
{
    const CoverCacheEntry *entry = cover_manager_get_cover(entity_type, entity_id, cover_size);
    if (!entry || !entry->rgba_data) {
        return 0;
    }

    void *dst = hal_gpu_get_draw_buffer_cpu();
    int dst_stride = VRAM_BUFFER_WIDTH;
    if (!dst || dst_stride <= 0) {
        return 0;
    }

    int copy_w = entry->w < max_w ? entry->w : max_w;
    int copy_h = entry->h < max_h ? entry->h : max_h;

    int fb_w = SCREEN_WIDTH;
    int fb_h = SCREEN_HEIGHT;
    if (dst_x < 0 || dst_y < 0 || dst_x + copy_w > fb_w || dst_y + copy_h > fb_h) {
        return 0;
    }

    int src_stride_pixels = entry->stride_bytes / 4;
    hal_gpu_flush_cache_range(entry->rgba_data,
        (unsigned int)((size_t)entry->stride_bytes * (size_t)entry->h));
    hal_gpu_copy_image(GU_PSM_8888, 0, 0, copy_w, copy_h,
        src_stride_pixels, entry->rgba_data, dst_x, dst_y, dst_stride, dst);
    return 1;
}

int cover_manager_copy_cover(CoverEntityType entity_type, int entity_id, const char *cover_size,
                              void *dst_buf, int dst_buf_size,
                              int *out_w, int *out_h, int *out_stride_bytes)
{
    if (!dst_buf || dst_buf_size <= 0) {
        return 0;
    }

    const CoverCacheEntry *entry = cover_manager_get_cover(entity_type, entity_id, cover_size);
    if (!entry || !entry->rgba_data || entry->w <= 0 || entry->h <= 0) {
        return 0;
    }

    int row_bytes = entry->w * 4;
    if (row_bytes * entry->h > dst_buf_size) {
        return 0;
    }

    for (int row = 0; row < entry->h; row++) {
        memcpy((u8 *)dst_buf + row * row_bytes,
               (u8 *)entry->rgba_data + row * entry->stride_bytes,
               row_bytes);
    }

    if (out_w) *out_w = entry->w;
    if (out_h) *out_h = entry->h;
    if (out_stride_bytes) *out_stride_bytes = row_bytes;
    return 1;
}

int cover_manager_is_loading(CoverEntityType entity_type, int entity_id, const char *cover_size)
{
    if (!s_cache) {
        return 0;
    }
    if (cover_cache_get(s_cache, entity_type, entity_id, cover_size)) {
        return 0;  // Already loaded
    }

    const char *sz = cover_size && cover_size[0] ? cover_size : "30x30";

    // Check if in load queue
    queue_lock();
    for (int i = 0; i < LOAD_QUEUE_SIZE; i++) {
        if (s_load_queue[i].state != LOAD_QUEUE_FREE &&
            s_load_queue[i].entity_type == entity_type &&
            s_load_queue[i].entity_id == entity_id &&
            strcmp(s_load_queue[i].cover_size[0] ? s_load_queue[i].cover_size : "30x30", sz) == 0) {
            queue_unlock();
            return 1;  // Loading
        }
    }
    queue_unlock();

    // Check if in pending queue
    pending_lock();
    for (int i = 0; i < PENDING_UPLOADS_SIZE; i++) {
        if (s_pending_uploads[i].pending &&
            s_pending_uploads[i].entity_type == entity_type &&
            s_pending_uploads[i].entity_id == entity_id &&
            strcmp(s_pending_uploads[i].cover_size[0] ? s_pending_uploads[i].cover_size : "30x30", sz) == 0) {
            pending_unlock();
            return 1;  // Loading (decoded, waiting for process_pending)
        }
    }
    pending_unlock();

    return 0;  // Not loading
}

void cover_manager_process_pending(void)
{
    if (!s_cache || !s_browsing_active) {
        return;
    }

    // Process ALL pending uploads in one call instead of just one
    // This fixes slow VRAM upload bottleneck (was processing only 1 cover per frame)
    int processed = 0;
    int checked = 0;
    for (int batch = 0; batch < PENDING_UPLOADS_SIZE; batch++) {
        CoverEntityType entity_type = COVER_ENTITY_PLAYLIST;
        int entity_id = 0;
        char cover_size_pending[16] = {0};
        void *rgba_data = NULL;
        int w = 0, h = 0;
        int stride_bytes = 0;
        int slot = -1;

        pending_lock();
        for (int i = 0; i < PENDING_UPLOADS_SIZE; i++) {
            if (s_pending_uploads[i].pending) {
                entity_type = s_pending_uploads[i].entity_type;
                entity_id = s_pending_uploads[i].entity_id;
                strncpy(cover_size_pending, s_pending_uploads[i].cover_size, sizeof(cover_size_pending) - 1);
                cover_size_pending[sizeof(cover_size_pending) - 1] = '\0';
                rgba_data = s_pending_uploads[i].rgba_data;
                w = s_pending_uploads[i].w;
                h = s_pending_uploads[i].h;
                stride_bytes = s_pending_uploads[i].stride_bytes;
                slot = i;
                s_pending_uploads[i].pending = 0;
                checked++;
                break;
            }
        }
        pending_unlock();

        if (slot < 0) {
            break;  // No more pending uploads
        }

        // For artist covers: bake circle mask into RGBA data before caching.
        // Artist avatars are always displayed as circles; masking once at load
        // avoids any CPU/GE synchronisation issues at render time.
        if (entity_type == COVER_ENTITY_ARTIST) {
            u32 *px     = (u32 *)rgba_data;
            int st      = stride_bytes / 4;
            float cx    = w * 0.5f;
            float cy    = h * 0.5f;
            float r     = (w < h ? w : h) * 0.5f;
            float r2    = r * r;
            for (int iy = 0; iy < h; iy++) {
                for (int ix = 0; ix < w; ix++) {
                    float dx = (float)ix + 0.5f - cx;
                    float dy = (float)iy + 0.5f - cy;
                    if (dx * dx + dy * dy > r2) {
                        px[iy * st + ix] = 0xFF1A1A1A;
                    }
                }
            }
        }
        // Add to cache
        if (cover_cache_put(s_cache, entity_type, entity_id, cover_size_pending, rgba_data, w, h, stride_bytes) == 0) {
            processed++;
            if (s_loaded_callback) {
                s_loaded_callback(entity_type, entity_id, s_callback_user_data);
            }
        } else {
            // Cache full or error - free data
            image_free_rgba8888(rgba_data);
        }
    }
    
}

void cover_manager_set_loaded_callback(CoverLoadedCallback callback, void *user_data)
{
    s_loaded_callback = callback;
    s_callback_user_data = user_data;
}
