#include "services/audio_stream_buf.h"

#include <string.h>
#include <malloc.h>
#include <pspkerneltypes.h>

#include "core/logger.h"

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void asb_init(AudioStreamBuf *b)
{
    memset(b, 0, sizeof(*b));
    b->content_length = -1;

    if (sceKernelCreateLwMutex(&b->mutex, "asb_mutex", 0, 0, NULL) < 0) {
        logLine("asb: mutex create failed\n");
        return;
    }
    b->initialized = 1;
    logLine("asb: init\n");
}

int asb_alloc(AudioStreamBuf *b, int64_t content_length)
{
    if (!b->initialized) return -1;
    if (content_length <= 0) {
        logLine("asb: alloc bad content_length=%d\n", (int)content_length);
        return -1;
    }

    /* Free previous buffer if any. */
    if (b->buf) {
        free(b->buf);
        b->buf = NULL;
    }

    b->buf = (uint8_t *)memalign(64, (size_t)content_length);
    if (!b->buf) {
        logLine("asb: memalign failed size=%d\n", (int)content_length);
        return -1;
    }

    sceKernelLockLwMutex(&b->mutex, 1, NULL);
    b->capacity       = content_length;
    b->content_length = content_length;
    b->write_pos      = 0;
    b->complete       = 0;
    b->error          = 0;
    sceKernelUnlockLwMutex(&b->mutex, 1);

    logLine("asb: alloc size=%d\n", (int)content_length);
    return 0;
}

void asb_reset(AudioStreamBuf *b)
{
    if (!b->initialized) return;

    sceKernelLockLwMutex(&b->mutex, 1, NULL);
    if (b->buf) {
        free(b->buf);
        b->buf = NULL;
    }
    b->capacity       = 0;
    b->write_pos      = 0;
    b->content_length = -1;
    b->complete       = 0;
    b->error          = 0;
    sceKernelUnlockLwMutex(&b->mutex, 1);

    logLine("asb: reset\n");
}

void asb_destroy(AudioStreamBuf *b)
{
    if (!b->initialized) return;

    if (b->buf) {
        free(b->buf);
        b->buf = NULL;
    }
    sceKernelDeleteLwMutex(&b->mutex);
    b->initialized = 0;
    logLine("asb: destroyed\n");
}

/* -------------------------------------------------------------------------
 * Producer API
 * ------------------------------------------------------------------------- */

int asb_append(AudioStreamBuf *b, const uint8_t *data, int len)
{
    if (!b->initialized || !data || len <= 0) return -1;

    sceKernelLockLwMutex(&b->mutex, 1, NULL);

    if (b->error || !b->buf) {
        sceKernelUnlockLwMutex(&b->mutex, 1);
        return -1;
    }

    if (b->write_pos + len > b->capacity) {
        logLine("asb: overflow write_pos=%d len=%d capacity=%d\n",
                (int)b->write_pos, len, (int)b->capacity);
        b->error = 1;
        sceKernelUnlockLwMutex(&b->mutex, 1);
        return -1;
    }

    memcpy(b->buf + b->write_pos, data, (size_t)len);
    b->write_pos += len;

    sceKernelUnlockLwMutex(&b->mutex, 1);
    return 0;
}

void asb_set_complete(AudioStreamBuf *b)
{
    int64_t write_pos;
    if (!b->initialized) return;
    sceKernelLockLwMutex(&b->mutex, 1, NULL);
    b->complete = 1;
    write_pos = b->write_pos;
    sceKernelUnlockLwMutex(&b->mutex, 1);
    logLine("asb: complete write_pos=%d\n", (int)write_pos);
}

void asb_set_error(AudioStreamBuf *b)
{
    if (!b->initialized) return;
    sceKernelLockLwMutex(&b->mutex, 1, NULL);
    b->error = 1;
    sceKernelUnlockLwMutex(&b->mutex, 1);
    logLine("asb: error\n");
}

void asb_set_cancelled(AudioStreamBuf *b)
{
    if (!b->initialized) return;
    sceKernelLockLwMutex(&b->mutex, 1, NULL);
    b->error = 1;
    sceKernelUnlockLwMutex(&b->mutex, 1);
    logLine("asb: cancelled\n");
}

/* -------------------------------------------------------------------------
 * Consumer API
 * ------------------------------------------------------------------------- */

int asb_read_at(AudioStreamBuf *b, int64_t source_pos, uint8_t *dst, int want)
{
    int64_t wpos;
    int avail, to_copy;

    if (!b->initialized || !dst || want <= 0) return -1;

    sceKernelLockLwMutex(&b->mutex, 1, NULL);

    if (b->error || !b->buf) {
        sceKernelUnlockLwMutex(&b->mutex, 1);
        return -1;
    }

    if (source_pos < 0) {
        logLine("asb: read_at invalid source_pos=%d\n", (int)source_pos);
        sceKernelUnlockLwMutex(&b->mutex, 1);
        return -1;
    }

    wpos = b->write_pos;
    if (source_pos >= wpos) {
        sceKernelUnlockLwMutex(&b->mutex, 1);
        return 0;  /* not yet available */
    }

    avail   = (int)(wpos - source_pos);
    to_copy = want < avail ? want : avail;

    memcpy(dst, b->buf + source_pos, (size_t)to_copy);

    sceKernelUnlockLwMutex(&b->mutex, 1);
    return to_copy;
}

int asb_compare_at(AudioStreamBuf *b, int64_t source_pos,
                   const uint8_t *data, int len)
{
    int equal;

    if (!b || !data || len < 0 || !b->initialized) return -1;
    sceKernelLockLwMutex(&b->mutex, 1, NULL);
    if (!b->buf || source_pos < 0 || source_pos + len > b->write_pos) {
        sceKernelUnlockLwMutex(&b->mutex, 1);
        return -1;
    }
    equal = memcmp(b->buf + source_pos, data, (size_t)len) == 0;
    sceKernelUnlockLwMutex(&b->mutex, 1);
    return equal ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * Read-only accessors
 * ------------------------------------------------------------------------- */

int asb_get_snapshot(AudioStreamBuf *b, AudioStreamBufSnapshot *out)
{
    if (!b || !out || !b->initialized) return -1;
    sceKernelLockLwMutex(&b->mutex, 1, NULL);
    out->capacity = b->capacity;
    out->write_pos = b->write_pos;
    out->content_length = b->content_length;
    out->complete = b->complete;
    out->error = b->error;
    sceKernelUnlockLwMutex(&b->mutex, 1);
    return 0;
}

int64_t asb_write_pos(AudioStreamBuf *b)
{
    AudioStreamBufSnapshot snapshot;
    return asb_get_snapshot(b, &snapshot) == 0 ? snapshot.write_pos : 0;
}

int64_t asb_content_length(AudioStreamBuf *b)
{
    AudioStreamBufSnapshot snapshot;
    return asb_get_snapshot(b, &snapshot) == 0 ? snapshot.content_length : -1;
}

int asb_is_complete(AudioStreamBuf *b)
{
    AudioStreamBufSnapshot snapshot;
    return asb_get_snapshot(b, &snapshot) == 0 ? snapshot.complete : 0;
}

int asb_is_error(AudioStreamBuf *b)
{
    AudioStreamBufSnapshot snapshot;
    return asb_get_snapshot(b, &snapshot) == 0 ? snapshot.error : 1;
}
