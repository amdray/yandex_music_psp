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
#include "services/track_download.h"
#include "services/cover_manager.h"
#include "services/ym_api.h"
#include "services/audio_stream_buf.h"
#include "services/mp4_track.h"

#define AUDIO_CACHE_PROGRESSIVE_THRESHOLD (256 * 1024)
/* FLAC раньше стартовал с 256 КБ (~1-2 с аудио) и на медленной сети PSP
 * сразу подхватывал буферизацию. Ждём качнутое 10 с трека, прежде чем
 * отдавать источник плееру. Фолбэк — когда размер/длительность ещё не
 * известны: ~10 с типового FLAC (~1000 kbps). */
#define AUDIO_CACHE_FLAC_PREBUFFER_MS    10000
#define AUDIO_CACHE_FLAC_PREBUFFER_FALLBACK (1280 * 1024)
#define AUDIO_CACHE_SLOT_COUNT 2
#define AUDIO_CACHE_RESUME_VERIFY_BYTES (4 * 1024)
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
    char       path[64];
    char       token[256];
    int        pause_network;
} AcJob;

typedef struct AcSlot AcSlot;
struct AcSlot {
    int              slot_id;
    AudioCacheStatus status;
    AudioStreamBuf   stream_buf;
    // Постоянный воркер: создаётся в init, удаляется только в shutdown.
    // Потоки НИКОГДА не удаляются на живом задании — отмена всегда
    // кооперативная через job_cancel (проброшен и в TLS-слой).
    SceUID           worker_thread;
    SceUID           job_sema;
    AcJob            pending_job;
    int              job_pending;       // под s_status_mutex
    volatile int     job_cancel;        // 1 = текущее задание отменено
    AcSlot          *self_arg;
};

static volatile int s_active_slot = 0;
static char s_hint_next[40];   /* id следующего трека очереди (под hint) */
static volatile int s_shutdown = 0;
static AcSlot s_slots[AUDIO_CACHE_SLOT_COUNT];
static SceLwMutexWorkarea s_status_mutex;
static int s_mutex_initialized = 0;

typedef struct {
    AcSlot *slot;
    char    track_id[40];
    int64_t content_length;
    int64_t resume_write_pos;
    int64_t verify_pos;
    u64     speed_us;       /* периодический лог скорости закачки */
    int64_t speed_bytes;
} AcRamStreamCtx;

static AcSlot *active_slot(void)
{
    return &s_slots[s_active_slot ? 1 : 0];
}

static AcSlot *prefetch_slot(void)
{
    return &s_slots[s_active_slot ? 0 : 1];
}

static void slot_clear_status(AcSlot *slot)
{
    if (!slot) return;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    memset(&slot->status, 0, sizeof(slot->status));
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
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
                            int complete, int progressive, int error_code)
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

    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    if (prev_state != state || downloaded_bytes == 0) {
        logLine("ac: slot=%d source state -> %s track_id='%s' path='%s' bytes=%d/%d complete=%d progressive=%d\n",
                slot->slot_id, audio_cache_state_name(state), track_id ? track_id : "",
                path ? path : "", (int)downloaded_bytes, (int)content_length,
                complete, progressive);
    }
}

static int ac_ram_chunk_cb(const char *data, int size, void *user_data)
{
    AcRamStreamCtx *ctx = (AcRamStreamCtx *)user_data;
    int consumed = 0;
    int rc;
    if (!ctx || !ctx->slot) return -1;
    if (ctx->slot->job_cancel || s_shutdown) {
        return NET_HTTP_STREAM_CANCELLED;
    }
    if (ctx->verify_pos < ctx->resume_write_pos) {
        int64_t verify_left = ctx->resume_write_pos - ctx->verify_pos;
        int verify_now = verify_left < size ? (int)verify_left : size;
        if (asb_compare_at(&ctx->slot->stream_buf, ctx->verify_pos,
                           (const uint8_t *)data, verify_now) != 0) {
            logLine("ac: resume representation mismatch slot=%d track='%s' at=%d len=%d\n",
                    ctx->slot->slot_id, ctx->track_id,
                    (int)ctx->verify_pos, verify_now);
            return -1;
        }
        ctx->verify_pos += verify_now;
        consumed = verify_now;
    }
    if (consumed == size) return 0;

    rc = asb_append(&ctx->slot->stream_buf,
                    (const uint8_t *)data + consumed, size - consumed);
    if (rc != 0 && (ctx->slot->job_cancel || s_shutdown)) {
        return NET_HTTP_STREAM_CANCELLED;
    }
    return rc;
}

static void ac_log_speed(const char *track_id, int64_t absolute,
                         int64_t total, u64 *last_us, int64_t *last_bytes);

static int ac_ram_progress_cb(int written_total, int content_length, void *user_data)
{
    AcRamStreamCtx *ctx = (AcRamStreamCtx *)user_data;
    AcSlot *slot = ctx ? ctx->slot : NULL;
    int progressive;

    if (!slot || !ctx->track_id[0]) return -1;
    if (slot->job_cancel || s_shutdown) {
        return NET_HTTP_STREAM_CANCELLED;
    }

    /* First request allocates the full object. A resumed request must match
     * the coherent buffer snapshot captured immediately before it. */
    if (ctx->resume_write_pos == 0 && written_total == 0) {
        if (content_length <= 0) {
            logLine("ac: no Content-Length for track '%s', refusing\n", ctx->track_id);
            return -1;
        }
        ctx->content_length = (int64_t)content_length;
        if (asb_alloc(&slot->stream_buf, ctx->content_length) != 0) {
            logLine("ac: asb_alloc failed slot=%d track='%s' size=%d\n",
                    slot->slot_id, ctx->track_id, content_length);
            return -1;
        }
        logLine("ac: slot=%d buf allocated %d bytes for '%s'\n",
                slot->slot_id, content_length, ctx->track_id);
    } else if (written_total == ctx->verify_pos &&
               ctx->verify_pos <= ctx->resume_write_pos) {
        AudioStreamBufSnapshot snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        if (asb_get_snapshot(&slot->stream_buf, &snapshot) != 0 ||
            snapshot.write_pos != ctx->resume_write_pos ||
            snapshot.capacity != ctx->content_length ||
            snapshot.content_length != ctx->content_length ||
            content_length != ctx->content_length) {
            logLine("ac: resume snapshot mismatch slot=%d track='%s' request=%d write=%d expected=%d got_total=%d\n",
                    slot->slot_id, ctx->track_id, written_total,
                    (int)snapshot.write_pos, (int)ctx->content_length,
                    content_length);
            return -1;
        }
    }

    {
        AudioStreamBufSnapshot snapshot;
        if (asb_get_snapshot(&slot->stream_buf, &snapshot) != 0) return -1;
        written_total = (int)snapshot.write_pos;
    }
    progressive = (written_total >= AUDIO_CACHE_PROGRESSIVE_THRESHOLD);

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (strcmp(slot->status.track_id, ctx->track_id) == 0) {
        slot->status.downloaded_bytes = (int64_t)written_total;
        slot->status.content_length   = (int64_t)content_length;
        if ((int64_t)written_total > ctx->resume_write_pos) {
            slot->status.error_code = 0;
        }
        if (progressive && slot->status.state == AUDIO_CACHE_DOWNLOADING) {
            slot->status.state = AUDIO_CACHE_PROGRESSIVE_READY;
            slot->status.progressive = 1;
            logLine("ac: slot=%d progressive ready '%s' bytes=%d/%d\n",
                    slot->slot_id, ctx->track_id, written_total, content_length);
        }
    }
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    ac_log_speed(ctx->track_id, (int64_t)written_total,
                 (int64_t)content_length,
                 &ctx->speed_us, &ctx->speed_bytes);
    return 0;
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
static int ac_probe_stream(AcSlot *slot, const char *url, const char *token,
                           AcProbeResult *out)
{
    AcProbeCtx *p;
    int rc;

    if (!slot || !url || !token || !out) return -1;
    memset(out, 0, sizeof(*out));
    /* Проба живёт только внутри вызова: два слота качают одновременно,
     * общий статический буфер давал гонку. */
    p = (AcProbeCtx *)calloc(1, sizeof(AcProbeCtx));
    if (!p) return -1;
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
        return rc;
    }
    if (p->len >= 4 && memcmp(p->buf, "fLaC", 4) == 0) {
        out->is_raw_flac = flac_probe_streaminfo(p->buf, p->len,
                                                 &out->mp4.rate,
                                                 &out->mp4.ch, &out->mp4.bps);
        free(p);
        return out->is_raw_flac ? 0 : -1;
    }
    if (mp4_probe_buffer(p->buf, p->len, &out->mp4) == 0 &&
        out->mp4.kind != MP4_PROBE_NONE) {
        out->is_mp4 = 1;
        free(p);
        return 0;
    }
    free(p);
    return -1;
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
} AcFileDlCtx;

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

    if (*last_us == 0) {
        *last_us = now;
        *last_bytes = absolute;
        return;
    }
    if (now - *last_us >= 5000000ULL) {
        int64_t delta = absolute - *last_bytes;
        int secs = (int)((now - *last_us) / 1000000ULL);
        if (secs > 0 && delta >= 0) {
            logLine("ac: dl speed track='%s' %d/%d %d KB/s\n",
                    track_id, (int)absolute, (int)total,
                    (int)(delta / (int64_t)secs / 1024));
        }
        *last_us = now;
        *last_bytes = absolute;
    }
}

static int flac_file_chunk(const char *data, int size, void *user)
{
    AcFileDlCtx *ctx = (AcFileDlCtx *)user;
    int rc;
    if (!ctx || !data || size <= 0) return -1;
    if (ctx->slot->job_cancel || s_shutdown) {
        return NET_HTTP_STREAM_CANCELLED;
    }
    if (ctx->fd < 0) return -1;
    rc = fs_write(ctx->fd, data, (SceSize)size);
    if (rc != size) {
        logLine("ac: flac file write short slot=%d rc=%d\n",
                ctx->slot->slot_id, rc);
        return -1;
    }
    ctx->session_written += size;
    return 0;
}

static int flac_file_progress(int written_total, int content_length, void *user)
{
    AcFileDlCtx *ctx = (AcFileDlCtx *)user;
    int64_t absolute;
    int progressive;
    if (!ctx) return -1;
    if (ctx->attempt_total <= 0 && content_length > 0) {
        ctx->attempt_total = content_length;
    }
    /* net_http отдаёт УЖЕ абсолютный прогресс (progress_base включает
     * range_start), и content_length — полный размер объекта. Прибавлять
     * base_size повторно нельзя: счётчик обгонял файл ровно на base_size,
     * читатель проходил ворота avail и упирался в короткое чтение —
     * вечный «short read/wait» на фронтире докачки. */
    absolute = (int64_t)written_total;
    /* Размер объекта стал известен — уточняем пребафер (для FLAC это
     * 10 с трека по длительности, не больше полного файла). */
    ctx->progressive_need = ac_prebuffer_need(ctx);
    progressive = absolute >= ctx->progressive_need;
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
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    ctx->attempt_had_progress = 1;
    ac_log_speed(ctx->track_id, absolute,
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

/* Финализация файлового задания: READY + чистка старого кэша. */
static int file_job_ready(AcSlot *slot, const AcJob *job,
                          const char *final_path, int bitrate_kbps)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (fs_getstat(final_path, &st) != 0 || st.st_size <= 0) {
        logLine("ac: final missing after job slot=%d '%s'\n",
                slot->slot_id, final_path);
        logger_flush();
        slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL,
                        NET_HTTP_ERR_PROTOCOL);
        return -1;
    }
    slot_set_source(slot, AUDIO_CACHE_READY, job->track.id, final_path,
                    (int64_t)st.st_size, (int64_t)st.st_size, bitrate_kbps,
                    1, 1, 0);
    logLine("ac: state -> READY (file) slot=%d track='%s' path='%s' size=%d\n",
            slot->slot_id, job->track.id, final_path, (int)st.st_size);
    logger_flush();
    /* Чистка cache_music выполняется в worker_main после задания
     * (ac_trim_music: только текущий + предзагруженный трек). */
    return 0;
}

/* Файловая докачка (flac: raw fLaC либо flac-mp4 как есть; aac: mp4 как
 * есть) с Range-resume. Возвращает 0 (READY/CANCELLED — как RAM путь:
 * отмена не ошибка) или <0. Частичный файл сохраняется для resume и уже
 * с AUDIO_CACHE_PROGRESSIVE_THRESHOLD играется плеером напрямую. */
static int run_file_job(AcSlot *slot, const AcJob *job,
                        const YmApiContext *api_ctx, const char *family,
                        const char *final_path)
{
    unsigned int retry_delay_us = AUDIO_CACHE_RETRY_MIN_US;
    YmDownloadVariant variant;
    YmStatus ym_status;
    char dl_path[64];
    int restarted = 0;

    memset(&variant, 0, sizeof(variant));
    snprintf(dl_path, sizeof(dl_path), "%s", final_path);

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

        ym_status = strcmp(family, "flac") == 0
                        ? ym_api_download_get_flac_raw((YmApiContext *)api_ctx,
                                                       job->track.id,
                                                       &variant)
                        : ym_api_download_get_aac_raw((YmApiContext *)api_ctx,
                                                      job->track.id,
                                                      &variant);
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

        memset(&st, 0, sizeof(st));
        if (fs_getstat(dl_path, &st) == 0 && st.st_size > 0) {
            base_size = (int64_t)st.st_size;
        }

        /* cache_music/ мог отсутствовать (удалён вне приложения): ENOENT
         * на этом открытии ронял ВСЕ файловые кодеки (flac/aac), пока mp3
         * в RAM-ветке работал. http_stream делает ensure для своих sink'ов,
         * но fd здесь открывается раньше — ensure нужен и тут. */
        fs_ensure_dir(dl_path);
        memset(&ctx, 0, sizeof(ctx));
        ctx.slot = slot;
        snprintf(ctx.track_id, sizeof(ctx.track_id), "%s", job->track.id);
        ctx.base_size = base_size;
        ctx.is_flac = strcmp(family, "flac") == 0;
        ctx.bitrate_kbps = variant.bitrate_kbps;
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
        slot_set_source(slot, AUDIO_CACHE_DOWNLOADING, job->track.id,
                        final_path, base_size, base_size,
                        variant.bitrate_kbps, 0,
                        base_size >= ctx.progressive_need,
                        0);
        logLine("ac: %s file start slot=%d track='%s' resume=%d\n",
                family, slot->slot_id, job->track.id, (int)base_size);
        logger_flush();

        rc_dl = http_stream(&(HttpRequest){
                                .method = HTTP_GET, .url = variant.url,
                                .token = api_ctx->oauth_token,
                                .identity_encoding = 1,
                                .range_enabled = base_size > 0,
                                .range_start = (int)base_size,
                                .range_total = 0
                            },
                            &(HttpSink){ .on_chunk = flac_file_chunk,
                                         .on_progress = flac_file_progress,
                                         .user = &ctx });
        fs_close(ctx.fd);
        ctx.fd = -1;

        if (rc_dl == 0) {
            return file_job_ready(slot, job, final_path,
                                  variant.bitrate_kbps);
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

/* Какой кодек выбрала цепочка переговоров/кэша. */
typedef enum {
    DL_NONE = 0,
    DL_FLAC,
    DL_AAC,
    DL_MP3
} AcDlKind;

static int run_download_job(AcSlot *slot, const AcJob *job)
{
    YmApiContext api_ctx;
    YmDownloadVariant variant;
    YmDownloadVariant original_variant;
    YmStatus ym_status;
    int net_paused = job->pause_network;
    char dl_path[64];
    AcDlKind dl_kind = DL_NONE;
    int want_mp3;

    {
        const char *q = ym_api_download_quality();
        want_mp3 = (q && strcmp(q, "mp3") == 0);
    }

    logLine("ac: worker started slot=%d track_id='%s' want_mp3=%d\n",
            slot->slot_id, job->track.id, want_mp3);
    logger_flush();

    slot_clear_status(slot);
    asb_reset(&slot->stream_buf);

    if (job->token[0] == '\0') {
        logLine("ac: token missing slot=%d\n", slot->slot_id);
        logger_flush();
        if (net_paused) cover_manager_resume_network();
        slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, -1);
        return -1;
    }

    api_ctx.oauth_token = job->token;
    api_ctx.timeout_ms = 0;

    /* Хиты кэша по цепочке качества: hq = .flac -> .aac -> .mp3,
     * nq/lq = .aac -> .mp3. Частичный .aac хитом не считается. */
    {
        const char *quality = ym_api_download_quality();
        int try_flac = (quality && strcmp(quality, "hq") == 0);
        SceIoStat st;
        char cand[64];

        /* Выбран формат mp3: .mp3 — первый кандидат кэша. */
        if (want_mp3 &&
            track_download_build_path_for_codec(job->track.id, "mp3",
                                                cand, sizeof(cand)) == 0) {
            memset(&st, 0, sizeof(st));
            logLine("ac: check cache slot=%d '%s'\n", slot->slot_id, cand);
            logger_flush();
            if (fs_getstat(cand, &st) == 0) {
                logLine("ac: cache hit (mp3) slot=%d '%s'\n",
                        slot->slot_id, cand);
                logger_flush();
                if (net_paused) cover_manager_resume_network();
                strncpy(dl_path, cand, sizeof(dl_path) - 1);
                dl_path[sizeof(dl_path) - 1] = '\0';
                dl_kind = DL_MP3;
                slot_set_source(slot, AUDIO_CACHE_READY, job->track.id,
                                dl_path, (int64_t)st.st_size,
                                (int64_t)st.st_size, 0, 1, 1, 0);
                return 0;
            }
        }

        if (try_flac &&
            track_download_build_path_for_codec(job->track.id, "flac",
                                                cand, sizeof(cand)) == 0) {
            memset(&st, 0, sizeof(st));
            logLine("ac: check cache slot=%d '%s'\n", slot->slot_id, cand);
            logger_flush();
            if (fs_getstat(cand, &st) == 0) {
                logLine("ac: cache hit (flac) slot=%d '%s'\n", slot->slot_id, cand);
                logger_flush();
                if (net_paused) cover_manager_resume_network();
                strncpy(dl_path, cand, sizeof(dl_path) - 1);
                dl_path[sizeof(dl_path) - 1] = '\0';
                dl_kind = DL_FLAC;
                slot_set_source(slot, AUDIO_CACHE_READY, job->track.id,
                                dl_path, (int64_t)st.st_size,
                                (int64_t)st.st_size, 0, 1, 1, 0);
                return 0;
            }
        }

        /* Новый формат кэша flac: сырой mp4-контейнер X.mp4 (игрывается
         * напрямую). Частичный — не хит, но докачается с места обрыва. */
        if (try_flac &&
            track_download_build_path_for_codec(job->track.id, "mp4",
                                                cand, sizeof(cand)) == 0) {
            memset(&st, 0, sizeof(st));
            logLine("ac: check cache slot=%d '%s'\n", slot->slot_id, cand);
            logger_flush();
            if (fs_getstat(cand, &st) == 0 &&
                ac_mp4_file_complete(cand, "flac")) {
                logLine("ac: cache hit (flac mp4) slot=%d '%s'\n",
                        slot->slot_id, cand);
                logger_flush();
                if (net_paused) cover_manager_resume_network();
                strncpy(dl_path, cand, sizeof(dl_path) - 1);
                dl_path[sizeof(dl_path) - 1] = '\0';
                dl_kind = DL_FLAC;
                slot_set_source(slot, AUDIO_CACHE_READY, job->track.id,
                                dl_path, (int64_t)st.st_size,
                                (int64_t)st.st_size, 0, 1, 1, 0);
                return 0;
            }
        }

        if (track_download_build_path_for_codec(job->track.id, "aac",
                                                cand, sizeof(cand)) == 0) {
            memset(&st, 0, sizeof(st));
            logLine("ac: check cache slot=%d '%s'\n", slot->slot_id, cand);
            logger_flush();
            if (fs_getstat(cand, &st) == 0 &&
                ac_mp4_file_complete(cand, "aac")) {
                logLine("ac: cache hit (aac) slot=%d '%s'\n", slot->slot_id, cand);
                logger_flush();
                if (net_paused) cover_manager_resume_network();
                strncpy(dl_path, cand, sizeof(dl_path) - 1);
                dl_path[sizeof(dl_path) - 1] = '\0';
                dl_kind = DL_AAC;
                slot_set_source(slot, AUDIO_CACHE_READY, job->track.id,
                                dl_path, (int64_t)st.st_size,
                                (int64_t)st.st_size, 0, 1, 1, 0);
                return 0;
            }
        }

        /* .mp3 — фолбэк-кандидат (при выбранном mp3 уже проверен выше). */
        if (!want_mp3) {
            memset(&st, 0, sizeof(st));
            logLine("ac: check cache slot=%d '%s'\n", slot->slot_id, job->path);
            logger_flush();
            if (fs_getstat(job->path, &st) == 0) {
                logLine("ac: cache hit slot=%d '%s'\n", slot->slot_id, job->path);
                logger_flush();
                if (net_paused) cover_manager_resume_network();
                strncpy(dl_path, job->path, sizeof(dl_path) - 1);
                dl_path[sizeof(dl_path) - 1] = '\0';
                dl_kind = DL_MP3;
                slot_set_source(slot, AUDIO_CACHE_READY, job->track.id,
                                dl_path, (int64_t)st.st_size,
                                (int64_t)st.st_size, 0, 1, 1, 0);
                return 0;
            }
        }
    }

    logLine("ac: fetch url slot=%d track_id='%s' tls_ready=%d net_ready=%d\n",
            slot->slot_id, job->track.id,
            net_tls_config_is_ready(), net_stack_is_ready());
    logger_flush();

    /* Переговоры по цепочке качества: hq = FLAC -> AAC -> MP3,
     * nq/lq = AAC -> MP3. Каждый шаг: get-file-info + проба потока
     * (raw fLaC либо MP4), при отказе — следующий кодек. */
    {
        const char *quality = ym_api_download_quality();
        int try_flac = (quality && strcmp(quality, "hq") == 0);

        /* Формат mp3 выбран явно: codecs=mp3 напрямую, без AAC-шага;
         * при отказе сервера — фолбэк на AAC ниже. */
        if (want_mp3) {
            if (track_download_build_path_for_codec(job->track.id, "mp3",
                                                    dl_path,
                                                    sizeof(dl_path)) == 0) {
                ym_status = ym_api_download_get_mp3_raw(&api_ctx,
                                                         job->track.id,
                                                         &variant);
                if (ym_status == YM_OK) {
                    dl_kind = DL_MP3;
                    logLine("ac: mp3 committed slot=%d track='%s' bitrate=%d\n",
                            slot->slot_id, job->track.id,
                            variant.bitrate_kbps);
                    logger_flush();
                } else {
                    logLine("ac: mp3 unavailable slot=%d track='%s' status=%d fallback\n",
                            slot->slot_id, job->track.id, ym_status);
                    logger_flush();
                }
            }
        }

        if (try_flac) {
            YmDownloadVariant flac_variant;
            memset(&flac_variant, 0, sizeof(flac_variant));
            ym_status = ym_api_download_get_flac_raw(&api_ctx, job->track.id,
                                                     &flac_variant);
            if (ym_status == YM_OK) {
                AcProbeResult pr;
                int probe_rc = ac_probe_stream(slot, flac_variant.url,
                                               api_ctx.oauth_token, &pr);
                int kind_ok = pr.is_raw_flac ||
                              (pr.is_mp4 && pr.mp4.kind == MP4_PROBE_FLAC);
                if (probe_rc == 0 && kind_ok && pr.mp4.rate == 44100 &&
                    pr.mp4.ch == 2 && pr.mp4.bps >= 16 && pr.mp4.bps <= 32) {
                    if (track_download_build_path_for_codec(job->track.id,
                                                           pr.is_mp4 ? "mp4"
                                                                     : "flac",
                                                           dl_path,
                                                           sizeof(dl_path)) == 0) {
                        variant = flac_variant;
                        dl_kind = DL_FLAC;
                        logLine("ac: flac committed slot=%d track='%s' fmt=%d/%d/%d mp4=%d bitrate=%d\n",
                                slot->slot_id, job->track.id,
                                pr.mp4.rate, pr.mp4.ch, pr.mp4.bps,
                                pr.is_mp4, flac_variant.bitrate_kbps);
                        logger_flush();
                    }
                } else {
                    logLine("ac: flac rejected slot=%d track='%s' probe_rc=%d kind=%d fmt=%d/%d/%d\n",
                            slot->slot_id, job->track.id, probe_rc,
                            (int)pr.mp4.kind, pr.mp4.rate, pr.mp4.ch,
                            pr.mp4.bps);
                    logger_flush();
                }
            } else {
                logLine("ac: flac unavailable slot=%d track='%s' status=%d\n",
                        slot->slot_id, job->track.id, ym_status);
                logger_flush();
            }
        }

        if (dl_kind == DL_NONE) {
            YmDownloadVariant aac_variant;
            memset(&aac_variant, 0, sizeof(aac_variant));
            ym_status = ym_api_download_get_aac_raw(&api_ctx, job->track.id,
                                                    &aac_variant);
            if (ym_status == YM_OK) {
                AcProbeResult pr;
                int probe_rc = ac_probe_stream(slot, aac_variant.url,
                                               api_ctx.oauth_token, &pr);
                if (probe_rc == 0 && pr.is_mp4 &&
                    pr.mp4.kind == MP4_PROBE_AAC) {
                    if (track_download_build_path_for_codec(job->track.id,
                                                           "aac", dl_path,
                                                           sizeof(dl_path)) == 0) {
                        variant = aac_variant;
                        dl_kind = DL_AAC;
                        logLine("ac: aac committed slot=%d track='%s' bitrate=%d\n",
                                slot->slot_id, job->track.id,
                                aac_variant.bitrate_kbps);
                        logger_flush();
                    }
                } else {
                    logLine("ac: aac rejected slot=%d track='%s' probe_rc=%d kind=%d\n",
                            slot->slot_id, job->track.id, probe_rc,
                            (int)pr.mp4.kind);
                    logger_flush();
                }
            } else {
                logLine("ac: aac unavailable slot=%d track='%s' status=%d\n",
                        slot->slot_id, job->track.id, ym_status);
                logger_flush();
            }
        }
    }

    if (dl_kind == DL_NONE && !want_mp3) {
        /* MP3-фолбэк: codecs=mp3, lq/nq -> 192, hq -> 320. */
        if (track_download_build_path_for_codec(job->track.id, "mp3",
                                                dl_path, sizeof(dl_path)) != 0) {
            if (net_paused) cover_manager_resume_network();
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, -4);
            return -1;
        }
        ym_status = ym_api_download_get_mp3_raw(&api_ctx, job->track.id, &variant);
    if (ym_status != YM_OK) {
        if (net_paused) cover_manager_resume_network();
        if (slot->job_cancel || s_shutdown) {
            logLine("ac: fetch url cancelled slot=%d track_id='%s'\n",
                    slot->slot_id, job->track.id);
            logger_flush();
            slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id, NULL,
                            NET_HTTP_STREAM_CANCELLED);
            return 0;
        }
        logLine("ac: fetch url FAILED slot=%d track_id='%s' status=%d\n",
                slot->slot_id, job->track.id, ym_status);
        logger_flush();
        slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, (int)ym_status);
        return -1;
    }
        dl_kind = DL_MP3;
    }
    if (dl_kind == DL_NONE) {
        /* Ни один кодек не согласован (mp3 выбранным форматом + AAC
         * отказали) — ошибка, дальше с пустым variant не уходим. */
        if (net_paused) cover_manager_resume_network();
        logLine("ac: no codec available slot=%d track_id='%s'\n",
                slot->slot_id, job->track.id);
        logger_flush();
        slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL,
                        YM_ERR_UNSUPPORTED);
        return -1;
    }
    original_variant = variant;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (strcmp(slot->status.track_id, job->track.id) == 0) {
        slot->status.bitrate_kbps = variant.bitrate_kbps;
    }
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    if (net_paused) cover_manager_resume_network();

    if (slot->job_cancel || s_shutdown) {
        asb_set_cancelled(&slot->stream_buf);
        slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id, NULL,
                        NET_HTTP_STREAM_CANCELLED);
        return 0;
    }

    if (dl_kind == DL_FLAC) {
        return run_file_job(slot, job, &api_ctx, "flac", dl_path);
    }
    if (dl_kind == DL_AAC) {
        return run_file_job(slot, job, &api_ctx, "aac", dl_path);
    }

    slot_set_source(slot, AUDIO_CACHE_DOWNLOADING, job->track.id,
                    dl_path, 0, 0, 0, 0, 0, 0);

    logLine("ac: ram stream start slot=%d track_id='%s'\n",
            slot->slot_id, job->track.id);
    logger_flush();

    {
        AcRamStreamCtx ctx;
        int rc_dl;
        unsigned int retry_delay_us = AUDIO_CACHE_RETRY_MIN_US;

        for (;;) {
            AudioStreamBufSnapshot before;
            int request_start;

            if (asb_get_snapshot(&slot->stream_buf, &before) != 0) {
                rc_dl = NET_HTTP_ERR_PROTOCOL;
                break;
            }
            if (before.content_length > 0 &&
                before.write_pos == before.content_length) {
                rc_dl = 0;
                break;
            }

            memset(&ctx, 0, sizeof(ctx));
            ctx.slot = slot;
            snprintf(ctx.track_id, sizeof(ctx.track_id), "%s", job->track.id);
            ctx.content_length = before.content_length;
            ctx.resume_write_pos = before.write_pos;
            request_start = before.write_pos > AUDIO_CACHE_RESUME_VERIFY_BYTES
                                ? (int)before.write_pos - AUDIO_CACHE_RESUME_VERIFY_BYTES
                                : 0;
            ctx.verify_pos = request_start;

            rc_dl = track_download_mp3_to_ram_from(
                variant.url, request_start,
                before.content_length > 0 ? (int)before.content_length : 0,
                ac_ram_chunk_cb, ac_ram_progress_cb, &ctx);
            if (rc_dl == 0) {
                AudioStreamBufSnapshot after;
                memset(&after, 0, sizeof(after));
                if (asb_get_snapshot(&slot->stream_buf, &after) == 0 &&
                    after.content_length > 0 &&
                    after.write_pos == after.content_length) {
                    break;
                }
                logLine("ac: stream success with incomplete buffer slot=%d track='%s' bytes=%d/%d\n",
                        slot->slot_id, job->track.id,
                        (int)after.write_pos, (int)after.content_length);
                rc_dl = NET_HTTP_ERR_PROTOCOL;
                break;
            }
            if (rc_dl == NET_HTTP_STREAM_CANCELLED ||
                slot->job_cancel || s_shutdown) {
                logLine("ac: ram stream cancelled slot=%d track_id='%s' rc=%d\n",
                        slot->slot_id, job->track.id, rc_dl);
                logger_flush();
                asb_set_cancelled(&slot->stream_buf);
                slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id,
                                NULL, NET_HTTP_STREAM_CANCELLED);
                return 0;
            }
            if (!net_http_error_is_network(rc_dl)) break;

            if (asb_get_snapshot(&slot->stream_buf, &before) != 0) {
                rc_dl = NET_HTTP_ERR_PROTOCOL;
                break;
            }
            slot_set_source(slot,
                            before.write_pos >= AUDIO_CACHE_PROGRESSIVE_THRESHOLD
                                ? AUDIO_CACHE_PROGRESSIVE_READY
                                : AUDIO_CACHE_DOWNLOADING,
                            job->track.id, job->path, before.write_pos,
                            before.content_length, original_variant.bitrate_kbps,
                            0,
                            before.write_pos >= AUDIO_CACHE_PROGRESSIVE_THRESHOLD,
                            rc_dl);
            logLine("ac: ram stream interrupted slot=%d track_id='%s' rc=%d bytes=%d/%d retry_us=%u\n",
                    slot->slot_id, job->track.id, rc_dl,
                    (int)before.write_pos, (int)before.content_length,
                    retry_delay_us);
            logger_flush();

            if (ac_wait_for_retry(slot, job->track.id,
                                  sceKernelGetSystemTimeWide() + retry_delay_us) != 0) {
                asb_set_cancelled(&slot->stream_buf);
                slot_set_status(slot, AUDIO_CACHE_CANCELLED, job->track.id,
                                NULL, NET_HTTP_STREAM_CANCELLED);
                return 0;
            }

            ym_status = ym_api_download_get_mp3_raw(&api_ctx, job->track.id,
                                                     &variant);
            if (ym_status != YM_OK) {
                if (ym_status == YM_ERR_NETWORK) {
                    if (retry_delay_us < AUDIO_CACHE_RETRY_MAX_US / 2)
                        retry_delay_us *= 2;
                    else
                        retry_delay_us = AUDIO_CACHE_RETRY_MAX_US;
                    continue;
                }
                rc_dl = (int)ym_status;
                break;
            }
            if (variant.bitrate_kbps != original_variant.bitrate_kbps ||
                variant.encrypted != original_variant.encrypted ||
                strcmp(variant.codec, original_variant.codec) != 0 ||
                strcmp(variant.transport, original_variant.transport) != 0) {
                logLine("ac: resumed variant mismatch slot=%d track='%s' bitrate=%d/%d codec='%s'/'%s' transport='%s'/'%s'\n",
                        slot->slot_id, job->track.id,
                        original_variant.bitrate_kbps, variant.bitrate_kbps,
                        original_variant.codec, variant.codec,
                        original_variant.transport, variant.transport);
                rc_dl = NET_HTTP_ERR_PROTOCOL;
                break;
            }
            if (retry_delay_us < AUDIO_CACHE_RETRY_MAX_US / 2)
                retry_delay_us *= 2;
            else
                retry_delay_us = AUDIO_CACHE_RETRY_MAX_US;
        }

        if (rc_dl != 0) {
            logLine("ac: ram stream failed terminal slot=%d track_id='%s' rc=%d\n",
                    slot->slot_id, job->track.id, rc_dl);
            logger_flush();
            asb_set_error(&slot->stream_buf);
            slot_set_status(slot, AUDIO_CACHE_ERROR, job->track.id, NULL, rc_dl);
            return -1;
        }
    }

    /* The network object is complete now. Publish that before the slower
     * Memory Stick copy so playback never buffers at EOF waiting for I/O. */
    asb_set_complete(&slot->stream_buf);
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (strcmp(slot->status.track_id, job->track.id) == 0) {
        slot->status.complete = 1;
        slot->status.progressive = 1;
        slot->status.state = AUDIO_CACHE_READY;
        slot->status.error_code = 0;
    }
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    /* Write completed RAM buffer to disk for future cache hits. */
    {
        AudioStreamBufSnapshot snapshot;
        int64_t write_size = 0;
        if (asb_get_snapshot(&slot->stream_buf, &snapshot) == 0) {
            write_size = snapshot.write_pos;
        }
        if (write_size > 0 && slot->stream_buf.buf && job->path[0]) {
            u64 t0 = sceKernelGetSystemTimeWide();
            fs_ensure_dir(job->path);
            SceUID wfd = fs_open(job->path,
                                 PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (wfd < 0) {
                logLine("ac: cache write open failed slot=%d '%s' rc=0x%08X\n",
                        slot->slot_id, job->path, wfd);
            } else {
                const char *p = (const char *)slot->stream_buf.buf;
                int remaining = (int)write_size;
                int write_ok = 1;
                /* 64 KB blocks so cancellation is honoured mid-write:
                 * a full-track write to the Memory Stick takes seconds on
                 * hardware and must not delay the next queued job. */
                while (remaining > 0) {
                    int block = remaining > 65536 ? 65536 : remaining;
                    int rc;
                    if (slot->job_cancel || s_shutdown) {
                        logLine("ac: cache write cancelled slot=%d\n", slot->slot_id);
                        write_ok = 0;
                        break;
                    }
                    rc = fs_write(wfd, p, (SceSize)block);
                    if (rc <= 0) {
                        logLine("ac: cache write error slot=%d rc=%d\n",
                                slot->slot_id, rc);
                        write_ok = 0;
                        break;
                    }
                    p += rc;
                    remaining -= rc;
                }
                fs_close(wfd);
                if (write_ok) {
                    u64 t1 = sceKernelGetSystemTimeWide();
                    logLine("ac: cache written slot=%d '%s' size=%d took=%u ms\n",
                            slot->slot_id, job->path, (int)write_size,
                            (unsigned)((t1 - t0) / 1000));
                    /* Чистка cache_music — в worker_main (ac_trim_music);
                     * обложки чистит сам cover-подсистема после загрузки. */
                } else {
                    fs_remove(job->path);
                }
            }
        }
    }

    logLine("ac: state -> READY (ram) slot=%d track_id='%s'\n",
            slot->slot_id, job->track.id);
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

static int worker_main(SceSize args, void *argp)
{
    AcSlot *slot;

    if (args != sizeof(slot) || !argp) return -1;
    slot = *(AcSlot **)argp;
    if (!slot) return -1;

    log_worker_priority(sceKernelGetThreadId());

    for (;;) {
        AcJob job;
        int have_job = 0;

        sceKernelWaitSema(slot->job_sema, 1, NULL);

        if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
        if (s_shutdown) {
            if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
            break;
        }
        if (slot->job_pending) {
            job = slot->pending_job;
            slot->job_pending = 0;
            slot->job_cancel = 0;
            have_job = 1;
        }
        if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

        if (!have_job) continue;

        /* Чистка cache_music — ПЕРЕД заданием: id забранного job
         * уже известен (extra_id), статус слотов ещё не обновлён. */
        ac_trim_music(job.track.id);
        net_tls_cancel_bind(&slot->job_cancel);
        run_download_job(slot, &job);
        net_tls_cancel_unbind();
    }

    sceKernelExitThread(0);
    return 0;
}

/* Отменяет текущее задание слота и стирает ещё не взятое из ящика.
 * Поток не трогаем: воркер постоянный, отмена только кооперативная. */
static void cancel_job(AcSlot *slot)
{
    if (!slot) return;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    slot->job_pending = 0;
    slot->job_cancel = 1;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    asb_set_cancelled(&slot->stream_buf);
}

static int post_job(AcSlot *slot, const TrackEntry *track, const char *token,
                    int pause_network)
{
    AcJob job;

    if (!slot || !track || track->id[0] == '\0' || !token || token[0] == '\0') {
        return -1;
    }
    if (slot->worker_thread < 0 || slot->job_sema < 0) {
        logLine("ac: slot=%d worker unavailable\n", slot->slot_id);
        slot_set_status(slot, AUDIO_CACHE_ERROR, track->id, NULL, -2);
        return -1;
    }

    memset(&job, 0, sizeof(job));
    if (track_download_build_path(track->id, job.path, sizeof(job.path)) != 0) {
        logLine("ac: build_path failed for id='%s'\n", track->id);
        slot_set_status(slot, AUDIO_CACHE_ERROR, track->id, NULL, -3);
        return -1;
    }
    memcpy(&job.track, track, sizeof(TrackEntry));
    snprintf(job.token, sizeof(job.token), "%s", token);
    job.pause_network = pause_network;

    if (pause_network) {
        cover_manager_pause_network();
    }

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    slot->pending_job = job;
    slot->job_pending = 1;
    slot->job_cancel = 1;  /* завершить текущее задание, если оно есть */
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    sceKernelSignalSema(slot->job_sema, 1);

    logLine("ac: job posted slot=%d track_id='%s' path='%s'\n",
            slot->slot_id, track->id, job.path);
    return 0;
}

void audio_cache_init(void)
{
    int i;

    memset(s_slots, 0, sizeof(s_slots));
    s_active_slot = 0;
    s_shutdown = 0;

    if (sceKernelCreateLwMutex(&s_status_mutex, "ac_status", 0, 0, NULL) >= 0) {
        s_mutex_initialized = 1;
    } else {
        logLine("ac: WARN status mutex create failed\n");
    }

    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        AcSlot *slot = &s_slots[i];
        char name[16];

        slot->slot_id = i;
        slot->worker_thread = -1;
        slot->job_sema = -1;
        slot->self_arg = slot;
        asb_init(&slot->stream_buf);

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
    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        s_slots[i].job_pending = 0;
        s_slots[i].job_cancel = 1;
    }
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        asb_set_cancelled(&s_slots[i].stream_buf);
        if (s_slots[i].job_sema >= 0) {
            sceKernelSignalSema(s_slots[i].job_sema, 1);
        }
    }

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

        /* Буферы живого (утёкшего) потока не освобождаем — он может ещё
         * писать в них; при выходе приложения память заберёт ОС. */
    for (i = 0; i < AUDIO_CACHE_SLOT_COUNT; ++i) {
        asb_destroy(&s_slots[i].stream_buf);
    }

    memset(s_slots, 0, sizeof(s_slots));
    s_active_slot = 0;
    logLine("ac: shutdown\n");
    return 0;
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

    {
        AudioCacheStatus cur;
        audio_cache_get_status(&cur);
        if ((cur.state == AUDIO_CACHE_DOWNLOADING ||
             cur.state == AUDIO_CACHE_PROGRESSIVE_READY ||
             cur.state == AUDIO_CACHE_READY) &&
            strcmp(cur.track_id, track->id) == 0) {
            logLine("ac: skip, same track already %s\n",
                    cur.state == AUDIO_CACHE_READY ? "READY" :
                    (cur.state == AUDIO_CACHE_PROGRESSIVE_READY ? "PROGRESSIVE_READY" : "DOWNLOADING"));
            return 0;
        }
        if (cur.source_ref_count > 0) {
            logLine("ac: start refused, source held for track_id='%s' (wanted '%s')\n",
                    cur.track_id, track->id);
            return -1;
        }
    }

    cancel_job(prefetch);
    slot_clear_status(prefetch);
    return post_job(slot, track, token, 1);
}

void audio_cache_prefetch_start(const TrackEntry *track, const char *token)
{
    AcSlot *slot = prefetch_slot();
    AudioCacheStatus status;

    if (!track || track->id[0] == '\0' || !token || token[0] == '\0') {
        return;
    }

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    status = slot->status;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    if ((status.state == AUDIO_CACHE_DOWNLOADING ||
         status.state == AUDIO_CACHE_PROGRESSIVE_READY ||
         status.state == AUDIO_CACHE_READY) &&
        strcmp(status.track_id, track->id) == 0) {
        logLine("ac: prefetch skip same track_id='%s' state=%s\n",
                track->id, audio_cache_state_name(status.state));
        return;
    }

    logLine("ac: prefetch request slot=%d track_id='%s'\n", slot->slot_id, track->id);
    post_job(slot, track, token, 0);
}

int audio_cache_prefetch_swap(void)
{
    AcSlot *slot = prefetch_slot();
    AcSlot *old  = active_slot();
    AudioCacheState state;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    state = slot->status.state;
    if ((state != AUDIO_CACHE_PROGRESSIVE_READY && state != AUDIO_CACHE_READY) ||
        !slot->status.track_id[0] || !slot->status.path[0]) {
        if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
        return -1;
    }
    s_active_slot = slot->slot_id;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);

    /* Free the old active slot's RAM buffer immediately — the player has
     * finished with it and the new active slot is now in use. */
    asb_reset(&old->stream_buf);

    logLine("ac: prefetch swap active_slot=%d track_id='%s' state=%s\n",
            slot->slot_id, slot->status.track_id,
            audio_cache_state_name(state));
    return 0;
}

AudioCacheState audio_cache_prefetch_get_state(void)
{
    AudioCacheState state;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    state = prefetch_slot()->status.state;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    return state;
}

AudioCacheState audio_cache_get_state(void)
{
    AudioCacheState state;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    state = s_slots[s_active_slot ? 1 : 0].status.state;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    return state;
}

bool audio_cache_get_status(AudioCacheStatus *out)
{
    if (!out) return false;

    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
        *out = s_slots[s_active_slot ? 1 : 0].status;
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

int audio_cache_get_source(AudioSourceSnapshot *out)
{
    AcSlot *slot;

    if (!out) return -1;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    slot = &s_slots[s_active_slot ? 1 : 0];

    if ((slot->status.state != AUDIO_CACHE_PROGRESSIVE_READY &&
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
    out->use_stream_buf = (slot->stream_buf.buf != NULL);
    out->slot_id = slot->slot_id;
    out->stream_buf = out->use_stream_buf ? &slot->stream_buf : NULL;

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
            logLine("ac: source release slot=%d track_id='%s' refs=%d\n",
                    slot->slot_id, track_id, slot->status.source_ref_count);
            break;
        }
    }
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
}
