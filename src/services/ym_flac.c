/* FLAC-транспорт поверх dr_flac (src/services/dr_flac.h, public domain).
 * Один открытый декодер за раз (статический пул, malloc запрещён в аудиопути).
 */
#include "services/ym_flac.h"

#include <malloc.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/fs.h"
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

/* RAM read-ahead: окно 512К, фоновый поток читает с MS кусками 64К. */
#define YM_FLAC_RA_SIZE  (512 * 1024)
#define YM_FLAC_RA_CHUNK (64 * 1024)

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
    int is_file;
    AudioStreamBuf *sbuf;
    int *fdp; /* fd владельца (закроет сам); переоткрываем через него */
    char path[128]; /* тот же файл — для переоткрытия fd */
    int64_t pos;
    YmFlacAvailFn avail;
    void *avail_user;
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
    /* Ресинк после «залипшего» нулевого чтения (граница докачки). */
    drflac_uint64 resync_target;
    int resync_active;
    /* Ожидание фронтира докачки (см. YmFlacWaitFn в ym_flac.h). */
    YmFlacWaitFn wait;
    void *wait_user;
    /* RAM read-ahead (файловые режимы): окно на СВОЕМ fd, фоновый
     * поток читает вперёд с Memory Stick крупными кусками — декодер
     * берёт байты из RAM и не замирает на задержках MS (фризы на
     * полностью скачанном флаке). */
    int ra_fd;
    unsigned char *ra_buf;
    int64_t ra_base;
    int ra_have;
    int64_t ra_want;
    volatile int ra_stop;
    SceUID ra_thread;
    SceLwMutexWorkarea ra_mu;
    int ra_ok;
};

static YmFlac s_flac; /* единый пул: ra-поток работает через него */

static int64_t ym_flac_avail(struct YmFlac *f)
{
    int64_t a;
    if (!f || !f->avail) return 0;
    a = f->avail(f->avail_user);
    return a < 0 ? 0 : a;
}

/* Переоткрытие fd владельца: PSP-драйвер снимает размер файла при open
 * и не видит его роста — читатель упирается в EOF момента open, хотя
 * файл уже докачан (avail говорит «байты есть»). Новый fd закроет
 * владелец (плеер). */
static int flac_refresh_fd(struct YmFlac *f)
{
    int old;
    int nfd;

    if (!f->fdp || !f->path[0]) return -1;
    old = *f->fdp;
    nfd = fs_open(f->path, PSP_O_RDONLY, 0777);
    if (nfd < 0) {
        logLine("ym_flac: fd refresh fail '%s' rc=%d\n", f->path, nfd);
        return -1;
    }
    fs_close(old);
    *f->fdp = nfd;
    logLine("ym_flac: fd refreshed '%s' %d -> %d\n", f->path, old, nfd);
    return 0;
}

/* --- RAM read-ahead ------------------------------------------------ */

/* Попытка прочитать из RAM-окна. Возвращает len либо -1 (нет в окне). */
static int ra_try_read(struct YmFlac *f, int64_t abs, void *dst, int len)
{
    int got = -1;
    if (!f->ra_buf || !f->ra_ok || len <= 0) return -1;
    sceKernelLockLwMutex(&f->ra_mu, 1, NULL);
    f->ra_want = abs; /* следим за позицией читателя */
    if (abs >= f->ra_base && abs + len <= f->ra_base + f->ra_have) {
        memcpy(dst, f->ra_buf + (abs - f->ra_base), (size_t)len);
        got = len;
    }
    sceKernelUnlockLwMutex(&f->ra_mu, 1);
    return got;
}

/* Прямое чтение с fd декодера (fallback, когда окно не покрывает). */
static int ra_direct_read(struct YmFlac *f, int64_t abs, void *dst, int len)
{
    int got;

    if (!f->fdp || *f->fdp < 0 || len <= 0) return -1;
    if (fs_lseek(*f->fdp, abs, PSP_SEEK_SET) != abs) return -1;
    got = fs_read(*f->fdp, dst, (size_t)len);
    /* Запрос в пределах avail: короткое чтение = fd устарел (не видит
     * рост файла) — переоткрываем и повторяем чтение. */
    if (got >= 0 && got < len && abs + len <= ym_flac_avail(f) &&
        flac_refresh_fd(f) == 0 &&
        fs_lseek(*f->fdp, abs, PSP_SEEK_SET) == abs) {
        got = fs_read(*f->fdp, dst, (size_t)len);
    }
    return got;
}

/* Фоновое наполнение окна. Живёт на отдельном fd (ra_fd) — конфликт
 * позиций с fd декодера исключён. Читает только в пределах скачанного
 * префикса (avail), чтобы окно не ушло вперед докачки. */
static int ra_thread_main(SceSize args, void *argp)
{
    struct YmFlac *f = &s_flac;
    (void)args;
    (void)argp;
    while (!f->ra_stop) {
        int64_t want, base, avail;
        int have, fill;
        if (!f->ra_buf || f->ra_fd < 0 || !f->ra_ok) break;
        sceKernelLockLwMutex(&f->ra_mu, 1, NULL);
        want = f->ra_want;
        base = f->ra_base;
        have = f->ra_have;
        if (want < base || base + have - want < (int64_t)(YM_FLAC_RA_SIZE / 2)) {
            if (want >= base && want < base + have) {
                /* Скольжение окна: хвост за позицией читателя сохраняем.
                 * Старое поведение (обнуление окна) заставляло декодера
                 * на каждом такте (~256К потребления) уходить в прямое
                 * чтение с MS, которое конфликтует с записью докачки —
                 * отсюда щелчки каждые ~2 секунды. */
                int keep = (int)(base + have - want);
                if (keep > 0) {
                    memmove(f->ra_buf,
                            f->ra_buf + (size_t)(want - base),
                            (size_t)keep);
                }
                f->ra_base = want;
                f->ra_have = keep;
            } else {
                f->ra_base = want & ~(int64_t)4095;
                f->ra_have = 0;
            }
            base = f->ra_base;
            have = f->ra_have;
        }
        fill = have < YM_FLAC_RA_SIZE;
        sceKernelUnlockLwMutex(&f->ra_mu, 1);
        if (!fill) {
            sceKernelDelayThread(20 * 1000);
            continue;
        }
        avail = ym_flac_avail(f);
        {
            int chunk = YM_FLAC_RA_CHUNK;
            /* Не больше свободного места в окне: без этого при have>448К
             * fs_read писал до 64К за конец ra_buf (512К) — порча кучи,
             * вылеты в _Balloc/mallinfo через секунды после старта FLAC. */
            if (chunk > YM_FLAC_RA_SIZE - have) {
                chunk = YM_FLAC_RA_SIZE - have;
            }
            if (avail > 0 && base + have + chunk > avail) {
                chunk = (int)(avail - (base + have));
            }
            if (chunk <= 0) {
                sceKernelDelayThread(30 * 1000);
                continue;
            }
            if (fs_lseek(f->ra_fd, base + have, PSP_SEEK_SET) != base + have) {
                sceKernelDelayThread(30 * 1000);
                continue;
            }
            {
                int got = fs_read(f->ra_fd, f->ra_buf + have, (size_t)chunk);
                if (got >= 0 && got < chunk && base + have + got < avail) {
                    /* avail говорит «данные дальше уже на диске», а fd
                     * короче — fd устарел (не видит рост файла): берём
                     * новый и в следующей итерации читаем заново. */
                    int nfd = fs_open(f->path, PSP_O_RDONLY, 0777);
                    if (nfd >= 0) {
                        fs_close(f->ra_fd);
                        f->ra_fd = nfd;
                        logLine("ym_flac: read-ahead fd refreshed '%s'\n",
                                f->path);
                        continue;
                    }
                }
                if (got > 0) {
                    sceKernelLockLwMutex(&f->ra_mu, 1, NULL);
                    if (f->ra_base == base && f->ra_have == have) {
                        f->ra_have += got;
                    }
                    sceKernelUnlockLwMutex(&f->ra_mu, 1);
                } else {
                    /* 0 = файл короче (стрим): ждём докачки; <0 = ошибка MS. */
                    sceKernelDelayThread((got < 0 ? 50 : 30) * 1000);
                }
            }
        }
    }
    return sceKernelExitDeleteThread(0);
}

static void ra_start(struct YmFlac *f, const char *path)
{
    if (!path || f->ra_fd >= 0) return;
    f->ra_buf = (unsigned char *)memalign(64, YM_FLAC_RA_SIZE);
    if (!f->ra_buf) {
        logLine("ym_flac: read-ahead alloc fail (%d bytes)\n", YM_FLAC_RA_SIZE);
        return;
    }
    f->ra_fd = fs_open(path, PSP_O_RDONLY, 0777);
    if (f->ra_fd < 0) {
        logLine("ym_flac: read-ahead open fail '%s'\n", path);
        free(f->ra_buf);
        f->ra_buf = NULL;
        return;
    }
    if (sceKernelCreateLwMutex(&f->ra_mu, "ymflac_ra", 0, 0, NULL) < 0) {
        logLine("ym_flac: read-ahead mutex fail\n");
        fs_close(f->ra_fd);
        f->ra_fd = -1;
        free(f->ra_buf);
        f->ra_buf = NULL;
        return;
    }
    f->ra_ok = 1;
    f->ra_base = 0;
    f->ra_have = 0;
    f->ra_want = 0;
    f->ra_stop = 0;
    f->ra_thread = sceKernelCreateThread("flac_ra", ra_thread_main,
                                         0x28, 0x4000, PSP_THREAD_ATTR_USER,
                                         NULL);
    if (f->ra_thread < 0) {
        logLine("ym_flac: read-ahead thread fail rc=%d\n", (int)f->ra_thread);
        f->ra_ok = 0;
        sceKernelDeleteLwMutex(&f->ra_mu);
        fs_close(f->ra_fd);
        f->ra_fd = -1;
        free(f->ra_buf);
        f->ra_buf = NULL;
        return;
    }
    sceKernelStartThread(f->ra_thread, 0, NULL);
    logLine("ym_flac: read-ahead on '%s' window=%dK\n",
            path, YM_FLAC_RA_SIZE / 1024);
}

static void ra_stop_and_free(struct YmFlac *f)
{
    if (f->ra_thread >= 0) {
        f->ra_stop = 1;
        sceKernelWaitThreadEnd(f->ra_thread, NULL);
        sceKernelDeleteThread(f->ra_thread);
        f->ra_thread = -1;
    }
    if (f->ra_ok) {
        f->ra_ok = 0;
        sceKernelDeleteLwMutex(&f->ra_mu);
    }
    if (f->ra_fd >= 0) {
        fs_close(f->ra_fd);
        f->ra_fd = -1;
    }
    if (f->ra_buf) {
        free(f->ra_buf);
        f->ra_buf = NULL;
    }
}

void ym_flac_set_wait(YmFlac *f, YmFlacWaitFn wait, void *wait_user)
{
    if (!f) return;
    f->wait = wait;
    f->wait_user = wait_user;
}

int ym_flac_at_end(YmFlac *f)
{
    if (!f || !f->dec) return 1;
    if (f->dec->totalPCMFrameCount == 0) return 1; /* стрим: конец по complete */
    return f->dec->currentPCMFrame + 8192 >= f->dec->totalPCMFrameCount;
}

/* Курсор виртуального потока -> сэмпл mp4. rc==-2 (ростовой файл)
 * допустим: off/size уже посчитаны, готовность проверяет читатель. */
static int mp4_cursor_load(YmFlac *f, int idx, int64_t vbase)
{
    int64_t off = 0;
    int size = 0;
    if (mp4_track_sample_at(f->mt, idx, &off, &size) == -1 || size <= 0) {
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
    int64_t dl = ym_flac_avail(f);
    size_t n = 0;

    if (!f->mt || !f->fdp || *f->fdp < 0) return 0;
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
        if (mp4_cursor_to(f, v - (int64_t)f->hdr_len) != 0) break;
        {
            int64_t local = v - (int64_t)f->hdr_len - f->c_vbase;
            size_t k;
            int got;
            if (f->c_off + f->c_size > dl) {
                /* Фронт докачки: ждём байты прямо здесь, пока dr_flac не
                 * получил 0 посреди трека (его слепок «конец потока» не
                 * сбрасывается повтором -> ложный EOF -> скип трека). */
                if (f->wait &&
                    f->wait(f->wait_user, f->c_off + f->c_size) == 0) {
                    dl = ym_flac_avail(f);
                    continue;
                }
                break; /* данных не будет (остановка/ошибка/конец) */
            }
            k = (size_t)(f->c_size - local);
            if (k > want) k = want;
            got = ra_try_read(f, f->c_off + local, dst + n, (int)k);
            if (got < 0) {
                if (fs_lseek(*f->fdp, f->c_off + local, PSP_SEEK_SET) !=
                    f->c_off + local) {
                    break;
                }
                got = fs_read(*f->fdp, dst + n, (SceSize)k);
                /* Весь сэмпл в пределах dl (проверено выше): короткое
                 * чтение = fd устарел (не видит рост файла) —
                 * переоткрываем и повторяем. */
                if (got >= 0 && (size_t)got < k &&
                    f->c_off + f->c_size <= dl &&
                    flac_refresh_fd(f) == 0 &&
                    fs_lseek(*f->fdp, f->c_off + local, PSP_SEEK_SET) ==
                        f->c_off + local) {
                    got = fs_read(*f->fdp, dst + n, (SceSize)k);
                }
            }
            if (got <= 0) {
                /* 0 при заявленном куске сэмпла = файл короче таблицы:
                 * точка фронтира докачки или рассинхрон — логируем контекст. */
                logLine("ym_flac: mp4 short read V=%d off=%d want=%d got=%d dl=%d\n",
                        (int)v, (int)(f->c_off + local), (int)k, got,
                        (int)dl);
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
    int64_t avail;
    int64_t left;
    if (!f || !out || want == 0) return 0;
    if (f->mode_mp4) return ym_flac_mp4_read(f, out, want);
    avail = ym_flac_avail(f);
    if (f->pos >= avail) {
        /* Фронт докачки: ждём в читателе (см. YmFlacWaitFn) — нулевое
         * чтение посреди трека травит битовый ресивер dr_flac. */
        if (f->wait && f->wait(f->wait_user, f->pos + 1) == 0) {
            avail = ym_flac_avail(f);
            if (f->pos >= avail) return 0;
        } else {
            return 0;
        }
    }
    left = avail - f->pos;
    if ((int64_t)want > left) want = (size_t)left;
    if (f->is_file) {
        int got;
        if (!f->fdp || *f->fdp < 0) return 0;
        got = ra_try_read(f, f->pos, out, (int)want);
        if (got < 0) got = ra_direct_read(f, f->pos, out, (int)want);
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

YmFlac *ym_flac_open_ram(AudioStreamBuf *sbuf, YmFlacAvailFn avail, void *avail_user)
{
    YmFlac *f;
    if (!sbuf) return NULL;
    memset(&s_flac, 0, sizeof(s_flac));
    s_flac.ra_fd = -1;
    s_flac.ra_thread = -1;
    s_flac.is_file = 0;
    s_flac.sbuf = sbuf;
    s_flac.fdp = NULL;
    s_flac.pos = 0;
    s_flac.avail = avail;
    s_flac.avail_user = avail_user;
    f = ym_flac_open_common();
    if (!f) return NULL;
    return f;
}

YmFlac *ym_flac_open_file(int *fd, const char *path,
                          YmFlacAvailFn avail, void *avail_user)
{
    YmFlac *f;
    if (!fd || *fd < 0 || !path) return NULL;
    memset(&s_flac, 0, sizeof(s_flac));
    s_flac.ra_fd = -1;
    s_flac.ra_thread = -1;
    s_flac.is_file = 1;
    s_flac.sbuf = NULL;
    s_flac.fdp = fd;
    snprintf(s_flac.path, sizeof(s_flac.path), "%s", path);
    s_flac.pos = 0;
    s_flac.avail = avail;
    s_flac.avail_user = avail_user;
    f = ym_flac_open_common();
    if (!f) return NULL;
    ra_start(f, path);
    return f;
}

YmFlac *ym_flac_open_mp4(int *fd, const char *path,
                         YmFlacAvailFn avail, void *avail_user)
{
    YmFlac *f;
    int hdr_len;

    if (!fd || *fd < 0 || !path) return NULL;
    memset(&s_flac, 0, sizeof(s_flac));
    s_flac.ra_fd = -1;
    s_flac.ra_thread = -1;
    s_flac.is_file = 1;
    s_flac.sbuf = NULL;
    s_flac.fdp = fd;
    snprintf(s_flac.path, sizeof(s_flac.path), "%s", path);
    s_flac.pos = 0;
    s_flac.avail = avail;
    s_flac.avail_user = avail_user;
    s_flac.mode_mp4 = 1;
    s_flac.mt = mp4_track_open(path);
    if (!s_flac.mt) {
        logLine("ym_flac: mp4 open failed '%s'\n", path);
        return NULL;
    }
    if (!mp4_track_is_flac(s_flac.mt)) {
        logLine("ym_flac: mp4 not flac '%s'\n", path);
        mp4_track_close(s_flac.mt);
        s_flac.mt = NULL;
        return NULL;
    }
    hdr_len = mp4_track_flac_header(s_flac.mt, s_flac.hdr,
                                    (int)sizeof(s_flac.hdr));
    if (hdr_len <= 0) {
        logLine("ym_flac: mp4 header failed '%s'\n", path);
        mp4_track_close(s_flac.mt);
        s_flac.mt = NULL;
        return NULL;
    }
    s_flac.hdr_len = hdr_len;
    f = ym_flac_open_common();
    if (!f) {
        mp4_track_close(s_flac.mt);
        s_flac.mt = NULL;
        return NULL;
    }
    ra_start(f, path);
    logLine("ym_flac: mp4 mode '%s' header=%d samples=%d\n",
            path, hdr_len, mp4_track_sample_count(s_flac.mt));
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

/* Полный ресинк декодера: закрыть, сбросить пул, переоткрыть по тем же
 * колбэкам (позиция/курсор/mt/hdr в YmFlac не трогаются, f->pos=0 —
 * drflac_open читает заголовок с начала виртуального потока). */
static int ym_flac_reopen_dec(struct YmFlac *f)
{
    drflac *dec;
    if (f->dec) {
        drflac_close(f->dec);
        f->dec = NULL;
    }
    ym_flac_pool_reset();
    f->pos = 0;
    dec = drflac_open(ym_flac_on_read, ym_flac_on_seek, ym_flac_on_tell,
                      f, &s_alloc_cbs);
    if (!dec) {
        logLine("ym_flac: resync reopen failed target=%d\n",
                (int)f->resync_target);
        return -1;
    }
    f->dec = dec;
    return 0;
}

int64_t ym_flac_read_s16(YmFlac *f, short *pcm, int frames)
{
    drflac_uint64 got;
    if (!f || !f->dec || !pcm || frames <= 0) return -1;
    got = drflac_read_pcm_frames_s16(f->dec, (drflac_uint64)frames,
                                     (drflac_int16 *)pcm);
    if (got == 0) {
        if (!f->resync_active) {
            f->resync_target = f->dec->currentPCMFrame;
            f->resync_active = 1;
        }
        /* Дешёвый повтор: на границе докачки onRead мог вернуть 0 из-за
         * гонки счётчик/запись — повтор обычно проходит. */
        got = drflac_read_pcm_frames_s16(f->dec, (drflac_uint64)frames,
                                         (drflac_int16 *)pcm);
    }
    if (got == 0 && f->resync_active) {
        /* Повтор не помог: битовый ресивер дрейфовал после короткого
         * чтения (слепок «конец потока» не сбрасывается повтором) —
         * это давало вечные нули и ложный eof на фронтире докачки.
         * Полный ресинк: переоткрыть декодер и встать на сохранённый
         * кадр (drflac_seek_to_pcm_frame(current) — no-op, поэтому
         * именно переоткрытие). */
        if (ym_flac_reopen_dec(f) != 0) return -1;
        if (drflac_seek_to_pcm_frame(f->dec, f->resync_target)) {
            got = drflac_read_pcm_frames_s16(f->dec, (drflac_uint64)frames,
                                             (drflac_int16 *)pcm);
            logLine("ym_flac: resync ok target=%d got=%d\n",
                    (int)f->resync_target, (int)got);
        } else {
            logLine("ym_flac: resync seek wait target=%d avail=%d\n",
                    (int)f->resync_target, (int)ym_flac_avail(f));
        }
    }
    if (got > 0) f->resync_active = 0;
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
    /* f->dec может быть NULL после неудачного ресинка — закрываем mt и
     * сбрасываем флаг пула в любом случае, иначе s_flac_used навсегда
     * заблокирует следующие открытия. */
    if (!f) return;
    ra_stop_and_free(f);
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
