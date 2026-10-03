/* FLAC-транспорт поверх dr_flac (src/services/dr_flac.h, public domain).
 * Один открытый декодер за раз (статический пул, malloc запрещён в аудиопути).
 */
#include "services/ym_flac.h"

#include <string.h>

#include "core/fs.h"
#include "core/logger.h"

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

struct YmFlac {
    drflac *dec;
    int is_file;
    AudioStreamBuf *sbuf;
    int fd;
    int64_t pos;
    YmFlacAvailFn avail;
    void *avail_user;
};

static int64_t ym_flac_avail(struct YmFlac *f)
{
    int64_t a;
    if (!f || !f->avail) return 0;
    a = f->avail(f->avail_user);
    return a < 0 ? 0 : a;
}

static size_t ym_flac_on_read(void *user, void *out, size_t want)
{
    struct YmFlac *f = (struct YmFlac *)user;
    int64_t avail;
    int64_t left;
    if (!f || !out || want == 0) return 0;
    avail = ym_flac_avail(f);
    if (f->pos >= avail) return 0;
    left = avail - f->pos;
    if ((int64_t)want > left) want = (size_t)left;
    if (f->is_file) {
        SceOff rc;
        int got;
        if (f->fd < 0) return 0;
        rc = fs_lseek(f->fd, (SceOff)f->pos, PSP_SEEK_SET);
        if (rc < 0) return 0;
        got = fs_read(f->fd, out, want);
        if (got <= 0) return 0;
        f->pos += got;
        return (size_t)got;
    }
    if (!f->sbuf) return 0;
    {
        int got = asb_read_at(f->sbuf, f->pos, (uint8_t *)out, (int)want);
        if (got <= 0) return 0;
        f->pos += got;
        return (size_t)got;
    }
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
    if (np < 0) return DRFLAC_FALSE;
    f->pos = np;
    return DRFLAC_TRUE;
}

static drflac_bool32 ym_flac_on_tell(void *user, drflac_int64 *cursor)
{
    struct YmFlac *f = (struct YmFlac *)user;
    if (!f || !cursor) return DRFLAC_FALSE;
    *cursor = (drflac_int64)f->pos;
    return DRFLAC_TRUE;
}

static YmFlac s_flac;
static int s_flac_used = 0;

static YmFlac *ym_flac_open_common(void)
{
    static const drflac_allocation_callbacks s_alloc_cbs = {
        NULL, ym_flac_on_malloc, ym_flac_on_realloc, ym_flac_on_free
    };
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

YmFlac *ym_flac_open_ram(AudioStreamBuf *sbuf, YmFlacAvailFn avail, void *avail_user)
{
    YmFlac *f;
    if (!sbuf) return NULL;
    memset(&s_flac, 0, sizeof(s_flac));
    s_flac.is_file = 0;
    s_flac.sbuf = sbuf;
    s_flac.fd = -1;
    s_flac.pos = 0;
    s_flac.avail = avail;
    s_flac.avail_user = avail_user;
    f = ym_flac_open_common();
    if (!f) return NULL;
    return f;
}

YmFlac *ym_flac_open_file(int fd, int64_t content_length,
                          YmFlacAvailFn avail, void *avail_user)
{
    YmFlac *f;
    (void)content_length;
    if (fd < 0) return NULL;
    memset(&s_flac, 0, sizeof(s_flac));
    s_flac.is_file = 1;
    s_flac.sbuf = NULL;
    s_flac.fd = fd;
    s_flac.pos = 0;
    s_flac.avail = avail;
    s_flac.avail_user = avail_user;
    f = ym_flac_open_common();
    if (!f) return NULL;
    return f;
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
    return (int64_t)got;
}

int ym_flac_seek_frame(YmFlac *f, int64_t frame)
{
    if (!f || !f->dec || frame < 0) return -2;
    if (drflac_seek_to_pcm_frame(f->dec, (drflac_uint64)frame)) {
        return 0;
    }
    /* Эвристика недокачанного: цель за пределами доступного — подождать. */
    if (frame * 4 + 8192 > ym_flac_avail(f)) {
        return -1;
    }
    return -2;
}

void ym_flac_close(YmFlac *f)
{
    if (!f || !f->dec) return;
    drflac_close(f->dec);
    f->dec = NULL;
    s_flac_used = 0;
    ym_flac_pool_reset();
}
