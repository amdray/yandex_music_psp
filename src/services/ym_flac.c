/* FLAC-транспорт поверх dr_flac (src/services/dr_flac.h, public domain).
 * Один открытый декодер за раз (статический пул, malloc запрещён в аудиопути).
 */
#include "services/ym_flac.h"

#include <limits.h>
#include <string.h>

#include "core/logger.h"
#include "services/mp4_track.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wtype-limits"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#pragma GCC diagnostic pop

#define YM_FLAC_POOL_SIZE (64 * 1024)
#define YM_FLAC_ALLOC_MAX 8

static unsigned char s_pool[YM_FLAC_POOL_SIZE] __attribute__((aligned(16)));
static size_t s_pool_top = 0;

typedef struct {
    void *ptr;
    size_t size;
} YmFlacAlloc;

static YmFlacAlloc s_allocs[YM_FLAC_ALLOC_MAX];
static int s_alloc_count = 0;

static void ym_flac_pool_reset(void)
{
    s_pool_top = 0;
    s_alloc_count = 0;
    memset(s_allocs, 0, sizeof(s_allocs));
}

static void *ym_flac_on_malloc(size_t sz, void *user)
{
    size_t aligned;
    void *p;
    (void)user;
    if (sz == 0) return NULL;
    aligned = (sz + 15) & ~(size_t)15;
    if (aligned > sizeof(s_pool) - s_pool_top) return NULL;
    if (s_alloc_count >= YM_FLAC_ALLOC_MAX) return NULL;
    p = s_pool + s_pool_top;
    s_pool_top += aligned;
    s_allocs[s_alloc_count].ptr = p;
    s_allocs[s_alloc_count].size = sz;
    s_alloc_count++;
    return p;
}

static void *ym_flac_on_realloc(void *p, size_t sz, void *user)
{
    int i;
    size_t copy = 0;
    void *np;
    (void)user;
    if (!p) return ym_flac_on_malloc(sz, user);
    if (sz == 0) return NULL;
    for (i = 0; i < s_alloc_count; i++) {
        if (s_allocs[i].ptr == p) {
            copy = s_allocs[i].size < sz ? s_allocs[i].size : sz;
            break;
        }
    }
    np = ym_flac_on_malloc(sz, user);
    if (np && copy > 0) memcpy(np, p, copy);
    return np;
}

static void ym_flac_on_free(void *p, void *user)
{
    (void)p;
    (void)user;
}

static const drflac_allocation_callbacks s_alloc_cbs = {
    NULL, ym_flac_on_malloc, ym_flac_on_realloc, ym_flac_on_free
};

struct YmFlac {
    drflac *dec;
    AudioReader *reader;
    int64_t pos, stream_size;
    int eof, read_error;
    /* flac-mp4: виртуальный поток = hdr (fLaC+STREAMINFO из dfLa),
     * затем нагрузки сэмплов; pos — виртуальный, смещения mp4
     * разрешаются через курсор по таблице сэмплов. */
    int mode_mp4;
    Mp4Track *mt;
    unsigned char hdr[512];
    int hdr_len;
    int c_valid;
    int c_idx;
    int64_t c_vbase;
    int64_t c_off;
    int64_t c_size;
};

static YmFlac s_flac;

int ym_flac_at_end(YmFlac *f)
{
    if (!f || !f->dec) return 1;
    if (f->dec->totalPCMFrameCount)
        return f->dec->currentPCMFrame >= f->dec->totalPCMFrameCount;
    return f->mode_mp4 ? f->eof : audio_reader_at_end(f->reader);
}

static int mp4_cursor_load(YmFlac *f, int idx, int64_t vbase)
{
    int64_t off = 0;
    int size = 0;
    if (mp4_track_sample_at(f->mt, idx, &off, &size) != 0 || size <= 0) {
        f->c_valid = 0;
        return -1;
    }
    f->c_idx = idx;
    f->c_vbase = vbase;
    f->c_off = off;
    f->c_size = size;
    f->c_valid = 1;
    return 0;
}

static int mp4_cursor_to(YmFlac *f, int64_t V)
{
    if (V < 0) return -1;
    if (f->c_valid && V < f->c_vbase) f->c_valid = 0;
    if (!f->c_valid && mp4_cursor_load(f, 0, 0) != 0) return -1;
    while (V >= f->c_vbase + f->c_size) {
        if (mp4_cursor_load(f, f->c_idx + 1,
                            f->c_vbase + f->c_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static size_t ym_flac_mp4_read(YmFlac *f, void *out, size_t want)
{
    unsigned char *dst = (unsigned char *)out;
    int64_t v = f->pos;
    size_t n = 0;

    if (!f->mt) return 0;
    while (want > 0) {
        if (v < (int64_t)f->hdr_len) {
            size_t k = (size_t)f->hdr_len - (size_t)v;
            if (k > want) k = want;
            memcpy(dst + n, f->hdr + v, k);
            v += (int64_t)k;
            n += k;
            want -= k;
            continue;
        }
        if (mp4_cursor_to(f, v - (int64_t)f->hdr_len) != 0) {
            if (f->c_idx == mp4_track_sample_count(f->mt) - 1 &&
                v - f->hdr_len == f->c_vbase + f->c_size) f->eof = 1;
            else f->read_error = 1;
            break;
        }
        {
            int64_t local = v - (int64_t)f->hdr_len - f->c_vbase;
            size_t k;
            int got;
            k = (size_t)(f->c_size - local);
            if (k > want) k = want;
            got = audio_reader_read_at(f->reader, f->c_off + local, dst + n, (int)k);
            if (got != (int)k) {
                f->read_error = 1;
                break;
            }
            v += got;
            n += (size_t)got;
            want -= (size_t)got;
        }
    }
    f->pos = v;
    return n;
}

static size_t ym_flac_on_read(void *user, void *out, size_t want)
{
    struct YmFlac *f = (struct YmFlac *)user;
    int got;
    if (!f || !out || want == 0 || want > INT_MAX) return 0;
    if (f->mode_mp4) return ym_flac_mp4_read(f, out, want);
    got = audio_reader_read_at(f->reader, f->pos, out, (int)want);
    if (got < 0) { f->read_error = 1; return 0; }
    f->pos += got;
    return (size_t)got;
}

static drflac_bool32 ym_flac_on_seek(void *user, int offset, drflac_seek_origin origin)
{
    struct YmFlac *f = (struct YmFlac *)user;
    int64_t np;
    if (!f) return DRFLAC_FALSE;
    if (origin == DRFLAC_SEEK_SET) {
        np = (int64_t)offset;
    } else if (origin == DRFLAC_SEEK_CUR) {
        np = f->pos + (int64_t)offset;
    } else {
        return DRFLAC_FALSE;
    }
    if (np < 0 || np > f->stream_size) return DRFLAC_FALSE;
    f->pos = np;
    f->eof = 0;
    return DRFLAC_TRUE;
}

static drflac_bool32 ym_flac_on_tell(void *user, drflac_int64 *cursor)
{
    struct YmFlac *f = (struct YmFlac *)user;
    if (!f || !cursor) return DRFLAC_FALSE;
    *cursor = (drflac_int64)f->pos;
    return DRFLAC_TRUE;
}

static int s_flac_used = 0;

static YmFlac *ym_flac_open_common(void)
{
    drflac *dec;
    if (s_flac_used) return NULL;
    ym_flac_pool_reset();
    /* s_flac уже заполнен вызывателем (курсор нужен колбэкам СРАЗУ). */
    dec = drflac_open(ym_flac_on_read, ym_flac_on_seek, ym_flac_on_tell,
                      &s_flac, &s_alloc_cbs);
    if (!dec) {
        logLine("ym_flac: drflac_open NULL pool_top=%u allocs=%d\n",
                (unsigned)s_pool_top, s_alloc_count);
        logger_flush();
        ym_flac_pool_reset();
        return NULL;
    }
    s_flac.dec = dec;
    s_flac_used = 1;
    return &s_flac;
}

YmFlac *ym_flac_open_file(AudioReader *reader)
{
    if (!reader || s_flac_used) return NULL;
    memset(&s_flac, 0, sizeof(s_flac));
    s_flac.reader = reader;
    s_flac.stream_size = audio_reader_source(reader)->content_length;
    return ym_flac_open_common();
}

YmFlac *ym_flac_open_mp4(AudioReader *reader)
{
    YmFlac *f;
    if (!reader || s_flac_used) return NULL;
    memset(&s_flac, 0, sizeof(s_flac));
    s_flac.reader = reader;
    s_flac.mode_mp4 = 1;
    s_flac.mt = mp4_track_open_reader(reader);
    if (!s_flac.mt) return NULL;
    if (!mp4_track_is_flac(s_flac.mt)) goto fail;
    s_flac.hdr_len = mp4_track_flac_header(s_flac.mt, s_flac.hdr,
                                          (int)sizeof(s_flac.hdr));
    if (s_flac.hdr_len <= 0) goto fail;
    s_flac.stream_size = s_flac.hdr_len;
    {
        int i, count = mp4_track_sample_count(s_flac.mt);
        for (i = 0; i < count; ++i) {
            int64_t offset;
            int size;
            if (mp4_track_sample_at(s_flac.mt, i, &offset, &size) != 0) goto fail;
            s_flac.stream_size += size;
        }
    }
    f = ym_flac_open_common();
    if (!f) goto fail;
    return f;
fail:
    mp4_track_close(s_flac.mt);
    s_flac.mt = NULL;
    return NULL;
}

int ym_flac_format(YmFlac *f, YmFlacFormat *out)
{
    if (!f || !f->dec || !out) return -1;
    out->sample_rate = (int)f->dec->sampleRate;
    out->channels = (int)f->dec->channels;
    out->bits_per_sample = (int)f->dec->bitsPerSample;
    out->total_frames = (int64_t)f->dec->totalPCMFrameCount;
    return 0;
}

int64_t ym_flac_read_s16(YmFlac *f, short *pcm, int frames)
{
    drflac_uint64 got;
    if (!f || !f->dec || !pcm || frames <= 0) return -1;
    got = drflac_read_pcm_frames_s16(f->dec, (drflac_uint64)frames,
                                    (drflac_int16 *)pcm);
    if (f->read_error || audio_reader_error(f->reader)) return -1;
    return (int64_t)got;
}

int ym_flac_seek_frame(YmFlac *f, int64_t frame)
{
    if (!f || !f->dec || frame < 0 || f->read_error) return -1;
    return drflac_seek_to_pcm_frame(f->dec, (drflac_uint64)frame) &&
           !f->read_error && !audio_reader_error(f->reader) ? 0 : -1;
}

void ym_flac_close(YmFlac *f)
{
    if (!f) return;
    if (f->dec) {
        drflac_close(f->dec);
    }
    f->dec = NULL;
    if (f->mt) {
        mp4_track_close(f->mt);
        f->mt = NULL;
    }
    s_flac_used = 0;
    ym_flac_pool_reset();
}
