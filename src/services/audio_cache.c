#include "core/io_profile.h"
#include "services/audio_cache.h"

#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/logger.h"
#include "core/fs.h"
#include "services/net_tls.h"
#include "services/net_stack.h"
#include "services/net_http.h"
#include "services/audio_storage.h"
#include "services/cover_manager.h"
#include "services/ym_api.h"
#include "services/download_settings.h"
#include "services/mp4_track.h"
#include <mbedtls/aes.h>

#define AUDIO_CACHE_PROGRESSIVE_THRESHOLD (256 * 1024)
/* FLAC раньше стартовал с 256 КБ (~1-2 с аудио) и на медленной сети PSP
 * сразу подхватывал буферизацию. Ждём качнутое 10 с трека, прежде чем
 * отдавать источник плееру. Фолбэк — когда размер/длительность ещё не
 * известны: ~10 с типового FLAC (~1000 kbps). */
#define AUDIO_CACHE_FLAC_PREBUFFER_MS    10000
#define AUDIO_CACHE_FLAC_PREBUFFER_FALLBACK (1280 * 1024)
#define AUDIO_CACHE_SLOT_COUNT 2
#define AUDIO_CACHE_RETRY_MIN_US (250 * 1000)
#define AUDIO_CACHE_RETRY_MAX_US (4 * 1000 * 1000)

static const char *audio_cache_state_name(AudioCacheState state)
{
    switch (state) {
        case AUDIO_CACHE_DOWNLOADING:       return "DOWNLOADING";
        case AUDIO_CACHE_PROGRESSIVE_READY: return "PROGRESSIVE_READY";
        case AUDIO_CACHE_READY:             return "READY";
        case AUDIO_CACHE_ERROR:             return "ERROR";
        case AUDIO_CACHE_CANCELLED:         return "CANCELLED";
        default:                            return "IDLE";
    }
}

// Задание для постоянного воркера слота. Ящик держит не более одного
// задания: новое post перекрывает ещё не взятое старое (latest wins).
typedef struct {
    TrackEntry track;
    char       token[256];
    int        network_pause_held;
    DownloadSettings settings;
} AcJob;

typedef enum {
    AC_COMMIT_NONE,
    AC_COMMIT_PENDING,
    AC_COMMIT_IN_PROGRESS,
    AC_COMMIT_FAILED
} AcCommitState;

typedef struct AcSlot AcSlot;
struct AcSlot {
    int              slot_id;
    AudioCacheStatus status;
    DownloadSettings request_settings;
    char             request_track_id[TRACK_ID_SIZE];
    // Постоянный воркер: создаётся в init, удаляется только в shutdown.
    // Потоки НИКОГДА не удаляются на живом задании — отмена всегда
    // кооперативная через job_cancel (проброшен и в TLS-слой).
    SceUID           worker_thread;
    SceUID           job_sema;
    AcJob            pending_job;
    int              job_pending;       // под s_status_mutex
    volatile int     job_cancel;        // 1 = текущее задание отменено
    int              retire_requested;  /* protected by s_status_mutex */
    int              wake_pending;      /* at most one unconsumed wake */
    AcCommitState    commit_state;      /* protected by s_status_mutex */
    AcSlot          *wait_for;
    AcSlot          *self_arg;
};

#define AUDIO_CACHE_SETTINGS_PATH "config/audio_cache.cfg"
static const char kRetainOff[] = "retain=0\n";
static const char kRetainOn[] = "retain=1\n";
static unsigned int s_retain_downloads;

/* Loaded on the UI thread before workers start. Missing settings preserve
 * the existing temporary-cache policy. The buffer also detects extra bytes. */
static void load_cache_setting(void)
{
    char buffer[sizeof(kRetainOff)];
    SceUID fd = fs_open(AUDIO_CACHE_SETTINGS_PATH, PSP_O_RDONLY, 0777);
    int count;
    s_retain_downloads = 0;
    if (fd < 0) return;
    count = fs_read(fd, buffer, sizeof(buffer));
    fs_close(fd);
    if (count == sizeof(kRetainOn) - 1 &&
        memcmp(buffer, kRetainOn, sizeof(kRetainOn) - 1) == 0) {
        s_retain_downloads = 1;
    } else if (count != sizeof(kRetainOff) - 1 ||
               memcmp(buffer, kRetainOff, sizeof(kRetainOff) - 1) != 0) {
        logLine("ac: invalid retention setting\n");
    }
}

int audio_cache_retain_downloads(void)
{
    return __sync_fetch_and_add(&s_retain_downloads, 0U) != 0;
}

void audio_cache_set_retain_downloads(int enabled)
{
    unsigned int value = enabled != 0;
    if (audio_cache_retain_downloads() == (int)value) return;
    if (fs_write_deferred(AUDIO_CACHE_SETTINGS_PATH,
                          value ? kRetainOn : kRetainOff,
                          sizeof(kRetainOff) - 1) < 0) {
        logLine("ac: retention setting could not be queued\n");
        return;
    }
    (void)__sync_lock_test_and_set(&s_retain_downloads, value);
    logLine("ac: retain_downloads=%u queued\n", value);
}

static volatile int s_active_slot = 0;
static char s_hint_next[40];   /* id следующего трека очереди (под hint) */
static volatile int s_shutdown = 0;
static AcSlot s_slots[AUDIO_CACHE_SLOT_COUNT];
static SceLwMutexWorkarea s_status_mutex;
static int s_mutex_initialized = 0;
static AcSlot *s_source_watch_slot;
static AudioCacheSourceNotify s_source_notify;
static void *s_source_notify_user;

static void notify_source_locked(AcSlot *slot)
{
    if (slot == s_source_watch_slot && s_source_notify) {
        AudioCacheStatus status = slot->status;
        if (slot->retire_requested) status.state = AUDIO_CACHE_CANCELLED;
        s_source_notify(&status, s_source_notify_user);
    }
}

int audio_cache_watch_source(const char *track_id, AudioCacheSourceNotify notify,
                             void *user)
{
    int i, rc = -1;
    if (!s_mutex_initialized || !track_id || !notify) return -1;
    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (!s_source_notify) {
        for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
            AcSlot *slot = &s_slots[i];
            if (slot->status.source_ref_count > 0 &&
                strcmp(slot->status.track_id, track_id) == 0) {
                s_source_watch_slot = slot;
                s_source_notify = notify;
                s_source_notify_user = user;
                notify_source_locked(slot);
                rc = 0;
                break;
            }
        }
    }
    sceKernelUnlockLwMutex(&s_status_mutex, 1);
    return rc;
}

void audio_cache_unwatch_source(void *user)
{
    if (!s_mutex_initialized) return;
    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (s_source_notify_user == user) {
        s_source_watch_slot = NULL;
        s_source_notify = NULL;
        s_source_notify_user = NULL;
    }
    sceKernelUnlockLwMutex(&s_status_mutex, 1);
}

static AcSlot *active_slot(void)
{
    return &s_slots[s_active_slot ? 1 : 0];
}

static AcSlot *prefetch_slot(void)
{
    return &s_slots[s_active_slot ? 0 : 1];
}

static void slot_set_status(AcSlot *slot, AudioCacheState state,
                            const char *track_id, const char *path,
                            int error_code)
{
    AudioCacheState prev_state;

    if (!slot) return;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);

    prev_state = slot->status.state;
    slot->status.state = state;
    slot->status.error_code = error_code;
    if (track_id) {
        snprintf(slot->status.track_id, sizeof(slot->status.track_id), "%s", track_id);
    }
    if (path) {
        snprintf(slot->status.path, sizeof(slot->status.path), "%s", path);
    } else {
        slot->status.path[0] = '\0';
    }

    notify_source_locked(slot);
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    if (prev_state != state || error_code != 0) {
        logLine("ac: slot=%d state -> %s track_id='%s' err=%d\n",
                slot->slot_id, audio_cache_state_name(state),
                track_id ? track_id : "", error_code);
    }
}

static void slot_set_source(AcSlot *slot, AudioCacheState state,
                            const char *track_id, const char *path,
                            int64_t downloaded_bytes, int64_t content_length, int bitrate_kbps,
                            int complete, int progressive, int error_code,
                            AudioQuality quality, int from_disk_cache)
{
    AudioCacheState prev_state;

    if (!slot) return;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);

    prev_state = slot->status.state;
    slot->status.state = state;
    slot->status.error_code = error_code;
    slot->status.downloaded_bytes = downloaded_bytes;
    slot->status.content_length = content_length;
    slot->status.bitrate_kbps = bitrate_kbps;
    slot->status.quality = quality;
    slot->status.from_disk_cache = from_disk_cache;
    slot->status.complete = complete;
    slot->status.progressive = progressive;

    if (track_id) {
        snprintf(slot->status.track_id, sizeof(slot->status.track_id), "%s", track_id);
    }
    if (path) {
        snprintf(slot->status.path, sizeof(slot->status.path), "%s", path);
    } else {
        slot->status.path[0] = '\0';
    }

    notify_source_locked(slot);
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    if (prev_state != state || downloaded_bytes == 0) {
        logLine("ac: slot=%d source state -> %s track_id='%s' path='%s' bytes=%d/%d complete=%d progressive=%d\n",
                slot->slot_id, audio_cache_state_name(state), track_id ? track_id : "",
                path ? path : "", (int)downloaded_bytes, (int)content_length,
                complete, progressive);
    }
}

static void log_worker_priority(SceUID tid)
{
    SceKernelThreadRunStatus status;
    const int requested_priority = 0x18;

    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    if (sceKernelReferThreadRunStatus(tid, &status) == 0) {
        logLine("ac: worker priority tid=%d requested=0x%02X current=0x%02X\n",
                tid, requested_priority, status.currentPriority);
    } else {
        logLine("ac: worker priority tid=%d requested=0x%02X current=?\n",
                tid, requested_priority);
    }
}

static int ac_wait_for_retry(AcSlot *slot, const char *track_id, u64 retry_at)
{
    for (;;) {
        int same_track;
        u64 now;

        if (slot->job_cancel || s_shutdown) return NET_HTTP_STREAM_CANCELLED;
        if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
        same_track = strcmp(slot->status.track_id, track_id) == 0;
        if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
        if (!same_track) return NET_HTTP_STREAM_CANCELLED;

        now = sceKernelGetSystemTimeWide();
        if (net_stack_is_ready() && now >= retry_at) return 0;
        sceKernelDelayThread(100 * 1000);
    }
}

/* ---- FLAC lossless / AAC: файловые пути ----
 * RAM-буфер весь FLAC-трек не держит (20-40 МБ), поэтому FLAC идёт
 * файлом: raw fLaC пишется как есть, flac-mp4 качается как есть в
 * X.mp4 — плеер декодирует его напрямую (ym_flac mp4-режим), без
 * демукса и без ожидания полной закачки. AAC (aac-mp4) так же идёт
 * файлом: плеер читает MP4 на лету. MP3-путь ниже не тронут. */

/* Проба первых КБ потока: сырой fLaC начинается с "fLaC", flac-mp4 и
 * aac-mp4 — с ftyp; moov у Yandex faststart (лежит в первых КБ), но
 * не глубже 6 КБ — глубже проба срывается в mp3-фолбэк. */
#define MP4_PROBE_BYTES 16384
#define MP4_PROBE_NEED 6144

typedef struct {
    AcSlot *slot;
    int len;
    unsigned char buf[MP4_PROBE_BYTES];
} AcProbeCtx;

static int probe_chunk_cb(const char *data, int size, void *user)
{
    AcProbeCtx *p = (AcProbeCtx *)user;
    int room;
    if (!p || !data || size <= 0) return -1;
    if (p->slot->job_cancel || s_shutdown) {
        return NET_HTTP_STREAM_CANCELLED;
    }
    room = MP4_PROBE_BYTES - p->len;
    if (room <= 0) return NET_HTTP_STREAM_CANCELLED;
    if (size > room) size = room;
    memcpy(p->buf + p->len, data, (size_t)size);
    p->len += size;
    /* Достаточно — чистая остановка потока: любое иное ненулевое значение
     * http_stream считает ошибкой протокола (-21006). */
    return (p->len >= MP4_PROBE_NEED) ? NET_HTTP_STREAM_CANCELLED : 0;
}

/* Разбор STREAMINFO (первые 42 байта): 1 = rate/ch/bps годны. */
static int flac_probe_streaminfo(const unsigned char *p, int len,
                                 int *rate, int *ch, int *bps)
{
    unsigned type, mlen;
    if (!p || len < 42 || memcmp(p, "fLaC", 4) != 0) return 0;
    type = p[4] & 0x7F;
    mlen = ((unsigned)p[5] << 16) | ((unsigned)p[6] << 8) | (unsigned)p[7];
    if (type != 0 || mlen < 34) return 0;
    p += 8;
    *rate = ((int)p[10] << 12) | ((int)p[11] << 4) | ((int)p[12] >> 4);
    *ch = (((int)p[12] >> 1) & 7) + 1;
    *bps = ((((int)p[12] & 1) << 4) | ((int)p[13] >> 4)) + 1;
    return 1;
}

typedef struct {
    int is_raw_flac; /* поток начинается с "fLaC" */
    int is_mp4;      /* поток распознан как MP4 (flac-mp4 / aac-mp4) */
    Mp4ProbeInfo mp4;
} AcProbeResult;

/* Скачивает префикс url в RAM и определяет формат/кодек. 0 = распознано. */
static YmStatus ac_probe_stream(AcSlot *slot, const char *url, const char *token,
                           AcProbeResult *out)
{
    AcProbeCtx *p;
    int rc;

    if (!slot || !url || !token || !out) return YM_ERR_PARSE;
    memset(out, 0, sizeof(*out));
    /* Проба живёт только внутри вызова: два слота качают одновременно,
     * общий статический буфер давал гонку. */
    p = (AcProbeCtx *)calloc(1, sizeof(AcProbeCtx));
    if (!p) return YM_ERR_NO_MEMORY;
    p->slot = slot;

    rc = http_stream(&(HttpRequest){
                            .method = HTTP_GET, .url = url, .token = token,
                            .identity_encoding = 1,
                            .range_enabled = 1,
                            .range_start = 0,
                            .range_total = 0
                        },
                        &(HttpSink){ .on_chunk = probe_chunk_cb,
                                     .on_progress = NULL,
                                     .user = p });
    if (rc != 0 && rc != NET_HTTP_STREAM_CANCELLED) {
        free(p);
        return net_http_error_is_network(rc) ? YM_ERR_NETWORK : YM_ERR_HTTP;
    }
    if (slot->job_cancel || s_shutdown) {
        free(p);
        return YM_ERR_CANCELLED;
    }
    if (p->len >= 4 && memcmp(p->buf, "fLaC", 4) == 0) {
        out->is_raw_flac = flac_probe_streaminfo(p->buf, p->len,
                                                 &out->mp4.rate,
                                                 &out->mp4.ch, &out->mp4.bps);
        free(p);
        return out->is_raw_flac ? YM_OK : YM_ERR_UNSUPPORTED;
    }
    if (mp4_probe_buffer(p->buf, p->len, &out->mp4) == 0 &&
        out->mp4.kind != MP4_PROBE_NONE) {
        out->is_mp4 = 1;
        free(p);
        return YM_OK;
    }
    free(p);
    return YM_ERR_UNSUPPORTED;
}

typedef struct {
    AcSlot *slot;
    char track_id[40];
    SceUID fd;
    int64_t base_size;
    int64_t session_written;
    int64_t attempt_total;
    int attempt_had_progress;
    u64 last_speed_us;      /* периодический лог скорости закачки */
    int64_t last_speed_bytes;
    /* FLAC-пребафер: байты, после которых плееру можно отдать источник. */
    int is_flac;
    int bitrate_kbps;       /* из download-info, 0 если сервер не дал */
    int duration_ms;        /* длительность трека, 0 если неизвестна */
    int64_t progressive_need;
    /* Буфер между сетью и картой. Memory Stick на реальной PSP отвечает
     * секундами; без буфера приём из сети стоял на fs_write, TCP-окно
     * закрывалось и сервер рвал соединение (errno 104, 0 КБ/с). Теперь сеть
     * кладёт данные в ОЗУ-кольцо, отдельный поток пишет их в файл. */
    unsigned char *wbuf;
    size_t wb_head, wb_tail, wb_used;       /* под wmutex */
    SceLwMutexWorkarea wmutex;
    int wmutex_ok;
    SceUID wsema;
    SceUID wthread;
    volatile int wstop;                     /* дописать остаток и выйти */
    volatile int werror;
    volatile int64_t flushed;               /* байт записано в файл (сессия) */
    /* AES-128-CTR для encraw (flac): счётчик по абсолютной позиции файла. */
    int use_aes;
    mbedtls_aes_context aes;
    unsigned char aes_nonce[16];
    unsigned char aes_stream[16];
    size_t aes_off;
} AcFileDlCtx;

#define AC_WBUF_SIZE   (192 * 1024)
#define AC_WRITE_CHUNK (24 * 1024)
#define AC_WRITE_MIN   (12 * 1024)

/* Сколько байт дождаться перед стартом плеера. FLAC — 10 с трека:
 * 1) точно по размеру файла и длительности (когда content_length уже
 *    известен), 2) по битрейту, 3) константа-фолбэк. Порог не опускается
 *    ниже старого 256 КБ и не больше полного файла. */
static int64_t ac_prebuffer_need(const AcFileDlCtx *ctx)
{
    int64_t need;

    if (!ctx->is_flac) return AUDIO_CACHE_PROGRESSIVE_THRESHOLD;
    if (ctx->duration_ms > 0 && ctx->attempt_total > 0) {
        need = ctx->attempt_total * AUDIO_CACHE_FLAC_PREBUFFER_MS /
               ctx->duration_ms;
    } else if (ctx->bitrate_kbps > 0) {
        need = (int64_t)ctx->bitrate_kbps * 1000 / 8 * 10;
    } else {
        need = AUDIO_CACHE_FLAC_PREBUFFER_FALLBACK;
    }
    if (need < AUDIO_CACHE_PROGRESSIVE_THRESHOLD) {
        need = AUDIO_CACHE_PROGRESSIVE_THRESHOLD;
    }
    if (ctx->attempt_total > 0 && need > ctx->attempt_total) {
        need = ctx->attempt_total;
    }
    return need;
}

/* Лог скорости раз в ~5 с: по нему видно, медленная ли сеть на консоли
 * или что-то тормозит у нас. */
static void ac_log_speed(const char *track_id, int64_t absolute,
                         int64_t total, u64 *last_us, int64_t *last_bytes)
{
    u64 now = sceKernelGetSystemTimeWide();
    static u64 s_log_us = 0;

    if (*last_us == 0) {
        *last_us = now;
        *last_bytes = absolute;
        return;
    }
    /* Окно оценки — 2 с, запись в лог — не чаще раза в 5 с. */
    if (now - *last_us >= 2000000ULL) {
        int64_t delta = absolute - *last_bytes;
        if (delta >= 0) {
            if (now - s_log_us >= 5000000ULL) {
                logLine("ac: dl speed track='%s' %d/%d %d KB/s\n",
                        track_id, (int)absolute, (int)total,
                        (int)(delta * 1000000LL / (int64_t)(now - *last_us) / 1024));
                s_log_us = now;
            }
        }
        *last_us = now;
        *last_bytes = absolute;
    }
}

/* Статус закачки: байты, УЖЕ записанные в файл (их может читать плеер). */
static void ac_file_status_update(AcFileDlCtx *ctx, int64_t absolute)
{
    int progressive = absolute >= ctx->progressive_need;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (strcmp(ctx->slot->status.track_id, ctx->track_id) == 0) {
        ctx->slot->status.downloaded_bytes = absolute;
        if (ctx->attempt_total > 0) {
            ctx->slot->status.content_length = ctx->attempt_total;
        }
        ctx->slot->status.error_code = 0;
        if (progressive && ctx->slot->status.state == AUDIO_CACHE_DOWNLOADING) {
            ctx->slot->status.state = AUDIO_CACHE_PROGRESSIVE_READY;
            ctx->slot->status.progressive = 1;
            logLine("ac: slot=%d progressive ready '%s' bytes=%d\n",
                    ctx->slot->slot_id, ctx->track_id, (int)absolute);
        }
    }
    notify_source_locked(ctx->slot);
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
}

/* Поток записи: сливает кольцо в файл кусками, обновляет статус. */
static int ac_writer_main(SceSize args, void *argp)
{
    AcFileDlCtx *ctx = *(AcFileDlCtx **)argp;
    IoProfileLane profile;
    io_profile_begin(&profile, IP_WRITER0 + ctx->slot->slot_id);
    (void)args;
    for (;;) {
        SceUInt timeout = 200000;
        size_t n, tail;
        io_profile_phase(&profile, IP_WAIT);
        (void)sceKernelWaitSema(ctx->wsema, 1, &timeout);
        io_profile_phase(&profile, IP_OTHER);
        for (;;) {
            int rc;
            sceKernelLockLwMutex(&ctx->wmutex, 1, NULL);
            n = ctx->wb_used;
            uint64_t profile_now = sceKernelGetSystemTimeWide();
            io_profile_event(IP_WRITE_BUFFER, ctx->slot->slot_id, (int)n,
                             profile_now, profile_now);
            tail = ctx->wb_tail;
            sceKernelUnlockLwMutex(&ctx->wmutex, 1);
            if (n > AC_WRITE_CHUNK) n = AC_WRITE_CHUNK;
            if (n > AC_WBUF_SIZE - tail) n = AC_WBUF_SIZE - tail;
            if (n == 0) break;
            /* Мелкие порции копим, кроме финала/таймаута-слива. */
            if (n < AC_WRITE_MIN && !ctx->wstop && ctx->wb_used < AC_WRITE_MIN &&
                timeout == 0) {
                break;
            }
            io_profile_phase(&profile, IP_IO);
            rc = fs_write(ctx->fd, ctx->wbuf + tail, (SceSize)n);
            io_profile_phase(&profile, IP_OTHER);
            if (rc != (int)n) {
                logLine("ac: file write short slot=%d rc=%d\n",
                        ctx->slot->slot_id, rc);
                ctx->werror = 1;
                sceKernelLockLwMutex(&ctx->wmutex, 1, NULL);
                ctx->wb_used = 0;
                sceKernelUnlockLwMutex(&ctx->wmutex, 1);
                io_profile_end(&profile);
                return sceKernelExitDeleteThread(0);
            }
            sceKernelLockLwMutex(&ctx->wmutex, 1, NULL);
            ctx->wb_tail = (ctx->wb_tail + n) % AC_WBUF_SIZE;
            ctx->wb_used -= n;
            sceKernelUnlockLwMutex(&ctx->wmutex, 1);
            ctx->flushed += (int64_t)n;
            ctx->session_written = ctx->flushed;
            ac_file_status_update(ctx, ctx->base_size + ctx->flushed);
        }
        sceKernelLockLwMutex(&ctx->wmutex, 1, NULL);
        n = ctx->wb_used;
        sceKernelUnlockLwMutex(&ctx->wmutex, 1);
        if (ctx->wstop && n == 0) break;
    }
    io_profile_end(&profile);
    return sceKernelExitDeleteThread(0);
}

static int ac_writer_start(AcFileDlCtx *ctx)
{
    AcFileDlCtx *self = ctx;
    ctx->wbuf = (unsigned char *)malloc(AC_WBUF_SIZE);
    if (!ctx->wbuf) return -1;
    ctx->wb_head = ctx->wb_tail = ctx->wb_used = 0;
    ctx->wstop = 0;
    ctx->werror = 0;
    ctx->flushed = 0;
    ctx->wmutex_ok = (sceKernelCreateLwMutex(&ctx->wmutex, "ac_wbuf", 0, 0, NULL) >= 0);
    ctx->wsema = sceKernelCreateSema("ac_wsem", 0, 0, 1, NULL);
    if (!ctx->wmutex_ok || ctx->wsema < 0) goto fail;
    ctx->wthread = sceKernelCreateThread("ac_writer", ac_writer_main,
                                         0x20, 0x4000, 0, NULL);
    if (ctx->wthread < 0) goto fail;
    if (sceKernelStartThread(ctx->wthread, sizeof(self), &self) < 0) {
        sceKernelDeleteThread(ctx->wthread);
        ctx->wthread = -1;
        goto fail;
    }
    return 0;
fail:
    if (ctx->wsema >= 0) sceKernelDeleteSema(ctx->wsema);
    if (ctx->wmutex_ok) sceKernelDeleteLwMutex(&ctx->wmutex);
    free(ctx->wbuf);
    ctx->wbuf = NULL;
    ctx->wthread = -1;
    ctx->wsema = -1;
    return -1;
}

/* Дождаться слива остатка и освободить ресурсы. */
static void ac_writer_stop(AcFileDlCtx *ctx)
{
    SceUInt timeout = 60u * 1000000u;
    if (!ctx->wbuf) return;
    ctx->wstop = 1;
    sceKernelSignalSema(ctx->wsema, 1);
    if (sceKernelWaitThreadEnd(ctx->wthread, &timeout) < 0) {
        sceKernelTerminateDeleteThread(ctx->wthread);
        ctx->werror = 1;
    }
    sceKernelDeleteSema(ctx->wsema);
    sceKernelDeleteLwMutex(&ctx->wmutex);
    free(ctx->wbuf);
    ctx->wbuf = NULL;
    if (ctx->use_aes) mbedtls_aes_free(&ctx->aes);
}

static int ac_file_chunk(const char *data, int size, void *user)
{
    AcFileDlCtx *ctx = (AcFileDlCtx *)user;
    if (!ctx || !data || size <= 0) return -1;
    if (ctx->slot->job_cancel || s_shutdown) {
        return NET_HTTP_STREAM_CANCELLED;
    }
    if (ctx->fd < 0 || !ctx->wbuf) return -1;
    while (size > 0) {
        size_t freebytes, n;
        if (ctx->werror) return -1;
        sceKernelLockLwMutex(&ctx->wmutex, 1, NULL);
        freebytes = AC_WBUF_SIZE - ctx->wb_used;
        sceKernelUnlockLwMutex(&ctx->wmutex, 1);
        if (freebytes == 0) {
            /* Карта не успевает: тормозим приём (редко, при длинной записи). */
            sceKernelSignalSema(ctx->wsema, 1);
            if (ctx->slot->job_cancel || s_shutdown) {
                return NET_HTTP_STREAM_CANCELLED;
            }
            sceKernelDelayThread(3000);
            continue;
        }
        n = (size_t)size;
        if (n > freebytes) n = freebytes;
        if (n > AC_WBUF_SIZE - ctx->wb_head) n = AC_WBUF_SIZE - ctx->wb_head;
        if (ctx->use_aes) {
            mbedtls_aes_crypt_ctr(&ctx->aes, n, &ctx->aes_off, ctx->aes_nonce,
                                  ctx->aes_stream,
                                  (const unsigned char *)data,
                                  ctx->wbuf + ctx->wb_head);
        } else {
            memcpy(ctx->wbuf + ctx->wb_head, data, n);
        }
        ctx->wb_head = (ctx->wb_head + n) % AC_WBUF_SIZE;
        sceKernelLockLwMutex(&ctx->wmutex, 1, NULL);
        ctx->wb_used += n;
        freebytes = ctx->wb_used;
        sceKernelUnlockLwMutex(&ctx->wmutex, 1);
        data += n;
        size -= (int)n;
        if (freebytes >= AC_WRITE_MIN) sceKernelSignalSema(ctx->wsema, 1);
    }
    return 0;
}

static int ac_file_progress(int written_total, int content_length, void *user)
{
    AcFileDlCtx *ctx = (AcFileDlCtx *)user;
    if (!ctx) return -1;
    if (ctx->attempt_total <= 0 && content_length > 0) {
        ctx->attempt_total = content_length;
    }
    /* Размер объекта стал известен — уточняем пребафер (для FLAC это
     * 10 с трека по длительности, не больше полного файла). */
    ctx->progressive_need = ac_prebuffer_need(ctx);
    /* written_total — абсолютно принятые из сети байты (с range_start);
     * плееру доступно только записанное писателем (ac_file_status_update). */
    ctx->attempt_had_progress = 1;
    ac_log_speed(ctx->track_id, (int64_t)written_total,
                 (ctx->attempt_total > 0 ? ctx->attempt_total : content_length),
                 &ctx->last_speed_us, &ctx->last_speed_bytes);
    return 0;
}

/* 1 = MP4-файл полон (последний сэмпл в пределах файла). Частичный
 * resume-файл хитом кэша не считается. */
static int ac_mp4_file_complete(const char *path, const char *family)
{
    Mp4Track *t;
    int64_t off = 0;
    int size = 0;
    int ok = 0;

    t = mp4_track_open(path);
    if (!t) return 0;
    if (strcmp(family, "flac") == 0 ? mp4_track_is_flac(t)
                                    : mp4_track_is_aac(t)) {
        int n = mp4_track_sample_count(t);
        if (n > 0 && mp4_track_sample_at(t, n - 1, &off, &size) == 0) {
            ok = 1;
        }
    }
    mp4_track_close(t);
    return ok;
}

/* READY describes complete audio data; cache commit waits for all readers. */
static int file_job_ready(AcSlot *slot, const AcJob *job,
                          const char *download_path, const char *final_path,
                          int bitrate_kbps, AudioQuality quality)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (fs_getstat(download_path, &st) != 0 || st.st_size <= 0) {
        logLine("ac: final missing after job slot=%d '%s'\n",
                slot->slot_id, final_path);
        logger_flush();
        slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL,
                        NET_HTTP_ERR_PROTOCOL);
        return -1;
    }
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    slot->commit_state = strcmp(download_path, final_path) != 0
        ? AC_COMMIT_PENDING : AC_COMMIT_NONE;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    slot_set_source(slot, AUDIO_CACHE_READY, job->track.id, download_path,
                    (int64_t)st.st_size, (int64_t)st.st_size, bitrate_kbps,
                    1, 1, 0, quality, 0);
    logLine("ac: state -> READY (file) slot=%d track='%s' path='%s' size=%d\n",
            slot->slot_id, job->track.id, download_path, (int)st.st_size);
    logger_flush();
    /* Чистка cache_music выполняется в worker_main после задания
     * (ac_trim_music: только текущий + предзагруженный трек). */
    return 0;
}

static YmStatus ac_fetch_variant(YmApiContext *ctx, const char *track_id,
                                 DownloadCodec codec, const char *quality,
                                 YmDownloadVariant *variant)
{
    switch (codec) {
    case DOWNLOAD_CODEC_MP3:
        return ym_api_download_get_mp3_raw(ctx, track_id, quality, variant);
    case DOWNLOAD_CODEC_AAC:
        return ym_api_download_get_aac_raw(ctx, track_id, quality, variant);
    case DOWNLOAD_CODEC_FLAC:
        return ym_api_download_get_flac_raw(ctx, track_id, variant);
    default:
        return YM_ERR_UNSUPPORTED;
    }
}

/* Файловая загрузка с Range-resume и фиксированными параметрами запроса. */
static int run_file_job(AcSlot *slot, const AcJob *job,
                        const YmApiContext *api_ctx, DownloadCodec codec,
                        const char *quality, YmDownloadVariant *variant,
                        const char *initial_path)
{
    const char *family = download_codec_name(codec);
    unsigned int retry_delay_us = AUDIO_CACHE_RETRY_MIN_US;
    YmStatus ym_status;
    char final_path[AUDIO_STORAGE_PATH_CAPACITY];
    char dl_path[AUDIO_STORAGE_PATH_CAPACITY];
    int restarted = 0;
    int refresh_variant = 0;
    snprintf(final_path, sizeof(final_path), "%s", initial_path);

    for (;;) {
        AcFileDlCtx ctx;
        SceIoStat st;
        int64_t base_size = 0;
        int rc_dl;

        if (slot->job_cancel || s_shutdown) {
            slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id, NULL,
                            NET_HTTP_STREAM_CANCELLED);
            return 0;
        }

        ym_status = refresh_variant
            ? ac_fetch_variant((YmApiContext *)api_ctx, job->track.id, codec,
                                quality, variant) : YM_OK;
        refresh_variant = 1;
        if (ym_status == YM_ERR_CANCELLED || slot->job_cancel || s_shutdown) {
            slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id, NULL,
                            NET_HTTP_STREAM_CANCELLED);
            return 0;
        }
        if (ym_status != YM_OK) {
            if (ym_status == YM_ERR_NETWORK) {
                if (ac_wait_for_retry(slot, job->track.id,
                                      sceKernelGetSystemTimeWide() + retry_delay_us) != 0) {
                    slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id,
                                    NULL, NET_HTTP_STREAM_CANCELLED);
                    return 0;
                }
                if (retry_delay_us < AUDIO_CACHE_RETRY_MAX_US / 2) {
                    retry_delay_us *= 2;
                } else {
                    retry_delay_us = AUDIO_CACHE_RETRY_MAX_US;
                }
                continue;
            }
            logLine("ac: %s url FAILED slot=%d track='%s' status=%d\n",
                    family, slot->slot_id, job->track.id, ym_status);
            logger_flush();
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL,
                            (int)ym_status);
            return -1;
        }

        if (snprintf(dl_path, sizeof(dl_path), "%s.part", final_path) >= (int)sizeof(dl_path)) {
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, NET_HTTP_ERR_INVALID);
            return -1;
        }

        memset(&st, 0, sizeof(st));
        if (fs_getstat(dl_path, &st) == 0 && st.st_size > 0) {
            base_size = (int64_t)st.st_size;
        }

        memset(&ctx, 0, sizeof(ctx));
        ctx.slot = slot;
        snprintf(ctx.track_id, sizeof(ctx.track_id), "%s", job->track.id);
        ctx.base_size = base_size;
        ctx.is_flac = strcmp(family, "flac") == 0;
        ctx.bitrate_kbps = variant->bitrate_kbps;
        ctx.duration_ms = job->track.duration_ms;
        ctx.progressive_need = ac_prebuffer_need(&ctx);
        ctx.fd = fs_open(dl_path, PSP_O_WRONLY | (base_size > 0 ? PSP_O_APPEND : (PSP_O_CREAT | PSP_O_TRUNC)), 0777);
        if (ctx.fd < 0) {
            logLine("ac: %s file open failed slot=%d '%s' rc=0x%08X\n",
                    family, slot->slot_id, dl_path, ctx.fd);
            logger_flush();
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL,
                            ctx.fd);
            return -1;
        }

        /* Статус несёт финальный путь: плеер выбирает декодер по
         * расширению (.flac / .mp4 / .aac), файл играется уже во время
         * докачки — с порога PROGRESSIVE_READY. */
        /* Расшифровка encraw: CTR с нулевым nonce, позиция = размер файла. */
        if (variant->encrypted) {
            unsigned char zero[16];
            size_t skip = (size_t)(base_size % 16);
            uint64_t blk = (uint64_t)(base_size / 16);
            int i;
            memset(ctx.aes_nonce, 0, sizeof(ctx.aes_nonce));
            for (i = 0; i < 8; i++) {
                ctx.aes_nonce[15 - i] = (unsigned char)(blk >> (8 * i));
            }
            mbedtls_aes_init(&ctx.aes);
            ctx.aes_off = 0;
            if (mbedtls_aes_setkey_enc(&ctx.aes, variant->aes_key, 128) != 0) {
                logLine("ac: aes setkey failed slot=%d\n", slot->slot_id);
                mbedtls_aes_free(&ctx.aes);
                fs_close(ctx.fd);
                slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, -1);
                return -1;
            }
            ctx.use_aes = 1;
            if (skip > 0) {  /* докачка с позиции не на границе блока */
                memset(zero, 0, sizeof(zero));
                mbedtls_aes_crypt_ctr(&ctx.aes, skip, &ctx.aes_off,
                                      ctx.aes_nonce, ctx.aes_stream,
                                      zero, zero);
            }
        }
        if (ac_writer_start(&ctx) != 0) {
            logLine("ac: writer start failed slot=%d\n", slot->slot_id);
            if (ctx.use_aes) mbedtls_aes_free(&ctx.aes);
            fs_close(ctx.fd);
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, -1);
            return -1;
        }
        slot_set_source(slot, AUDIO_CACHE_DOWNLOADING, job->track.id,
                        dl_path, base_size, base_size,
                        variant->bitrate_kbps, 0,
                        base_size >= ctx.progressive_need,
                        0, variant->quality, 0);
        logLine("ac: %s file start slot=%d track='%s' resume=%d\n",
                family, slot->slot_id, job->track.id, (int)base_size);
        logger_flush();

        rc_dl = http_stream(&(HttpRequest){
                                .method = HTTP_GET, .url = variant->url,
                                .token = api_ctx->oauth_token,
                                .identity_encoding = 1,
                                .range_enabled = base_size > 0,
                                .range_start = (int)base_size,
                                .range_total = 0
                            },
                            &(HttpSink){ .on_chunk = ac_file_chunk,
                                         .on_progress = ac_file_progress,
                                         .user = &ctx });
        /* Дописать всё принятое (и при обрыве — для resume), затем закрыть. */
        ac_writer_stop(&ctx);
        {
            int close_rc = fs_close(ctx.fd);
            if (rc_dl == 0 && close_rc < 0) rc_dl = close_rc;
        }
        ctx.fd = -1;
        if (rc_dl == 0 && ctx.werror) rc_dl = NET_HTTP_ERR_PROTOCOL;

        if (rc_dl == 0) {
            return file_job_ready(slot, job, dl_path, final_path,
                                  variant->bitrate_kbps, variant->quality);
        }
        if (rc_dl == NET_HTTP_STREAM_CANCELLED ||
            slot->job_cancel || s_shutdown) {
            slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id, NULL,
                            NET_HTTP_STREAM_CANCELLED);
            return 0;
        }
        if (!net_http_error_is_network(rc_dl)) {
            /* 416/битый resume: частичный файл не подходит новому варианту
             * (смена качества, другой размер файла) — стираем и качаем с
             * нуля один раз, вместо pb: error skip. */
            if (base_size > 0 && !restarted) {
                logLine("ac: %s terminal rc=%d slot=%d track='%s' resume rejected -> restart from 0\n",
                        family, rc_dl, slot->slot_id, job->track.id);
                logger_flush();
                fs_remove(dl_path);
                restarted = 1;
                continue;
            }
            logLine("ac: %s file failed terminal slot=%d track='%s' rc=%d\n",
                    family, slot->slot_id, job->track.id, rc_dl);
            logger_flush();
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, rc_dl);
            return -1;
        }
        logLine("ac: %s file interrupted slot=%d track='%s' rc=%d\n",
                family, slot->slot_id, job->track.id, rc_dl);
        logger_flush();
        if (ac_wait_for_retry(slot, job->track.id,
                              sceKernelGetSystemTimeWide() + retry_delay_us) != 0) {
            slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id, NULL,
                            NET_HTTP_STREAM_CANCELLED);
            return 0;
        }
        if (retry_delay_us < AUDIO_CACHE_RETRY_MAX_US / 2) {
            retry_delay_us *= 2;
        } else {
            retry_delay_us = AUDIO_CACHE_RETRY_MAX_US;
        }
    }
}

static const DownloadCodec kCodecPlans[DOWNLOAD_CODEC_COUNT][DOWNLOAD_CODEC_COUNT] = {
    { DOWNLOAD_CODEC_MP3, DOWNLOAD_CODEC_AAC, DOWNLOAD_CODEC_COUNT },
    { DOWNLOAD_CODEC_AAC, DOWNLOAD_CODEC_MP3, DOWNLOAD_CODEC_COUNT },
    { DOWNLOAD_CODEC_FLAC, DOWNLOAD_CODEC_AAC, DOWNLOAD_CODEC_MP3 }
};

static const char *const kCacheExtensions[] = { "mp3", "aac", "flac", "mp4" };

static int ac_try_cached_variant(AcSlot *slot, const AcJob *job,
                                 AudioQuality quality, char *path, size_t capacity)
{
    size_t representation;
    for (representation = 0; representation < sizeof(kCacheExtensions) / sizeof(kCacheExtensions[0]); representation++) {
        SceIoStat st;
        const char *extension = kCacheExtensions[representation];
        if (audio_storage_build_path(job->track.id, extension, quality, path, capacity) != 0) {
            return -1;
        }
        memset(&st, 0, sizeof(st));
        if (fs_getstat(path, &st) != 0) continue;
        if (strcmp(extension, "aac") == 0 && !ac_mp4_file_complete(path, "aac")) continue;
        if (strcmp(extension, "mp4") == 0 && !ac_mp4_file_complete(path, "flac")) continue;
        if (slot->job_cancel || s_shutdown) return 0;
        logLine("ac: cache hit quality=%s slot=%d '%s'\n",
                audio_quality_name(quality), slot->slot_id, path);
        slot_set_source(slot, AUDIO_CACHE_READY, job->track.id, path,
                        (int64_t)st.st_size, (int64_t)st.st_size, 0, 1, 1, 0, quality, 1);
        return 1;
    }
    return 0;
}

static int ac_remove_outdated_cache(AcSlot *slot, const AcJob *job, AudioQuality required)
{
    int quality;
    size_t representation;
    for (quality = AUDIO_QUALITY_UNKNOWN; quality < (int)required; quality++) {
        for (representation = 0; representation < sizeof(kCacheExtensions) / sizeof(kCacheExtensions[0]); representation++) {
            char path[AUDIO_STORAGE_PATH_CAPACITY];
            char partial[AUDIO_STORAGE_PATH_CAPACITY];
            SceIoStat st;
            int rc;
            if (slot->job_cancel || s_shutdown) return 0;
            if (audio_storage_build_path(job->track.id, kCacheExtensions[representation],
                                        (AudioQuality)quality, path, sizeof(path)) != 0 ||
                snprintf(partial, sizeof(partial), "%s.part", path) >= (int)sizeof(partial)) {
                return NET_HTTP_ERR_INVALID;
            }
            if (fs_getstat(path, &st) == 0) {
                rc = fs_remove(path);
                if (rc < 0) return rc;
                logLine("ac: removed outdated cache '%s'\n", path);
            }
            if (fs_getstat(partial, &st) == 0) {
                rc = fs_remove(partial);
                if (rc < 0) return rc;
                logLine("ac: removed outdated partial '%s'\n", partial);
            }
        }
    }
    return 0;
}

static int ac_try_cached(AcSlot *slot, const AcJob *job, AudioQuality required,
                          char *path, size_t capacity)
{
    int quality;
    for (quality = required; quality < AUDIO_QUALITY_COUNT; quality++) {
        int cached = ac_try_cached_variant(slot, job, (AudioQuality)quality, path, capacity);
        if (cached < 0) {
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, YM_ERR_PARSE);
        }
        if (cached != 0 || slot->job_cancel || s_shutdown) return cached;
    }
    return 0;
}

static YmStatus ac_prepare_variant(AcSlot *slot, const AcJob *job,
                                   YmApiContext *ctx, DownloadCodec codec,
                                   const YmDownloadVariant *variant,
                                   char *path, size_t capacity)
{
    YmStatus status;
    const char *extension = download_codec_name(codec);
    if (codec != DOWNLOAD_CODEC_MP3) {
        AcProbeResult probe;
        status = ac_probe_stream(slot, variant->url, ctx->oauth_token, &probe);
        if (slot->job_cancel || s_shutdown) return YM_ERR_CANCELLED;
        if (status != YM_OK) return status;
        if (codec == DOWNLOAD_CODEC_AAC) {
            if (!probe.is_mp4 || probe.mp4.kind != MP4_PROBE_AAC) {
                return YM_ERR_UNSUPPORTED;
            }
        } else {
            int kind_ok = probe.is_raw_flac ||
                          (probe.is_mp4 && probe.mp4.kind == MP4_PROBE_FLAC);
            if (!kind_ok || probe.mp4.rate != 44100 || probe.mp4.ch != 2 ||
                probe.mp4.bps < 16 || probe.mp4.bps > 32) {
                return YM_ERR_UNSUPPORTED;
            }
            extension = probe.is_mp4 ? "mp4" : "flac";
        }
    }
    return audio_storage_build_path(job->track.id, extension, variant->quality, path, capacity) == 0
        ? YM_OK : YM_ERR_PARSE;
}

static int run_download_job(AcSlot *slot, AcJob *job)
{
    DownloadSettings settings = job->settings;
    const DownloadCodec *plan = kCodecPlans[settings.codec];
    const char *quality = download_quality_name(settings.quality);
    YmApiContext api_ctx;
    int candidate;
    AudioQuality required = download_settings_required_quality(settings);
    int cached;
    int remove_rc;
    char path[AUDIO_STORAGE_PATH_CAPACITY];

    logLine("ac: worker started slot=%d track_id='%s' codec=%s quality=%s\n",
            slot->slot_id, job->track.id,
            download_codec_name(settings.codec), quality);
    if (slot->job_cancel || s_shutdown) goto cancelled;
    if (job->token[0] == '\0') {
        slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, -1);
        return -1;
    }
    api_ctx.oauth_token = job->token;
    api_ctx.timeout_ms = 0;

    cached = ac_try_cached(slot, job, required, path, sizeof(path));
    if (slot->job_cancel || s_shutdown) goto cancelled;
    if (cached != 0) return cached > 0 ? 0 : -1;

    for (candidate = 0; candidate < DOWNLOAD_CODEC_COUNT &&
         plan[candidate] != DOWNLOAD_CODEC_COUNT; candidate++) {
        DownloadCodec codec = plan[candidate];
        YmDownloadVariant variant;
        YmStatus status;
        if (slot->job_cancel || s_shutdown) goto cancelled;
        logLine("ac: fetch codec=%s slot=%d track='%s' quality=%s\n",
                download_codec_name(codec), slot->slot_id, job->track.id,
                codec == DOWNLOAD_CODEC_FLAC ? "lossless" : quality);
        memset(&variant, 0, sizeof(variant));
        status = ac_fetch_variant(&api_ctx, job->track.id, codec, quality, &variant);
        if (status == YM_ERR_CANCELLED || slot->job_cancel || s_shutdown) goto cancelled;
        if (status == YM_ERR_UNSUPPORTED) continue;
        if (status != YM_OK) {
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, status);
            return -1;
        }
        logLine("ac: resolved slot=%d track='%s' requested=%s quality=%s codec=%s\n",
                slot->slot_id, job->track.id,
                codec == DOWNLOAD_CODEC_FLAC ? "lossless" : quality,
                audio_quality_name(variant.quality), download_codec_name(codec));
        cached = ac_try_cached(slot, job, variant.quality, path, sizeof(path));
        if (slot->job_cancel || s_shutdown) goto cancelled;
        if (cached != 0) return cached > 0 ? 0 : -1;
        status = ac_prepare_variant(slot, job, &api_ctx, codec,
                                    &variant, path, sizeof(path));
        if (status == YM_ERR_CANCELLED || slot->job_cancel || s_shutdown) goto cancelled;
        if (status == YM_ERR_UNSUPPORTED) continue;
        if (status != YM_OK) {
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, status);
            return -1;
        }
        remove_rc = ac_remove_outdated_cache(slot, job, variant.quality);
        if (slot->job_cancel || s_shutdown) goto cancelled;
        if (remove_rc < 0) {
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, remove_rc);
            return -1;
        }
        if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
        if (strcmp(slot->status.track_id, job->track.id) == 0) {
            slot->status.bitrate_kbps = variant.bitrate_kbps;
        }
        if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
        cover_manager_resume_network(&job->network_pause_held);
        if (slot->job_cancel || s_shutdown) goto cancelled;
        return run_file_job(slot, job, &api_ctx, codec, quality, &variant, path);
    }
    logLine("ac: no codec available slot=%d track_id='%s'\n",
            slot->slot_id, job->track.id);
    slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, YM_ERR_UNSUPPORTED);
    return -1;

cancelled:
    slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id, NULL,
                    NET_HTTP_STREAM_CANCELLED);
    return 0;
}

void audio_cache_hint_next(const char *track_id)
{
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    snprintf(s_hint_next, sizeof(s_hint_next), "%s", track_id ? track_id : "");
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
}

/* Политика временного кэша: в cache_music живут только текущий и
 * предзагруженный треки (все их форматы: <id>.mp3/.aac/.mp4/.flac),
 * всё остальное стирается перед каждым новым заданием.
 * Keep: status.track_id обоих слотов, pending_job слотов, extra_id
 * (id задания, которое worker только что забрал — статус ещё не
 * обновлён) и s_hint_next (будущий prefetch: trim идёт ДО поста
 * prefetch-задания, о нём в кэше ещё никто не знает). */
static void ac_trim_music(const char *extra_id)
{
    char ids[5][48];
    const char *keep[5];
    int slot_idx[2];
    int n = 0;
    int i;

    if (audio_cache_retain_downloads()) return;

    memset(ids, 0, sizeof(ids));
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    slot_idx[0] = s_active_slot ? 1 : 0;
    slot_idx[1] = s_active_slot ? 0 : 1;
    for (i = 0; i < 2; i++) {
        AcSlot *s = &s_slots[slot_idx[i]];

        snprintf(ids[n], sizeof(ids[n]), "%s", s->status.track_id);
        keep[n] = ids[n];
        n++;
        if (s->job_pending) {
            snprintf(ids[n], sizeof(ids[n]), "%s", s->pending_job.track.id);
            keep[n] = ids[n];
            n++;
        }
    }
    if (s_hint_next[0] != '\0' && n < 5) {
        snprintf(ids[n], sizeof(ids[n]), "%s", s_hint_next);
        keep[n] = ids[n];
        n++;
    }
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    if (extra_id && extra_id[0] && n < 5) {
        snprintf(ids[n], sizeof(ids[n]), "%s", extra_id);
        keep[n] = ids[n];
        n++;
    }

    fs_evict_except("cache_music", keep, n);
}

/* Caller holds s_status_mutex. State is authoritative; the semaphore only
 * requests another inspection of that state. */
static int wake_slot_locked(AcSlot *slot)
{
    int rc;
    if (slot->wake_pending) return 0;
    if (slot->worker_thread < 0 || slot->job_sema < 0) return -1;
    rc = sceKernelSignalSema(slot->job_sema, 1);
    if (rc < 0) {
        logLine("ac: wake failed slot=%d rc=%d\n", slot->slot_id, rc);
        return -1;
    }
    slot->wake_pending = 1;
    return 0;
}

static int cancel_job_locked(AcSlot *slot)
{
    if (slot->job_pending) cover_manager_resume_network(&slot->pending_job.network_pause_held);
    slot->job_pending = 0;
    slot->wait_for = NULL;
    slot->job_cancel = 1;
    slot->retire_requested = 1;
    notify_source_locked(slot);
    return wake_slot_locked(slot);
}

/* Only the slot worker commits; no new source may enter during rename. */
static void commit_completed_file(AcSlot *slot)
{
    char download_path[sizeof(slot->status.path)];
    char final_path[sizeof(slot->status.path)];
    char track_id[sizeof(slot->status.track_id)];
    int rc;

    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (slot->commit_state != AC_COMMIT_PENDING ||
        slot->status.source_ref_count != 0) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
        return;
    }
    slot->commit_state = AC_COMMIT_IN_PROGRESS;
    memcpy(download_path, slot->status.path, sizeof(download_path));
    memcpy(track_id, slot->status.track_id, sizeof(track_id));
    sceKernelUnlockLwMutex(&s_status_mutex, 1);

    rc = audio_storage_complete_path(download_path, final_path, sizeof(final_path));
    if (rc == 0) {
        logLine("ac: cache commit begin slot=%d track='%s' refs=0 from='%s' to='%s'\n",
                slot->slot_id, track_id, download_path, final_path);
        rc = fs_rename(download_path, final_path);
        if (rc < 0) {
            SceIoStat previous;
            if (fs_getstat(final_path, &previous) == 0 &&
                fs_remove(final_path) == 0) {
                rc = fs_rename(download_path, final_path);
            }
        }
    }

    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (rc >= 0) {
        snprintf(slot->status.path, sizeof(slot->status.path), "%s", final_path);
        slot->commit_state = AC_COMMIT_NONE;
    } else {
        slot->commit_state = AC_COMMIT_FAILED;
    }
    sceKernelUnlockLwMutex(&s_status_mutex, 1);
    if (rc >= 0) {
        logLine("ac: cache committed slot=%d track='%s' path='%s'\n",
                slot->slot_id, track_id, final_path);
    } else {
        logLine("ac: cache commit failed slot=%d track='%s' rc=0x%08X; source retained '%s'\n",
                slot->slot_id, track_id, (unsigned int)rc, download_path);
    }
}

/* Only the slot worker calls this, after run_download_job has returned.
 * Existing source holders block replacement until their files are released. */
static int retire_slot_locked(AcSlot *slot)
{
    int i;
    if (!slot->retire_requested) return 1;
    if (slot->status.source_ref_count != 0) return 0;
    logLine("ac: cancel complete slot=%d track_id='%s'\n",
            slot->slot_id, slot->status.track_id);
    memset(&slot->status, 0, sizeof(slot->status));
    slot->request_track_id[0] = '\0';
    slot->commit_state = AC_COMMIT_NONE;
    slot->retire_requested = 0;
    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        AcSlot *waiting = &s_slots[i];
        if (waiting->wait_for == slot && wake_slot_locked(waiting) < 0) {
            logLine("ac: cancellation waiter unavailable slot=%d\n", i);
        }
    }
    return 1;
}

static int worker_main(SceSize args, void *argp)
{
    AcSlot *slot;
    if (args != sizeof(slot) || !argp) return -1;
    slot = *(AcSlot **)argp;
    if (!slot) return -1;
    log_worker_priority(sceKernelGetThreadId());

    for (;;) {
        int wait_rc = sceKernelWaitSema(slot->job_sema, 1, NULL);
        if (wait_rc < 0) {
            logLine("ac: worker wait failed slot=%d rc=%d\n", slot->slot_id, wait_rc);
            return wait_rc;
        }
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
        slot->wake_pending = 0;
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
        for (;;) {
            AcJob job;
            commit_completed_file(slot);
            sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
            if (!retire_slot_locked(slot)) {
                sceKernelUnlockLwMutex(&s_status_mutex, 1);
                break;
            }
            if (s_shutdown) {
                sceKernelUnlockLwMutex(&s_status_mutex, 1);
                return 0;
            }
            if (!slot->job_pending ||
                (slot->wait_for && slot->wait_for->retire_requested)) {
                sceKernelUnlockLwMutex(&s_status_mutex, 1);
                break;
            }
            job = slot->pending_job;
            slot->request_settings = job.settings;
            memcpy(slot->request_track_id, job.track.id, sizeof(slot->request_track_id));
            slot->pending_job.network_pause_held = 0;
            slot->job_pending = 0;
            slot->wait_for = NULL;
            slot->job_cancel = 0;
            sceKernelUnlockLwMutex(&s_status_mutex, 1);
            ac_trim_music(job.track.id);
            net_tls_cancel_bind(&slot->job_cancel);
            run_download_job(slot, &job);
            net_tls_cancel_unbind();
            cover_manager_resume_network(&job.network_pause_held);
        }
    }
}

static int post_job(AcSlot *slot, const TrackEntry *track, const char *token,
                    int pause_network, AcSlot *wait_for)
{
    AcJob job;
    int wake_rc;

    if (!s_mutex_initialized || !slot || !track || track->id[0] == '\0' || !token || token[0] == '\0') {
        return -1;
    }
    if (slot->worker_thread < 0 || slot->job_sema < 0) {
        logLine("ac: slot=%d worker unavailable\n", slot->slot_id);
        slot_set_status(slot, AUDIO_CACHE_ERROR, track->id, NULL, -2);
        return -1;
    }

    memset(&job, 0, sizeof(job));
    memcpy(&job.track, track, sizeof(TrackEntry));
    job.settings = download_settings_get();
    snprintf(job.token, sizeof(job.token), "%s", token);

    if (pause_network) {
        cover_manager_pause_network(&job.network_pause_held);
    }

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (slot->job_pending) cover_manager_resume_network(&slot->pending_job.network_pause_held);
    slot->pending_job = job;
    job.network_pause_held = 0;
    slot->job_pending = 1;
    slot->job_cancel = 1;
    slot->retire_requested = 1;
    slot->wait_for = wait_for;
    wake_rc = wake_slot_locked(slot);
    if (wake_rc < 0) {
        slot->job_pending = 0;
        cover_manager_resume_network(&slot->pending_job.network_pause_held);
    }
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    if (wake_rc < 0) {
        return -1;
    }

    logLine("ac: job posted slot=%d track_id='%s'\n", slot->slot_id, track->id);
    return 0;
}

void audio_cache_init(void)
{
    int i;

    load_cache_setting();

    memset(s_slots, 0, sizeof(s_slots));
    s_active_slot = 0;
    s_shutdown = 0;
    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        s_slots[i].worker_thread = -1;
        s_slots[i].job_sema = -1;
    }

    if (sceKernelCreateLwMutex(&s_status_mutex, "ac_status", 0, 0, NULL) >= 0) {
        s_mutex_initialized = 1;
    } else {
        logLine("ac: status mutex create failed\n");
        return;
    }

    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        AcSlot *slot = &s_slots[i];
        char name[16];

        slot->slot_id = i;
        slot->self_arg = slot;

        snprintf(name, sizeof(name), "ac_job%d", i);
        slot->job_sema = sceKernelCreateSema(name, 0, 0, 64, NULL);
        if (slot->job_sema < 0) {
            logLine("ac: job sema create failed slot=%d 0x%08X\n",
                    i, slot->job_sema);
            continue;
        }
        snprintf(name, sizeof(name), "ac_dl%d", i);
        slot->worker_thread = sceKernelCreateThread(name, worker_main,
                                                    0x18, 0x10000, 0, NULL);
        if (slot->worker_thread < 0) {
            logLine("ac: worker create failed slot=%d 0x%08X\n",
                    i, slot->worker_thread);
            continue;
        }
        if (sceKernelStartThread(slot->worker_thread,
                                 sizeof(slot->self_arg), &slot->self_arg) < 0) {
            logLine("ac: worker start failed slot=%d\n", i);
            sceKernelDeleteThread(slot->worker_thread);
            slot->worker_thread = -1;
        }
    }

    logLine("ac: init\n");
}

int audio_cache_quiesce(void)
{
    int i;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    s_shutdown = 1;
    int wake_failed = 0;
    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        if (s_slots[i].worker_thread >= 0 && cancel_job_locked(&s_slots[i]) < 0) {
            wake_failed = 1;
        }
    }
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    if (wake_failed) return -1;

    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        AcSlot *slot = &s_slots[i];
            if (slot->worker_thread >= 0) {
                /* Отмена кооперативная и ограничена сверху TLS-слоем; 20 с —
                 * страховочный предел. Живой поток НЕ удаляем: Delete на
                 * не-dormant потоке не работает (SCE_KERNEL_ERROR_NOT_DORMANT). */
                SceUInt timeout_us = 20000000U;
                if (sceKernelWaitThreadEnd(slot->worker_thread, &timeout_us) < 0) {
                    logLine("ac: worker did not stop slot=%d, leaking thread\n", i);
                    return -1;
                }
            }
    }
    return 0;
}

int audio_cache_shutdown(void)
{
    int i;
    if (audio_cache_quiesce() < 0) return -1;
    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
            AcSlot *slot = &s_slots[i];
            if (slot->worker_thread >= 0) {
                sceKernelDeleteThread(slot->worker_thread);
                slot->worker_thread = -1;
            }
            if (slot->job_sema >= 0) {
                sceKernelDeleteSema(slot->job_sema);
                slot->job_sema = -1;
            }
    }
    if (s_mutex_initialized) {
            sceKernelDeleteLwMutex(&s_status_mutex);
            s_mutex_initialized = 0;
    }

    memset(s_slots, 0, sizeof(s_slots));
    s_active_slot = 0;
    logLine("ac: shutdown\n");
    return 0;
}

/* Caller holds s_status_mutex. */
static int slot_satisfies_request(const AcSlot *slot)
{
    DownloadSettings settings = download_settings_get();
    return slot->status.quality >= download_settings_required_quality(settings) ||
        (slot->request_settings.codec == settings.codec &&
         slot->request_settings.quality == settings.quality);
}

/* Caller holds s_status_mutex. */
static int slot_source_matches(const AcSlot *slot, const char *track_id)
{
    AudioCacheState state = slot->status.state;
    return !slot->retire_requested && !slot->job_cancel &&
        (state == AUDIO_CACHE_DOWNLOADING || state == AUDIO_CACHE_PROGRESSIVE_READY ||
         state == AUDIO_CACHE_READY) &&
        strcmp(slot->status.track_id, track_id) == 0 && slot_satisfies_request(slot);
}

/* Caller holds s_status_mutex. The request may precede publication of a file. */
static int slot_has_request(const AcSlot *slot, const char *track_id)
{
    DownloadSettings settings = download_settings_get();
    if (slot->job_pending) {
        return strcmp(slot->pending_job.track.id, track_id) == 0 &&
            slot->pending_job.settings.codec == settings.codec &&
            slot->pending_job.settings.quality == settings.quality;
    }
    if (slot_source_matches(slot, track_id)) return 1;
    return !slot->retire_requested && !slot->job_cancel &&
        slot->status.state == AUDIO_CACHE_IDLE &&
        strcmp(slot->request_track_id, track_id) == 0 &&
        slot->request_settings.codec == settings.codec &&
        slot->request_settings.quality == settings.quality;
}

int audio_cache_start(const TrackEntry *track, const char *token)
{
    AcSlot *slot = active_slot();
    AcSlot *prefetch = prefetch_slot();

    if (!track || track->id[0] == '\0') return -1;
    if (!token || token[0] == '\0') {
        logLine("ac: start refused, empty token\n");
        return -1;
    }

    logLine("ac: start request track_id='%s' active_slot=%d\n", track->id, slot->slot_id);
    if (!s_mutex_initialized) return -1;

    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (slot_source_matches(slot, track->id) ||
        (slot->status.source_ref_count == 0 && slot_has_request(slot, track->id))) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
        logLine("ac: reuse active request track_id='%s'\n", track->id);
        return 0;
    }
    if (slot->status.source_ref_count > 0) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
        logLine("ac: start refused, active source held (wanted '%s')\n", track->id);
        return -1;
    }
    if (slot_has_request(prefetch, track->id)) {
        int cancel_rc = cancel_job_locked(slot);
        AudioCacheState state = prefetch->status.state;
        if (cancel_rc >= 0) s_active_slot = prefetch->slot_id;
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
        if (cancel_rc < 0) return -1;
        logLine("ac: reuse prefetch active_slot=%d track_id='%s' state=%s\n",
                prefetch->slot_id, track->id, audio_cache_state_name(state));
        return 0;
    }
    int cancel_rc = cancel_job_locked(prefetch);
    sceKernelUnlockLwMutex(&s_status_mutex, 1);
    if (cancel_rc < 0) return -1;
    return post_job(slot, track, token, 1, prefetch);
}

int audio_cache_prefetch_start(const TrackEntry *track, const char *token)
{
    AcSlot *slot = prefetch_slot();
    AudioCacheStatus status;
    int active_source_held;
    int already_requested;

    if (!track || track->id[0] == '\0' || !token || token[0] == '\0') {
        return -1;
    }

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    active_source_held = active_slot()->status.source_ref_count > 0 &&
        strcmp(active_slot()->status.track_id, track->id) == 0;
    status = slot->status;
    already_requested = slot_has_request(slot, track->id);
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    if (active_source_held) return -1;

    if (already_requested) {
        logLine("ac: prefetch skip same track_id='%s' state=%s\n",
                track->id, audio_cache_state_name(status.state));
        return 0;
    }

    logLine("ac: prefetch request slot=%d track_id='%s'\n", slot->slot_id, track->id);
    return s_mutex_initialized ? post_job(slot, track, token, 0, NULL) : -1;
}

AudioCacheState audio_cache_prefetch_get_state(const char *track_id)
{
    AudioCacheState state;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    state = !track_id || strcmp(prefetch_slot()->status.track_id, track_id) != 0 ||
        prefetch_slot()->retire_requested || !slot_satisfies_request(prefetch_slot())
        ? AUDIO_CACHE_IDLE : prefetch_slot()->status.state;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    return state;
}

int audio_cache_prefetch_cancel(void)
{
    if (!s_mutex_initialized) return -1;
    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    int rc = cancel_job_locked(prefetch_slot());
    sceKernelUnlockLwMutex(&s_status_mutex, 1);
    return rc;
}
AudioCacheState audio_cache_get_state(void)
{
    AudioCacheState state;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    state = active_slot()->retire_requested ? AUDIO_CACHE_IDLE : active_slot()->status.state;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    return state;
}

bool audio_cache_get_status(AudioCacheStatus *out)
{
    if (!out) return false;

    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
        *out = active_slot()->status;
        if (active_slot()->retire_requested) out->state = AUDIO_CACHE_IDLE;
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
    } else {
        *out = s_slots[s_active_slot ? 1 : 0].status;
    }
    return true;
}

bool audio_cache_error_is_network(void)
{
    AudioCacheStatus status;
    if (!audio_cache_get_status(&status) || status.state != AUDIO_CACHE_ERROR)
        return false;
    return status.error_code == YM_ERR_NETWORK ||
           net_http_error_is_network(status.error_code);
}

int audio_cache_get_source(const char *track_id, AudioSourceSnapshot *out)
{
    AcSlot *slot;

    if (!track_id || !out) return -1;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    slot = &s_slots[s_active_slot ? 1 : 0];

    if (strcmp(slot->status.track_id, track_id) != 0 ||
        slot->retire_requested || slot->commit_state == AC_COMMIT_IN_PROGRESS ||
        (slot->status.state != AUDIO_CACHE_PROGRESSIVE_READY &&
         slot->status.state != AUDIO_CACHE_READY) ||
        !slot->status.track_id[0] || !slot->status.path[0]) {
        if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->state = slot->status.state;
    snprintf(out->track_id, sizeof(out->track_id), "%s", slot->status.track_id);
    snprintf(out->path, sizeof(out->path), "%s", slot->status.path);
    out->downloaded_bytes = slot->status.downloaded_bytes;
    out->content_length = slot->status.content_length;
    out->bitrate_kbps = slot->status.bitrate_kbps;
    out->complete = slot->status.complete;
    out->progressive = slot->status.progressive;
    out->error_code = slot->status.error_code;

    slot->status.source_ref_count++;
    logLine("ac: source acquire slot=%d track_id='%s' state=%s refs=%d path='%s' bytes=%d/%d bitrate=%d complete=%d\n",
            slot->slot_id, out->track_id, audio_cache_state_name(out->state),
            slot->status.source_ref_count, out->path,
            (int)out->downloaded_bytes, (int)out->content_length,
            out->bitrate_kbps, out->complete);

    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    return 0;
}

void audio_cache_release_source(const char *track_id)
{
    int i;

    if (!track_id || !track_id[0]) return;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        AcSlot *slot = &s_slots[i];
        if (strcmp(slot->status.track_id, track_id) == 0 &&
            slot->status.source_ref_count > 0) {
            slot->status.source_ref_count--;
            if ((slot->retire_requested || slot->commit_state == AC_COMMIT_PENDING) &&
                slot->status.source_ref_count == 0) {
                (void)wake_slot_locked(slot);
            }
            logLine("ac: source release slot=%d track_id='%s' refs=%d\n",
                    slot->slot_id, track_id, slot->status.source_ref_count);
            break;
        }
    }
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
}
