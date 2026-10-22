/* Разбор MP4 (ISOBMFF) аудио-трека. Один модуль на три потребителя:
 * проба потока перед закачкой, демукс flac-mp4 -> сырой fLaC, путь
 * плеера для aac-mp4 (esds ASC + сэмплы). moov целиком в RAM. */
#include "services/mp4_track.h"

#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"

#define MP4_MAX_MOOV (8 * 1024 * 1024)
#define MP4_MAX_SAMPLES 2000000u
#define MP4_MAX_CHUNKS 200000u
#define MP4_MAX_ENTRIES 500000u
#define MP4_FLAC_HEADER_CAP 512
#define MP4_ASC_CAP 64

struct Mp4Track {
    int kind; /* Mp4ProbeKind */

    /* FLAC */
    unsigned char flac_header[MP4_FLAC_HEADER_CAP];
    int flac_header_len;
    int rate, ch, bps;

    /* AAC */
    unsigned char asc[MP4_ASC_CAP];
    int asc_len;

    /* Таблицы */
    uint32_t timescale;
    uint32_t sample_count;
    uint32_t fixed_sample_size; /* 0 -> sizes[] */
    uint32_t *sizes;
    uint32_t chunk_count;
    uint64_t *chunk_off;
    uint32_t stsc_n;
    uint32_t *stsc_first; /* 1-based */
    uint32_t *stsc_sps;
    uint32_t stts_n;
    uint32_t *stts_cnt;
    uint32_t *stts_delta;
    int64_t file_size;

    /* Курсор последовательного чтения (только внутри одного чанка:
     * между чанками файл может содержать зазоры, стco монотонен,
     * но не обязан быть непрерывным). */
    int cursor_valid;
    int next_idx;
    int64_t cur_off;
    uint32_t cursor_chunk_end; /* первый индекс сэмпла следующего чанка */
};

/* ---- little helpers ---- */

static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const unsigned char *p)
{
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4);
}

typedef struct {
    const unsigned char *p; /* payload */
    size_t declared;        /* размер payload по заголовку (может > avail) */
    size_t avail;           /* реально доступно байт в буфере */
} BoxPayload;

typedef struct {
    BoxPayload c;
    size_t off;
} BoxIter;

static void box_iter_init(BoxIter *it, BoxPayload c)
{
    it->c = c;
    it->off = 0;
}

/* 1 = следующий дочерний бокс, 0 = конец/обрыв, -1 = повреждение. */
static int box_iter_next(BoxIter *it, unsigned char type_out[5],
                         BoxPayload *child)
{
    while (it->off + 8 <= it->c.declared) {
        size_t o = it->off;
        const unsigned char *h;
        uint64_t size;
        size_t hdr = 8;
        size_t payload_off;
        size_t declared;
        size_t avail;

        if (o + 8 > it->c.avail) return 0; /* буфер кончился */
        h = it->c.p + o;
        size = rd32(h);
        memcpy(type_out, h + 4, 4);
        type_out[4] = '\0';
        if (size == 1) {
            if (o + 16 > it->c.avail) return 0;
            size = rd64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = (uint64_t)(it->c.declared - o);
        }
        if (size < (uint64_t)hdr || o + (size_t)size > it->c.declared) {
            return -1;
        }
        payload_off = o + hdr;
        declared = (size_t)size - hdr;
        avail = payload_off <= it->c.avail ? it->c.avail - payload_off : 0;
        it->off = o + (size_t)size;
        child->p = it->c.p + payload_off;
        child->declared = declared;
        child->avail = avail;
        return 1;
    }
    return 0;
}

static int find_child(BoxPayload c, const char *type, BoxPayload *out)
{
    BoxIter it;
    unsigned char t[5];
    BoxPayload child;
    int rc;
    box_iter_init(&it, c);
    while ((rc = box_iter_next(&it, t, &child)) > 0) {
        if (memcmp(t, type, 4) == 0) {
            *out = child;
            return 0;
        }
    }
    (void)rc;
    return -1;
}

/* ---- STREAMINFO / dfLa ---- */

static int flac_parse_streaminfo(const unsigned char *si /* 34 байта */,
                                 int *rate, int *ch, int *bps)
{
    *rate = ((int)si[10] << 12) | ((int)si[11] << 4) | ((int)si[12] >> 4);
    *ch = (((int)si[12] >> 1) & 7) + 1;
    *bps = ((((int)si[12] & 1) << 4) | ((int)si[13] >> 4)) + 1;
    if (*rate <= 0 || *ch <= 0 || *bps <= 0) return -1;
    return 0;
}

/* dfLa -> сырой заголовок fLaC: "fLaC" + только STREAMINFO-блок
 * (прочие опциональные метаблоки пропускаем, last-bit выставляем сами:
 * их отсутствие для dr_flac не критично, а объём держит заголовок
 * фиксированным в 42 байта). */
static int parse_dfla(const BoxPayload *d, Mp4Track *t)
{
    const unsigned char *p;
    size_t len;
    unsigned b0, mlen;

    if (d->declared < 4) return -1;
    p = d->p;
    len = d->avail < d->declared ? d->avail : d->declared;
    if (len < 4 + 4 + 34) return -1;
    /* Full payload: version(1) flags(3) + метаблоки */
    b0 = p[4];
    mlen = ((unsigned)p[5] << 16) | ((unsigned)p[6] << 8) | (unsigned)p[7];
    if ((b0 & 0x7F) != 0 || mlen != 34) return -1;
    if (4 + 4 + 34 > len) return -1;
    t->flac_header[0] = 'f';
    t->flac_header[1] = 'L';
    t->flac_header[2] = 'a';
    t->flac_header[3] = 'C';
    memcpy(t->flac_header + 4, p + 4, 4 + 34);
    t->flac_header[4] = (unsigned char)(b0 | 0x80); /* этот блок последний */
    t->flac_header_len = 4 + 4 + 34;
    if (flac_parse_streaminfo(t->flac_header + 8, &t->rate, &t->ch,
                              &t->bps) != 0) {
        return -1;
    }
    return 0;
}

/* ---- esds -> AudioSpecificConfig ---- */

/* varint-длина дескриптора: 7 бит на байт, старший = продолжение. */
static int desc_len(const unsigned char *p, size_t len, size_t *out_len,
                    size_t *out_hdr)
{
    size_t v = 0;
    size_t i;
    for (i = 0; i < 4; i++) {
        unsigned char b;
        if (1 + i >= len) return -1;
        b = p[1 + i];
        v = (v << 7) | (size_t)(b & 0x7F);
        if (!(b & 0x80)) {
            *out_len = v;
            *out_hdr = 2 + i;
            return 0;
        }
    }
    return -1;
}

static int parse_esds(const BoxPayload *e, Mp4Track *t)
{
    const unsigned char *p = e->p;
    size_t len = e->avail < e->declared ? e->avail : e->declared;
    size_t off = 0;
    size_t dlen, dhdr;

    if (len < 5) return -1;
    off = 4; /* FullBox version/flags */
    if (1 + 4 > len) return -1;
    /* Ожидаем ES_Descriptor (0x03), внутри DecoderConfig (0x04),
     * внутри DecSpecificInfo (0x05) с ASC. */
    if (p[off] != 0x03) return -1;
    if (desc_len(p + off, len - off, &dlen, &dhdr) != 0) return -1;
    {
        size_t es = off + dhdr;
        size_t ee = es + dlen;
        size_t pos;
        unsigned flags;
        if (ee > len) return -1;
        if (es + 3 > ee) return -1;
        flags = p[es + 2];
        pos = es + 3;
        if (flags & 0x80) pos += 2; /* DependsOn */
        if (flags & 0x40) {         /* URL */
            if (pos >= ee) return -1;
            pos += 1 + (size_t)p[pos];
        }
        if (flags & 0x20) pos += 2; /* OCR */
        if (pos > ee) return -1;

        if (pos + 1 > ee || p[pos] != 0x04) return -1;
        if (desc_len(p + pos, ee - pos, &dlen, &dhdr) != 0) return -1;
        {
            size_t dc = pos + dhdr;
            size_t dce = dc + dlen;
            if (dce > ee || dlen < 13) return -1;
            /* objectTypeIndication, streamType, bufferSizeDB, max/avg
             * bitrate — дал не читаем; внутри ищем 0x05. */
            pos = dc + 13;
            if (pos > dce) return -1;
            if (pos + 1 > dce || p[pos] != 0x05) return -1;
            if (desc_len(p + pos, dce - pos, &dlen, &dhdr) != 0) return -1;
            if (pos + dhdr + dlen > dce) return -1;
            if (dlen == 0 || dlen > MP4_ASC_CAP) return -1;
            memcpy(t->asc, p + pos + dhdr, dlen);
            t->asc_len = (int)dlen;
            return 0;
        }
    }
}

/* ---- stsd: выбор аудио-трека ---- */

static int parse_stsd(const BoxPayload *stsd, Mp4Track *t)
{
    const unsigned char *p = stsd->p;
    size_t len = stsd->avail < stsd->declared ? stsd->avail : stsd->declared;
    size_t off;
    uint32_t count;
    uint32_t i;

    if (len < 8) return -1;
    count = rd32(p + 4);
    off = 8;
    for (i = 0; i < count; i++) {
        uint32_t size;
        const unsigned char *body;
        uint32_t body_len;
        BoxPayload entry_children;
        unsigned char ctype[5];

        if (off + 8 > len) return -1;
        size = rd32(p + off);
        memcpy(ctype, p + off + 4, 4);
        ctype[4] = '\0';
        if (size < 8 || off + size > len) return -1;
        body = p + off + 8;
        body_len = size - 8;

        if ((memcmp(ctype, "fLaC", 4) == 0 ||
             memcmp(ctype, "mp4a", 4) == 0) &&
            body_len >= 36) {
            /* SampleEntry(8) + AudioSampleEntry v0(20) = 28 -> дети. */
            entry_children.p = body + 28;
            entry_children.declared = body_len - 28;
            entry_children.avail = body_len - 28;
            if (memcmp(ctype, "fLaC", 4) == 0) {
                BoxPayload dfla;
                if (find_child(entry_children, "dfLa", &dfla) == 0 &&
                    parse_dfla(&dfla, t) == 0) {
                    t->kind = MP4_PROBE_FLAC;
                    return 0;
                }
            } else {
                BoxPayload esds;
                if (find_child(entry_children, "esds", &esds) == 0 &&
                    parse_esds(&esds, t) == 0) {
                    t->kind = MP4_PROBE_AAC;
                    return 0;
                }
            }
        }
        off += size;
    }
    return -1;
}

/* ---- таблицы сэмплов (stts/stsc/stsz/stco) ---- */

static int parse_tables(const BoxPayload *stbl, Mp4Track *t)
{
    BoxPayload b;

    if (find_child(*stbl, "stsz", &b) != 0) return -1;
    {
        size_t len = b.avail < b.declared ? b.avail : b.declared;
        uint32_t count;
        if (len < 12) return -1;
        t->fixed_sample_size = rd32(b.p + 4);
        count = rd32(b.p + 8);
        if (count == 0 || count > MP4_MAX_SAMPLES) return -1;
        if (t->fixed_sample_size == 0) {
            if (len < 12 + (size_t)count * 4) return -1;
            t->sizes = (uint32_t *)malloc((size_t)count * 4);
            if (!t->sizes) return -1;
            {
                uint32_t k;
                for (k = 0; k < count; k++) {
                    t->sizes[k] = rd32(b.p + 12 + (size_t)k * 4);
                }
            }
        }
        t->sample_count = count;
    }

    if (find_child(*stbl, "stsc", &b) != 0) return -1;
    {
        size_t len = b.avail < b.declared ? b.avail : b.declared;
        uint32_t count;
        uint32_t k;
        if (len < 8) return -1;
        count = rd32(b.p + 4);
        if (count == 0 || count > MP4_MAX_ENTRIES ||
            len < 8 + (size_t)count * 12) {
            return -1;
        }
        t->stsc_first = (uint32_t *)malloc((size_t)count * 4);
        t->stsc_sps = (uint32_t *)malloc((size_t)count * 4);
        if (!t->stsc_first || !t->stsc_sps) return -1;
        for (k = 0; k < count; k++) {
            t->stsc_first[k] = rd32(b.p + 8 + (size_t)k * 12);
            t->stsc_sps[k] = rd32(b.p + 8 + (size_t)k * 12 + 4);
        }
        if (t->stsc_first[0] != 1) return -1;
        t->stsc_n = count;
    }

    {
        int found64 = 0;
        size_t len;
        uint32_t count;
        uint32_t k;
        if (find_child(*stbl, "co64", &b) == 0) {
            found64 = 1;
        } else if (find_child(*stbl, "stco", &b) != 0) {
            return -1;
        }
        len = b.avail < b.declared ? b.avail : b.declared;
        if (len < 8) return -1;
        count = rd32(b.p + 4);
        if (count == 0 || count > MP4_MAX_CHUNKS) return -1;
        t->chunk_off = (uint64_t *)malloc((size_t)count * 8);
        if (!t->chunk_off) return -1;
        if (found64) {
            if (len < 8 + (size_t)count * 8) return -1;
            for (k = 0; k < count; k++) {
                t->chunk_off[k] = rd64(b.p + 8 + (size_t)k * 8);
            }
        } else {
            if (len < 8 + (size_t)count * 4) return -1;
            for (k = 0; k < count; k++) {
                t->chunk_off[k] = (uint64_t)rd32(b.p + 8 + (size_t)k * 4);
            }
        }
        t->chunk_count = count;
    }

    if (find_child(*stbl, "stts", &b) == 0) {
        size_t len = b.avail < b.declared ? b.avail : b.declared;
        uint32_t count;
        uint32_t k;
        if (len >= 8) {
            count = rd32(b.p + 4);
            if (count > 0 && count <= MP4_MAX_ENTRIES &&
                len >= 8 + (size_t)count * 8) {
                t->stts_cnt = (uint32_t *)malloc((size_t)count * 4);
                t->stts_delta = (uint32_t *)malloc((size_t)count * 4);
                if (t->stts_cnt && t->stts_delta) {
                    for (k = 0; k < count; k++) {
                        t->stts_cnt[k] = rd32(b.p + 8 + (size_t)k * 8);
                        t->stts_delta[k] = rd32(b.p + 8 + (size_t)k * 8 + 4);
                    }
                    t->stts_n = count;
                }
            }
        }
    }
    /* stts опционален для проигрывания (seek без него невозможен). */
    return 0;
}

/* ---- общий разбор moov ---- */

/* moov может обрываться буфером пробы (avail < declared) — тогда таблицы
 * не читаем, валидируем только путь dfLa/esds. */
static int parse_moov(BoxPayload moov, Mp4Track *t, int want_tables)
{
    BoxIter it;
    unsigned char tt[5];
    BoxPayload trak;
    int rc;

    box_iter_init(&it, moov);
    while ((rc = box_iter_next(&it, tt, &trak)) > 0) {
        BoxPayload mdia, minf, stbl, stsd;

        if (memcmp(tt, "trak", 4) != 0) continue;
        if (find_child(trak, "mdia", &mdia) != 0) continue;

        /* mdhd -> timescale (для маппинга pcm <-> сэмпл). */
        {
            BoxPayload mdhd;
            if (find_child(mdia, "mdhd", &mdhd) == 0) {
                size_t len = mdhd.avail < mdhd.declared ? mdhd.avail
                                                        : mdhd.declared;
                if (len >= 16 && mdhd.p[0] == 0) {
                    t->timescale = rd32(mdhd.p + 12);
                } else if (len >= 24 && mdhd.p[0] == 1) {
                    t->timescale = rd32(mdhd.p + 20);
                }
            }
        }

        if (find_child(mdia, "minf", &minf) != 0) continue;
        if (find_child(minf, "stbl", &stbl) != 0) continue;
        if (find_child(stbl, "stsd", &stsd) != 0) continue;

        {
            int saved_kind = t->kind;
            t->kind = MP4_PROBE_NONE;
            if (parse_stsd(&stsd, t) != 0) {
                t->kind = saved_kind;
                continue;
            }
        }
        if (want_tables && parse_tables(&stbl, t) != 0) {
            return -1;
        }
        return 0;
    }
    (void)rc;
    return -1;
}

/* ---- проба из буфера ---- */

int mp4_probe_buffer(const unsigned char *buf, int len, Mp4ProbeInfo *out)
{
    size_t off = 0;
    Mp4Track t;

    if (!buf || len < 16 || !out) return -1;
    memset(out, 0, sizeof(*out));
    memset(&t, 0, sizeof(t));

    /* Верхнеуровневый поиск moov: в пробе moov может выйти за буфер. */
    while (off + 8 <= (size_t)len) {
        const unsigned char *h = buf + off;
        uint64_t size = rd32(h);
        char type[5];
        memcpy(type, h + 4, 4);
        type[4] = '\0';
        if (size == 1) {
            if (off + 16 > (size_t)len) return -1;
            size = rd64(h + 8);
            if (size < 16) return -1;
        } else if (size == 0) {
            size = (uint64_t)len - off;
        } else if (size < 8) {
            return -1;
        }
        if (memcmp(type, "moov", 4) == 0) {
            BoxPayload moov;
            moov.p = h + (size == 1 ? 16 : 8);
            moov.declared = (size_t)size - (size == 1 ? 16 : 8);
            moov.avail = (size_t)len - (off + (size == 1 ? 16 : 8));
            if (moov.avail > moov.declared) moov.avail = moov.declared;
            if (parse_moov(moov, &t, 0) != 0) return -1;
            out->kind = (Mp4ProbeKind)t.kind;
            out->rate = t.rate;
            out->ch = t.ch;
            out->bps = t.bps;
            return t.kind != MP4_PROBE_NONE ? 0 : -1;
        }
        /* Следующий верхнеуровневый бокс (ftyp/free/...): за буфер не
         * выходим — moov в faststart лежит в начале. */
        if (off + (size_t)size > (size_t)len) return -1;
        off += (size_t)size;
    }
    return -1;
}

/* ---- файловый доступ ---- */

static int read_full(SceUID fd, void *dst, int want)
{
    unsigned char *p = (unsigned char *)dst;
    int got_total = 0;
    while (got_total < want) {
        int rc = fs_read(fd, p + got_total, want - got_total);
        if (rc <= 0) break;
        got_total += rc;
    }
    return got_total;
}

static int find_moov(SceUID fd, int64_t file_size, int64_t *out_off,
                     uint64_t *out_size, size_t *out_hdr)
{
    int64_t off = 0;
    while (off + 8 <= file_size) {
        unsigned char h[16];
        uint64_t size;
        size_t hdr = 8;
        char type[5];

        if (fs_lseek(fd, off, PSP_SEEK_SET) != off) return -1;
        if (read_full(fd, h, 8) != 8) return -1;
        size = rd32(h);
        memcpy(type, h + 4, 4);
        type[4] = '\0';
        if (size == 1) {
            if (read_full(fd, h + 8, 8) != 8) return -1;
            size = rd64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = (uint64_t)(file_size - off);
        }
        if (size < (uint64_t)hdr || off + (int64_t)size > file_size) {
            return -1;
        }
        if (memcmp(type, "moov", 4) == 0) {
            *out_off = off + (int64_t)hdr;
            *out_size = size - hdr;
            *out_hdr = hdr;
            return 0;
        }
        off += (int64_t)size;
    }
    return -1;
}

Mp4Track *mp4_track_open(const char *path)
{
    SceUID fd;
    int64_t file_size;
    int64_t moov_off = 0;
    uint64_t moov_size = 0;
    size_t moov_hdr = 8;
    unsigned char *buf = NULL;
    Mp4Track *t = NULL;

    if (!path) return NULL;
    fd = fs_open(path, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        logLine("mp4: open failed '%s' rc=0x%08X\n", path, fd);
        return NULL;
    }
    file_size = fs_lseek(fd, 0, PSP_SEEK_END);
    if (file_size <= 16 ||
        find_moov(fd, file_size, &moov_off, &moov_size, &moov_hdr) != 0) {
        logLine("mp4: no moov '%s' size=%d\n", path, (int)file_size);
        fs_close(fd);
        return NULL;
    }
    if (moov_size == 0 || moov_size > MP4_MAX_MOOV) {
        logLine("mp4: bad moov size=%u\n", (unsigned)moov_size);
        fs_close(fd);
        return NULL;
    }

    buf = (unsigned char *)malloc((size_t)moov_size);
    if (!buf) {
        fs_close(fd);
        return NULL;
    }
    if (fs_lseek(fd, moov_off, PSP_SEEK_SET) != moov_off ||
        read_full(fd, buf, (int)moov_size) != (int)moov_size) {
        logLine("mp4: moov read failed '%s'\n", path);
        free(buf);
        fs_close(fd);
        return NULL;
    }
    fs_close(fd);

    t = (Mp4Track *)calloc(1, sizeof(Mp4Track));
    if (!t) {
        free(buf);
        return NULL;
    }
    t->file_size = file_size;
    {
        BoxPayload moov;
        moov.p = buf;
        moov.declared = (size_t)moov_size;
        moov.avail = (size_t)moov_size;
        if (parse_moov(moov, t, 1) != 0 || t->kind == MP4_PROBE_NONE) {
            logLine("mp4: no audio track '%s'\n", path);
            mp4_track_close(t);
            free(buf);
            return NULL;
        }
    }
    free(buf);
    logLine("mp4: open ok '%s' kind=%d samples=%u chunks=%u ts=%u "
            "flac=%d/%d/%d asc=%d\n",
            path, t->kind, t->sample_count, t->chunk_count, t->timescale,
            t->rate, t->ch, t->bps, t->asc_len);
    return t;
}

void mp4_track_close(Mp4Track *t)
{
    if (!t) return;
    free(t->sizes);
    free(t->chunk_off);
    free(t->stsc_first);
    free(t->stsc_sps);
    free(t->stts_cnt);
    free(t->stts_delta);
    free(t);
}

int mp4_track_is_flac(const Mp4Track *t)
{
    return t && t->kind == MP4_PROBE_FLAC;
}

int mp4_track_is_aac(const Mp4Track *t)
{
    return t && t->kind == MP4_PROBE_AAC;
}

int mp4_track_flac_header(const Mp4Track *t, unsigned char *out, int cap)
{
    if (!t || t->kind != MP4_PROBE_FLAC || !out ||
        cap < t->flac_header_len) {
        return -1;
    }
    memcpy(out, t->flac_header, (size_t)t->flac_header_len);
    return t->flac_header_len;
}

int mp4_track_flac_format(const Mp4Track *t, int *rate, int *ch, int *bps)
{
    if (!t || t->kind != MP4_PROBE_FLAC) return -1;
    if (rate) *rate = t->rate;
    if (ch) *ch = t->ch;
    if (bps) *bps = t->bps;
    return 0;
}

const unsigned char *mp4_track_asc(const Mp4Track *t, int *len)
{
    if (!t || t->kind != MP4_PROBE_AAC || t->asc_len <= 0) return NULL;
    if (len) *len = t->asc_len;
    return t->asc;
}

int mp4_track_sample_count(const Mp4Track *t)
{
    return t ? (int)t->sample_count : 0;
}

static uint32_t sample_size_at(const Mp4Track *t, uint32_t idx)
{
    if (t->fixed_sample_size) return t->fixed_sample_size;
    if (idx < t->sample_count) return t->sizes[idx];
    return 0;
}

static uint32_t sps_for_chunk(const Mp4Track *t, uint32_t chunk0)
{
    uint32_t i;
    uint32_t n = t->stsc_sps[0];
    uint32_t chunk1 = chunk0 + 1;
    for (i = 0; i < t->stsc_n; i++) {
        if (t->stsc_first[i] <= chunk1) {
            n = t->stsc_sps[i];
        } else {
            break;
        }
    }
    return n;
}

int mp4_track_sample_at(Mp4Track *t, int idx, int64_t *out_off, int *out_size)
{
    uint32_t left;
    uint32_t c;
    int64_t o;
    uint32_t sz;

    if (!t || !out_off || !out_size || idx < 0 ||
        (uint32_t)idx >= t->sample_count) {
        return -1;
    }
    sz = sample_size_at(t, (uint32_t)idx);
    if (sz == 0) return -1;

    if (t->cursor_valid && idx == t->next_idx &&
        (uint32_t)idx < t->cursor_chunk_end) {
        *out_off = t->cur_off;
        *out_size = (int)sz;
        t->cur_off += sz;
        t->next_idx = idx + 1;
        return 0;
    }

    left = (uint32_t)idx;
    for (c = 0; c < t->chunk_count; c++) {
        uint32_t n = sps_for_chunk(t, c);
        if (left < n) {
            uint32_t i;
            o = (int64_t)t->chunk_off[c];
            for (i = 0; i < left; i++) {
                uint32_t s = sample_size_at(t, (uint32_t)idx - left + i);
                o += s;
            }
            if (o < 0) return -1;
            *out_off = o;
            *out_size = (int)sz;
            t->cursor_valid = 1;
            t->next_idx = idx + 1;
            t->cur_off = o + sz;
            t->cursor_chunk_end = (uint32_t)idx - left + n;
            if (o + sz > t->file_size) {
                /* -2: сэмпл за пределами загруженного префикса (прогрес-
                 * сивная докачка, file_size заморожен на момент open).
                 * off/size уже заполнены — вызывающий сверяет их со своим
                 * avail и либо читает (файл вырос), либо ждёт докачку. */
                return -2;
            }
            return 0;
        }
        left -= n;
    }
    return -1;
}

/* ticks сэмпла idx в единицах timescale (stts кумулятивно). */
static int64_t ticks_at_sample(const Mp4Track *t, int idx)
{
    int64_t cum = 0;
    int64_t seen = 0;
    uint32_t i;
    if (!t->stts_n || t->timescale == 0) return -1;
    for (i = 0; i < t->stts_n; i++) {
        int64_t next = seen + (int64_t)t->stts_cnt[i];
        if (idx < next) {
            return cum + ((int64_t)idx - seen) * (int64_t)t->stts_delta[i];
        }
        cum += (int64_t)t->stts_cnt[i] * (int64_t)t->stts_delta[i];
        seen = next;
    }
    return -1;
}

int mp4_track_sample_for_pcm(const Mp4Track *t, int64_t pcm, int rate)
{
    int64_t ticks;
    int64_t cum = 0;
    int64_t seen = 0;
    uint32_t i;

    if (!t || rate <= 0 || t->timescale == 0 || pcm < 0) return -1;
    ticks = (pcm * (int64_t)t->timescale) / rate;
    if (!t->stts_n) return -1;
    for (i = 0; i < t->stts_n; i++) {
        int64_t span = (int64_t)t->stts_cnt[i] * (int64_t)t->stts_delta[i];
        if (ticks < cum + span) {
            int64_t local = ticks - cum;
            int64_t idx = seen + (t->stts_delta[i] ? local / t->stts_delta[i]
                                                   : 0);
            if (idx < 0) idx = 0;
            if (idx >= (int64_t)t->sample_count) {
                idx = (int64_t)t->sample_count - 1;
            }
            return (int)idx;
        }
        cum += span;
        seen += (int64_t)t->stts_cnt[i];
    }
    return (int)t->sample_count - 1;
}

int64_t mp4_track_pcm_for_sample(const Mp4Track *t, int idx, int rate)
{
    int64_t ticks;
    if (!t || rate <= 0 || t->timescale == 0 || idx < 0) return -1;
    ticks = ticks_at_sample(t, idx);
    if (ticks < 0) return -1;
    return (ticks * rate) / (int64_t)t->timescale;
}

int64_t mp4_track_total_pcm(const Mp4Track *t, int rate)
{
    int64_t ticks = 0;
    uint32_t i;
    if (!t || rate <= 0 || t->timescale == 0 || !t->stts_n) return -1;
    for (i = 0; i < t->stts_n; i++) {
        ticks += (int64_t)t->stts_cnt[i] * (int64_t)t->stts_delta[i];
    }
    return (ticks * rate) / (int64_t)t->timescale;
}
