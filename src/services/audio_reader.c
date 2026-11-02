#include "services/audio_reader.h"

#include <malloc.h>
#include <pspkernel.h>
#include <string.h>

#include "core/fs.h"
#include "core/io_profile.h"
#include "core/logger.h"

#define READER_CAPACITY (512 * 1024)
#define READER_CHUNK (64 * 1024)
#define READER_ERROR (-1004)

struct AudioReader {
    AudioSourceSnapshot source;
    unsigned char *buffer;
    int active, stop, error, eof;
    int fd;
    SceUID thread, control, data;
    int started, control_pending, data_pending;
    int64_t base, available, wanted_end;
    int head, have, complete;
    unsigned generation;
    AudioReaderBuffering buffering;
    void *user;
};

static AudioReader s_reader;
static SceLwMutexWorkarea s_mutex;
static int s_initialized;

/* State is authoritative; a semaphore requests a fresh inspection of it.
 * All calls hold s_mutex, as in audio_cache's slot wake protocol. */
static int wake_locked(SceUID sema, int *pending)
{
    int rc;
    if (*pending || sema < 0) return 0;
    rc = sceKernelSignalSema(sema, 1);
    if (rc < 0) s_reader.error = rc;
    else *pending = 1;
    return rc;
}

static void source_changed(const AudioCacheStatus *status, void *user)
{
    AudioReader *r = user;
    sceKernelLockLwMutex(&s_mutex, 1, NULL);
    r->available = status->downloaded_bytes;
    r->complete = status->complete;
    if (status->state == AUDIO_CACHE_ERROR ||
        status->state == AUDIO_CACHE_CANCELLED) {
        r->error = status->error_code < 0 ? status->error_code : READER_ERROR;
    }
    wake_locked(r->data, &r->data_pending);
    wake_locked(r->control, &r->control_pending);
    sceKernelUnlockLwMutex(&s_mutex, 1);
}

/* The cache publishes bytes only after fs_write completes. A descriptor
 * opened before that write may retain the old length. */
static int read_file(AudioReader *r, int64_t offset, void *out, int count)
{
    int got, rc;
    if (fs_lseek(r->fd, offset, PSP_SEEK_SET) != offset) return READER_ERROR;
    got = fs_read(r->fd, out, count);
    if (got >= 0 && got < count) {
        rc = fs_close(r->fd);
        if (rc < 0) return rc;
        r->fd = fs_open(r->source.path, PSP_O_RDONLY, 0777);
        if (r->fd < 0) return r->fd;
        if (fs_lseek(r->fd, offset, PSP_SEEK_SET) != offset) return READER_ERROR;
        got = fs_read(r->fd, out, count);
    }
    return got == count ? got : got < 0 ? got : READER_ERROR;
}

static int reader_worker(SceSize args, void *argp)
{
    AudioReader *r = &s_reader;
    IoProfileLane profile;
    (void)args;
    (void)argp;
    io_profile_begin(&profile, IP_SOURCE_READER);
    for (;;) {
        int count, tail, rc;
        int64_t offset, remaining;
        unsigned generation;
        io_profile_phase(&profile, IP_OTHER);
        sceKernelLockLwMutex(&s_mutex, 1, NULL);
        if (r->stop || r->error) {
            wake_locked(r->data, &r->data_pending);
            sceKernelUnlockLwMutex(&s_mutex, 1);
            break;
        }
        offset = r->base + r->have;
        remaining = r->available - offset;
        count = READER_CAPACITY - r->have;
        if (count > READER_CHUNK) count = READER_CHUNK;
        if (remaining < count) count = remaining > 0 ? (int)remaining : 0;
        /* Batch prefetch; a waiting request or final tail can be shorter. */
        if (count < READER_CHUNK && offset >= r->wanted_end &&
            !(r->complete && remaining == count)) count = 0;
        tail = (r->head + r->have) % READER_CAPACITY;
        if (count > READER_CAPACITY - tail) count = READER_CAPACITY - tail;
        if (count == 0) {
            sceKernelUnlockLwMutex(&s_mutex, 1);
            io_profile_phase(&profile, IP_WAIT);
            rc = sceKernelWaitSema(r->control, 1, NULL);
            sceKernelLockLwMutex(&s_mutex, 1, NULL);
            r->control_pending = 0;
            if (rc < 0) r->error = rc;
            sceKernelUnlockLwMutex(&s_mutex, 1);
            continue;
        }
        generation = r->generation;
        sceKernelUnlockLwMutex(&s_mutex, 1);
        io_profile_phase(&profile, IP_IO);
        rc = read_file(r, offset, r->buffer + tail, count);
        io_profile_phase(&profile, IP_OTHER);
        sceKernelLockLwMutex(&s_mutex, 1, NULL);
        if (rc < 0) r->error = rc;
        else if (generation == r->generation) r->have += count;
        wake_locked(r->data, &r->data_pending);
        sceKernelUnlockLwMutex(&s_mutex, 1);
    }
    io_profile_end(&profile);
    return 0;
}

int audio_reader_init(void)
{
    int rc;
    if (s_initialized) return 0;
    rc = sceKernelCreateLwMutex(&s_mutex, "audio_reader", 0, 0, NULL);
    if (rc < 0) return rc;
    s_reader.fd = s_reader.thread = s_reader.control = s_reader.data = -1;
    s_initialized = 1;
    return 0;
}

AudioReader *audio_reader_open(const AudioSourceSnapshot *source,
                               AudioReaderBuffering buffering, void *user)
{
    AudioReader *r = &s_reader;
    int rc;
    SceUID control, data;
    if (!source) return NULL;
    if (!s_initialized || r->active || source->content_length <= 0) {
        audio_cache_release_source(source->track_id);
        return NULL;
    }
    sceKernelLockLwMutex(&s_mutex, 1, NULL);
    memset(r, 0, sizeof(*r));
    r->active = 1;
    r->fd = r->thread = r->control = r->data = -1;
    r->source = *source;
    r->buffering = buffering;
    r->user = user;
    sceKernelUnlockLwMutex(&s_mutex, 1);
    r->buffer = memalign(64, READER_CAPACITY);
    if (!r->buffer) goto fail;
    r->fd = fs_open(source->path, PSP_O_RDONLY, 0777);
    if (r->fd < 0) goto fail;
    control = sceKernelCreateSema("audio_read_control", 0, 0, 1, NULL);
    data = sceKernelCreateSema("audio_read_data", 0, 0, 1, NULL);
    sceKernelLockLwMutex(&s_mutex, 1, NULL);
    r->control = control;
    r->data = data;
    sceKernelUnlockLwMutex(&s_mutex, 1);
    if (r->control < 0 || r->data < 0) goto fail;
    if (audio_cache_watch_source(source->track_id, source_changed, r) < 0) goto fail;
    r->thread = sceKernelCreateThread("audio_reader", reader_worker,
                                     0x1C, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (r->thread < 0) goto fail;
    rc = sceKernelStartThread(r->thread, 0, NULL);
    if (rc < 0) goto fail;
    r->started = 1;
    return r;
fail:
    audio_reader_close();
    return NULL;
}

const AudioSourceSnapshot *audio_reader_source(const AudioReader *r)
{
    return r ? &r->source : NULL;
}

int audio_reader_read_at(AudioReader *r, int64_t offset, void *out, int count)
{
    unsigned char *dst = out;
    int copied = 0, notified = 0;
    if (r != &s_reader || offset < 0 || count < 0 || (!out && count) ||
        offset > INT64_MAX - count) return READER_ERROR;
    while (copied < count) {
        int need = count - copied, drop, take, first, rc;
        int64_t pos = offset + copied;
        if (need > READER_CAPACITY) need = READER_CAPACITY;
        sceKernelLockLwMutex(&s_mutex, 1, NULL);
        if (!r->active || r->stop || r->error) {
            rc = r->error ? r->error : READER_ERROR;
            sceKernelUnlockLwMutex(&s_mutex, 1);
            return rc;
        }
        if (r->complete && pos >= r->available) {
            r->eof = 1;
            sceKernelUnlockLwMutex(&s_mutex, 1);
            return copied;
        }
        if (pos < r->base || pos > r->base + r->have) {
            r->base = pos;
            r->head = r->have = 0;
            r->generation++;
        } else {
            drop = (int)(pos - r->base);
            r->head = (r->head + drop) % READER_CAPACITY;
            r->base = pos;
            r->have -= drop;
        }
        r->wanted_end = pos + need;
        r->eof = 0;
        take = need;
        if (r->complete && r->available - pos < take)
            take = r->available > pos ? (int)(r->available - pos) : 0;
        {
            u64 now = sceKernelGetSystemTimeWide();
            io_profile_event(IP_SOURCE_BUFFER, r->have, need, now, now);
        }
        wake_locked(r->control, &r->control_pending);
        if (r->have >= take) {
            first = take;
            if (first > READER_CAPACITY - r->head) first = READER_CAPACITY - r->head;
            memcpy(dst + copied, r->buffer + r->head, first);
            memcpy(dst + copied + first, r->buffer, take - first);
            copied += take;
            if (take < need) r->eof = 1;
            sceKernelUnlockLwMutex(&s_mutex, 1);
            if (take < need) return copied;
            continue;
        }
        sceKernelUnlockLwMutex(&s_mutex, 1);
        if (!notified && r->buffering) {
            r->buffering(r->user);
            notified = 1;
        }
        rc = sceKernelWaitSema(r->data, 1, NULL);
        sceKernelLockLwMutex(&s_mutex, 1, NULL);
        r->data_pending = 0;
        if (rc < 0) r->error = rc;
        sceKernelUnlockLwMutex(&s_mutex, 1);
    }
    return copied;
}

int audio_reader_error(AudioReader *r)
{
    int rc;
    if (!r) return READER_ERROR;
    sceKernelLockLwMutex(&s_mutex, 1, NULL);
    rc = r->error ? r->error : r->stop ? READER_ERROR : 0;
    sceKernelUnlockLwMutex(&s_mutex, 1);
    return rc;
}

int audio_reader_at_end(AudioReader *r)
{
    int eof;
    sceKernelLockLwMutex(&s_mutex, 1, NULL);
    eof = r->eof;
    sceKernelUnlockLwMutex(&s_mutex, 1);
    return eof;
}

void audio_reader_cancel(void)
{
    AudioReader *r = &s_reader;
    if (!s_initialized) return;
    sceKernelLockLwMutex(&s_mutex, 1, NULL);
    if (r->active) {
        r->stop = 1;
        wake_locked(r->control, &r->control_pending);
        wake_locked(r->data, &r->data_pending);
    }
    sceKernelUnlockLwMutex(&s_mutex, 1);
}

int audio_reader_close(void)
{
    AudioReader *r = &s_reader;
    int rc;
    if (!s_initialized || !r->active) return 0;
    audio_reader_cancel();
    audio_cache_unwatch_source(r);
    if (r->thread >= 0) {
        if (r->started) {
            rc = sceKernelWaitThreadEnd(r->thread, NULL);
            if (rc < 0) return rc;
            r->started = 0;
        }
        rc = sceKernelDeleteThread(r->thread);
        if (rc < 0) return rc;
        r->thread = -1;
    }
    if (r->fd >= 0) {
        rc = fs_close(r->fd);
        if (rc < 0) return rc;
        r->fd = -1;
    }
    sceKernelLockLwMutex(&s_mutex, 1, NULL);
    if (r->control >= 0) {
        rc = sceKernelDeleteSema(r->control);
        if (rc < 0) goto unlock;
        r->control = -1;
    }
    if (r->data >= 0) {
        rc = sceKernelDeleteSema(r->data);
        if (rc < 0) goto unlock;
        r->data = -1;
    }
    free(r->buffer);
    r->buffer = NULL;
    r->active = 0;
    sceKernelUnlockLwMutex(&s_mutex, 1);
    audio_cache_release_source(r->source.track_id);
    return 0;
unlock:
    sceKernelUnlockLwMutex(&s_mutex, 1);
    logLine("audio_reader: cleanup retained rc=0x%08X\n", rc);
    return rc;
}
