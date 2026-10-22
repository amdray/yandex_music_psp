#include "services/ym_aac.h"

#include <malloc.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <neaacdec.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/mp4_track.h"

#define YM_AAC_MAX_SAMPLE 16384   /* сырой AAC access unit */
#define YM_AAC_MAX_FRAMES 2048    /* кадр HE-AAC x2 от 1024 */

/* RAM read-ahead: окно 512К, фоновый поток читает с MS кусками 64К. */
#define YM_AAC_RA_SIZE  (512 * 1024)
#define YM_AAC_RA_CHUNK (64 * 1024)

struct YmAac {
    int *fdp; /* fd владельца (закроет сам); переоткрываем через него */
    char path[128]; /* тот же файл — для переоткрытия fd */
    Mp4Track *t;
    NeAACDecHandle dec;
    unsigned char asc[64];
    int asc_len;
    int core_rate;
    int next_sample;
    int total_samples;
    int err_count;
    YmAacAvailFn avail;
    void *avail_user;

    unsigned char sample_buf[YM_AAC_MAX_SAMPLE];
    short carry_pcm[YM_AAC_MAX_FRAMES * 2];
    int carry_len;
    int carry_pos;
    short mono_tmp[YM_AAC_MAX_FRAMES * 2];

    /* RAM read-ahead (см. ym_flac): декодер берёт сэмплы из RAM-окна,
     * MS дёргает только фоновый поток — прямое чтение с MS во время
     * записи докачки давало подвисания звука. */
    unsigned char *ra_buf;
    int64_t ra_base;
    int ra_have;
    int64_t ra_want;
    volatile int ra_stop;
    SceUID ra_thread;
    SceLwMutexWorkarea ra_mu;
    int ra_ok;
    int ra_fd;
};

/* Один декодер за раз: плеер декодирует единственный трек. */
static YmAac s_aac;
static int s_aac_busy = 0;

/* --- RAM read-ahead ------------------------------------------------- */

/* Попытка прочитать из RAM-окна. Возвращает len либо -1 (нет в окне). */
static int aac_ra_try_read(YmAac *a, int64_t abs, void *dst, int len)
{
    int got = -1;
    if (!a->ra_buf || !a->ra_ok || len <= 0) return -1;
    sceKernelLockLwMutex(&a->ra_mu, 1, NULL);
    a->ra_want = abs; /* следим за позицией читателя */
    if (abs >= a->ra_base && abs + len <= a->ra_base + a->ra_have) {
        memcpy(dst, a->ra_buf + (size_t)(abs - a->ra_base), (size_t)len);
        got = len;
    }
    sceKernelUnlockLwMutex(&a->ra_mu, 1);
    return got;
}

/* Фоновое наполнение окна. Живёт на отдельном fd (ra_fd) — конфликт
 * позиций с fd декодера исключён. Читает только в пределах скачанного
 * префикса (avail), чтобы окно не ушло вперед докачки. */
static int aac_ra_thread_main(SceSize args, void *argp)
{
    YmAac *a = &s_aac;
    (void)args;
    (void)argp;
    while (!a->ra_stop) {
        int64_t want, base, avail;
        int have, fill;
        if (!a->ra_buf || a->ra_fd < 0 || !a->ra_ok) break;
        sceKernelLockLwMutex(&a->ra_mu, 1, NULL);
        want = a->ra_want;
        base = a->ra_base;
        have = a->ra_have;
        if (want < base || base + have - want < (int64_t)(YM_AAC_RA_SIZE / 2)) {
            if (want >= base && want < base + have) {
                /* Скольжение окна: хвост за позицией читателя сохраняем —
                 * декодер не упирается в пустое окно. */
                int keep = (int)(base + have - want);
                if (keep > 0) {
                    memmove(a->ra_buf,
                            a->ra_buf + (size_t)(want - base),
                            (size_t)keep);
                }
                a->ra_base = want;
                a->ra_have = keep;
            } else {
                a->ra_base = want & ~(int64_t)4095;
                a->ra_have = 0;
            }
            base = a->ra_base;
            have = a->ra_have;
        }
        fill = have < YM_AAC_RA_SIZE;
        sceKernelUnlockLwMutex(&a->ra_mu, 1);
        if (!fill) {
            sceKernelDelayThread(20 * 1000);
            continue;
        }
        avail = a->avail ? a->avail(a->avail_user) : -1;
        if (avail < 0) {
            sceKernelDelayThread(30 * 1000);
            continue;
        }
        {
            int chunk = YM_AAC_RA_CHUNK;
            /* Clamp to free space in the 512K window: otherwise fs_read
             * overruns ra_buf by up to 64K (heap corruption). */
            if (chunk > YM_AAC_RA_SIZE - have) {
                chunk = YM_AAC_RA_SIZE - have;
            }
            if (base + have + chunk > avail) {
                chunk = (int)(avail - (base + have));
            }
            if (chunk <= 0) {
                sceKernelDelayThread(30 * 1000);
                continue;
            }
            if (fs_lseek(a->ra_fd, base + have, PSP_SEEK_SET) !=
                base + have) {
                sceKernelDelayThread(30 * 1000);
                continue;
            }
            {
                int got = fs_read(a->ra_fd, a->ra_buf + have,
                                  (size_t)chunk);
                if (got >= 0 && got < chunk && base + have + got < avail) {
                    /* avail говорит «данные дальше уже на диске», а fd
                     * короче — fd устарел (PSP не видит рост файла):
                     * берём новый и в следующей итерации читаем заново. */
                    int nfd = fs_open(a->path, PSP_O_RDONLY, 0777);
                    if (nfd >= 0) {
                        fs_close(a->ra_fd);
                        a->ra_fd = nfd;
                        logLine("ym_aac: read-ahead fd refreshed '%s'\n",
                                a->path);
                        continue;
                    }
                }
                if (got > 0) {
                    sceKernelLockLwMutex(&a->ra_mu, 1, NULL);
                    if (a->ra_base == base && a->ra_have == have) {
                        a->ra_have += got;
                    }
                    sceKernelUnlockLwMutex(&a->ra_mu, 1);
                } else {
                    /* 0 = файл короче (стрим): ждём докачки; <0 = MS. */
                    sceKernelDelayThread((got < 0 ? 50 : 30) * 1000);
                }
            }
        }
    }
    return sceKernelExitDeleteThread(0);
}

static void aac_ra_start(YmAac *a, const char *path)
{
    if (!path || a->ra_fd >= 0) return;
    a->ra_buf = (unsigned char *)memalign(64, YM_AAC_RA_SIZE);
    if (!a->ra_buf) {
        logLine("ym_aac: read-ahead alloc fail (%d bytes)\n", YM_AAC_RA_SIZE);
        return;
    }
    a->ra_fd = fs_open(path, PSP_O_RDONLY, 0777);
    if (a->ra_fd < 0) {
        logLine("ym_aac: read-ahead open fail '%s'\n", path);
        free(a->ra_buf);
        a->ra_buf = NULL;
        return;
    }
    if (sceKernelCreateLwMutex(&a->ra_mu, "ymaac_ra", 0, 0, NULL) < 0) {
        logLine("ym_aac: read-ahead mutex fail\n");
        fs_close(a->ra_fd);
        a->ra_fd = -1;
        free(a->ra_buf);
        a->ra_buf = NULL;
        return;
    }
    a->ra_ok = 1;
    a->ra_base = 0;
    a->ra_have = 0;
    a->ra_want = 0;
    a->ra_stop = 0;
    a->ra_thread = sceKernelCreateThread("aac_ra", aac_ra_thread_main,
                                         0x28, 0x4000, PSP_THREAD_ATTR_USER,
                                         NULL);
    if (a->ra_thread < 0) {
        logLine("ym_aac: read-ahead thread fail rc=%d\n", (int)a->ra_thread);
        a->ra_ok = 0;
        sceKernelDeleteLwMutex(&a->ra_mu);
        fs_close(a->ra_fd);
        a->ra_fd = -1;
        free(a->ra_buf);
        a->ra_buf = NULL;
        return;
    }
    sceKernelStartThread(a->ra_thread, 0, NULL);
    logLine("ym_aac: read-ahead on '%s' window=%dK\n",
            path, YM_AAC_RA_SIZE / 1024);
}

static void aac_ra_stop_and_free(YmAac *a)
{
    if (a->ra_thread >= 0) {
        a->ra_stop = 1;
        sceKernelWaitThreadEnd(a->ra_thread, NULL);
        sceKernelDeleteThread(a->ra_thread);
        a->ra_thread = -1;
    }
    if (a->ra_ok) {
        a->ra_ok = 0;
        sceKernelDeleteLwMutex(&a->ra_mu);
    }
    if (a->ra_fd >= 0) {
        fs_close(a->ra_fd);
        a->ra_fd = -1;
    }
    if (a->ra_buf) {
        free(a->ra_buf);
        a->ra_buf = NULL;
    }
}

static int aac_init_decoder(YmAac *a)
{
    NeAACDecConfigurationPtr cfg;
    unsigned long rate = 0;
    unsigned char ch = 0;

    if (a->dec) {
        NeAACDecClose(a->dec);
        a->dec = NULL;
    }
    a->dec = NeAACDecOpen();
    if (!a->dec) {
        logLine("ym_aac: NeAACDecOpen failed\n");
        return -1;
    }
    cfg = NeAACDecGetCurrentConfiguration(a->dec);
    if (cfg) {
        cfg->outputFormat = FAAD_FMT_16BIT;
        cfg->downMatrix = 1; /* >2ch -> стерео внутри декодера */
        NeAACDecSetConfiguration(a->dec, cfg);
    }
    if (NeAACDecInit2(a->dec, a->asc, (unsigned long)a->asc_len, &rate,
                      &ch) != 0) {
        logLine("ym_aac: Init2 failed asc_len=%d\n", a->asc_len);
        NeAACDecClose(a->dec);
        a->dec = NULL;
        return -1;
    }
    a->core_rate = (int)rate;
    return 0;
}

YmAac *ym_aac_open_file(int *fd, const char *path, YmAacAvailFn avail,
                        void *avail_user)
{
    YmAac *a = &s_aac;
    const unsigned char *asc;
    int asc_len = 0;

    if (!fd || *fd < 0 || !path) return NULL;
    if (s_aac_busy) {
        logLine("ym_aac: already open, refusing\n");
        return NULL;
    }
    memset(a, 0, sizeof(*a));
    a->ra_fd = -1;
    a->ra_thread = -1;
    a->fdp = fd;
    snprintf(a->path, sizeof(a->path), "%s", path);
    a->avail = avail;
    a->avail_user = avail_user;

    a->t = mp4_track_open(path);
    if (!a->t || !mp4_track_is_aac(a->t)) {
        logLine("ym_aac: not an aac MP4 '%s'\n", path);
        if (a->t) {
            mp4_track_close(a->t);
            a->t = NULL;
        }
        return NULL;
    }
    asc = mp4_track_asc(a->t, &asc_len);
    if (!asc || asc_len <= 0 || asc_len > (int)sizeof(a->asc)) {
        logLine("ym_aac: no ASC '%s' len=%d\n", path, asc_len);
        mp4_track_close(a->t);
        a->t = NULL;
        return NULL;
    }
    memcpy(a->asc, asc, (size_t)asc_len);
    a->asc_len = asc_len;
    a->total_samples = mp4_track_sample_count(a->t);
    if (a->total_samples <= 0 || aac_init_decoder(a) != 0) {
        if (a->dec) {
            NeAACDecClose(a->dec);
            a->dec = NULL;
        }
        mp4_track_close(a->t);
        a->t = NULL;
        return NULL;
    }
    s_aac_busy = 1;
    aac_ra_start(a, path);
    logLine("ym_aac: open '%s' samples=%d core_rate=%d asc_len=%d\n",
            path, a->total_samples, a->core_rate, a->asc_len);
    return a;
}

int ym_aac_format(YmAac *a, YmAacFormat *out)
{
    if (!a || !out || !a->t) return -1;
    out->sample_rate = 44100;
    out->channels = 2;
    out->total_frames = mp4_track_total_pcm(a->t, 44100);
    if (out->total_frames < 0) out->total_frames = 0;
    return 0;
}

/* Переоткрытие fd: PSP-драйвер снимает размер файла при open и не видит
 * его роста — читатель упирается в EOF момента open, хотя файл уже
 * докачан (avail говорит «байты есть»). Новый fd владелец закроет сам. */
static int aac_refresh_fd(YmAac *a)
{
    int old;
    int nfd;

    if (!a->fdp) return -1;
    old = *a->fdp;
    nfd = fs_open(a->path, PSP_O_RDONLY, 0777);
    if (nfd < 0) {
        logLine("ym_aac: fd refresh fail '%s' rc=%d\n", a->path, nfd);
        return -1;
    }
    fs_close(old);
    *a->fdp = nfd;
    logLine("ym_aac: fd refreshed '%s' %d -> %d\n", a->path, old, nfd);
    return 0;
}

/* Декодирует один MP4-сэмпл в carry.
 * 0 = обработан (carry может быть пустым при пропуске битого кадра),
 * 1 = данных ещё нет (стрим), -1 = фатально. */
static int aac_decode_one(YmAac *a)
{
    int64_t off = 0;
    int size = 0;
    NeAACDecFrameInfo fi;
    short *pcmdata;
    const short *src;
    int n, ch, rate, i;

    {
        int rc = mp4_track_sample_at(a->t, a->next_sample, &off, &size);
        if (rc == -2) {
            /* Таблица дальше загруженного префикса. Если avail говорит,
             * что байты уже есть — файл просто вырос с момента open;
             * иначе данных ещё нет -> ждать докачку (return 1). */
            int64_t av = a->avail ? a->avail(a->avail_user) : -1;
            if (av >= 0 && off + size > av) return 1;
            if (av < 0) {
                logLine("ym_aac: sample %d/%d offset unavailable\n",
                        a->next_sample, a->total_samples);
                return -1;
            }
        } else if (rc != 0) {
            logLine("ym_aac: sample %d/%d offset unavailable\n",
                    a->next_sample, a->total_samples);
            return -1;
        }
    }
    if (size <= 0 || size > (int)sizeof(a->sample_buf)) {
        logLine("ym_aac: bad sample size %d at %d\n", size, a->next_sample);
        return -1;
    }
    if (a->avail) {
        int64_t avail_bytes = a->avail(a->avail_user);
        if (avail_bytes >= 0 && off + size > avail_bytes) {
            return 1;
        }
    }
    if (aac_ra_try_read(a, off, a->sample_buf, size) < 0) {
        if (fs_lseek(*a->fdp, off, PSP_SEEK_SET) != off) {
            return -1;
        }
        {
            int got = fs_read(*a->fdp, a->sample_buf, (SceSize)size);
            if (got != size) {
                SceOff end = fs_lseek(*a->fdp, 0, PSP_SEEK_END);
                int64_t av = a->avail ? a->avail(a->avail_user) : -1;

                /* Счётчик говорит «байты на диске», а fd короче — fd устарел
                 * (не видит рост файла): переоткрываем и повторяем чтение. */
                if (got >= 0 && end >= 0 && off + size > end &&
                    av >= 0 && av >= off + size &&
                    aac_refresh_fd(a) == 0 &&
                    fs_lseek(*a->fdp, off, PSP_SEEK_SET) == off) {
                    got = fs_read(*a->fdp, a->sample_buf, (SceSize)size);
                }
                if (got != size) {
                    /* Граница докачки: счётчик downloaded_bytes обогнал запись
                     * в файл — это отсутствие данных, а не фатал. Возвращаем
                     * «ждать» (1): иначе трек уезжает в pb: error skip. */
                    end = fs_lseek(*a->fdp, 0, PSP_SEEK_END);
                    av = a->avail ? a->avail(a->avail_user) : -1;
                    logLine("ym_aac: short read sample=%d off=%d size=%d got=%d fd_end=%d avail=%d wait\n",
                            a->next_sample, (int)off, size, got, (int)end, (int)av);
                    return 1;
                }
            }
        }
    }
    a->next_sample++;

    memset(&fi, 0, sizeof(fi));
    pcmdata = (short *)NeAACDecDecode(a->dec, &fi, a->sample_buf,
                                      (unsigned long)size);
    if (fi.error != 0 || !pcmdata || fi.samples <= 0) {
        if (++a->err_count > 32) {
            logLine("ym_aac: too many frame errors err=%d sample=%d\n",
                    (int)fi.error, a->next_sample - 1);
            return -1;
        }
        logLine("ym_aac: frame err=%d sample=%d/%d skip\n",
                (int)fi.error, a->next_sample - 1, a->total_samples);
        return 0;
    }
    a->err_count = 0;

    ch = fi.channels > 0 ? (int)fi.channels : 2;
    rate = fi.samplerate > 0 ? (int)fi.samplerate : a->core_rate;
    if (rate > 0 && rate != a->core_rate) {
        /* SBR подключился после первого кадра (he-aac). */
        a->core_rate = rate;
        logLine("ym_aac: decoder rate -> %d sample=%d\n",
                rate, a->next_sample - 1);
    }
    n = (int)(fi.samples / (unsigned long)ch);
    if (n <= 0) return 0;
    if (n > YM_AAC_MAX_FRAMES) {
        logLine("ym_aac: frame too long n=%d\n", n);
        return -1;
    }

    src = pcmdata;
    if (ch == 1) {
        for (i = 0; i < n; i++) {
            a->mono_tmp[i * 2] = pcmdata[i];
            a->mono_tmp[i * 2 + 1] = pcmdata[i];
        }
        src = a->mono_tmp;
    } else if (ch > 2) {
        /* defDownMatrix не сработал — берём два первых канала. */
        for (i = 0; i < n; i++) {
            a->mono_tmp[i * 2] = pcmdata[i * ch];
            a->mono_tmp[i * 2 + 1] = pcmdata[i * ch + 1];
        }
        src = a->mono_tmp;
    }

    if (rate == 44100) {
        memcpy(a->carry_pcm, src, (size_t)n * 4);
        a->carry_len = n;
    } else {
        /* Линейный ресемпл -> 44100 (тот же приём, что в старом aacdec.c):
         * ветка редкая — faad обычно даёт 44100 сразу (SBR). */
        uint64_t total_out = ((uint64_t)n * 44100ULL) / (uint64_t)rate;
        if (total_out > YM_AAC_MAX_FRAMES) total_out = YM_AAC_MAX_FRAMES;
        for (i = 0; i < (int)total_out; i++) {
            uint64_t p = ((uint64_t)i * (uint64_t)rate) / 44100ULL;
            uint64_t frac = ((uint64_t)i * (uint64_t)rate) % 44100ULL;
            int idx = (int)p;
            short l0, r0, l1, r1;
            if (idx >= n) idx = n - 1;
            l0 = src[idx * 2];
            r0 = src[idx * 2 + 1];
            if (idx + 1 < n) {
                l1 = src[idx * 2 + 2];
                r1 = src[idx * 2 + 3];
            } else {
                l1 = l0;
                r1 = r0;
            }
            a->carry_pcm[i * 2] =
                (short)(l0 + (int)(((int64_t)(l1 - l0) * (int64_t)frac) / 44100));
            a->carry_pcm[i * 2 + 1] =
                (short)(r0 + (int)(((int64_t)(r1 - r0) * (int64_t)frac) / 44100));
        }
        a->carry_len = (int)total_out;
    }
    a->carry_pos = 0;
    return 0;
}

int64_t ym_aac_read_s16(YmAac *a, short *pcm, int frames)
{
    int out = 0;

    if (!a || !pcm || frames <= 0 || !a->dec) return -1;
    while (out < frames) {
        if (a->carry_pos < a->carry_len) {
            int n = a->carry_len - a->carry_pos;
            if (n > frames - out) n = frames - out;
            memcpy(pcm + (size_t)out * 2,
                   a->carry_pcm + (size_t)a->carry_pos * 2,
                   (size_t)n * 4);
            a->carry_pos += n;
            out += n;
            if (a->carry_pos >= a->carry_len) {
                a->carry_pos = 0;
                a->carry_len = 0;
            }
            continue;
        }
        if (a->next_sample >= a->total_samples) break; /* EOF */
        {
            int rc = aac_decode_one(a);
            if (rc < 0) return -1;
            if (rc > 0) break; /* данных ещё нет (стрим) */
        }
    }
    return out;
}

int ym_aac_seek_frame(YmAac *a, int64_t frame)
{
    int idx;
    int64_t off = 0;
    int size = 0;

    if (!a || !a->t || frame < 0) return -2;
    idx = mp4_track_sample_for_pcm(a->t, frame, 44100);
    if (idx < 0) return -2;
    if (idx >= a->total_samples) idx = a->total_samples - 1;
    if (idx < 0) return -2;
    {
        int rc = mp4_track_sample_at(a->t, idx, &off, &size);
        if (rc == -2) {
            int64_t av = a->avail ? a->avail(a->avail_user) : -1;
            if (av >= 0 && off + size > av) return -1; /* ждать докачку */
            if (av < 0) return -2;
        } else if (rc != 0) {
            return -2;
        }
    }
    if (a->avail) {
        int64_t avail_bytes = a->avail(a->avail_user);
        if (avail_bytes >= 0 && off + size > avail_bytes) {
            return -1;
        }
    }
    /* SBR/PS-состояние не переживает seek — полный пере-init по ASC. */
    if (aac_init_decoder(a) != 0) return -2;
    a->next_sample = idx;
    a->carry_len = 0;
    a->carry_pos = 0;
    a->err_count = 0;
    return 0;
}

int ym_aac_at_end(YmAac *a)
{
    if (!a) return 1;
    return a->next_sample >= a->total_samples;
}

void ym_aac_close(YmAac *a)
{
    if (!a || a != &s_aac || !s_aac_busy) return;
    aac_ra_stop_and_free(a);
    if (a->dec) NeAACDecClose(a->dec);
    if (a->t) mp4_track_close(a->t);
    memset(a, 0, sizeof(*a));
    s_aac_busy = 0;
}
