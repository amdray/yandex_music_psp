#include "services/ym_aac.h"

#include <pspkernel.h>
#include <string.h>
#include <stdint.h>
#include <pspaudiocodec.h>

#include "core/logger.h"
#include "services/mp4_track.h"

#define YM_AAC_MAX_SAMPLE 0x600 /* raw AAC: размер чтения sceAudiocodec */
#define YM_AAC_PCM_BYTES (YM_AAC_FRAME_SAMPLES * 2 * sizeof(short))

struct YmAac {
    AudioReader *reader;
    Mp4Track *t;
    unsigned long codec[65] __attribute__((aligned(64)));
    int edram, decoder_ready, rate, next_sample, total_samples;
    unsigned char sample_buf[YM_AAC_MAX_SAMPLE] __attribute__((aligned(64)));
};

static YmAac s_aac;
static int s_aac_busy;

static int aac_init_decoder(YmAac *a)
{
    int rc;

    a->decoder_ready = 0;
    if (a->edram) {
        rc = sceAudiocodecReleaseEDRAM(a->codec);
        if (rc < 0) return rc;
        a->edram = 0;
    }
    memset(a->codec, 0, sizeof(a->codec));
    rc = sceAudiocodecCheckNeedMem(a->codec, PSP_CODEC_AAC);
    if (rc < 0) goto fail;
    rc = sceAudiocodecGetEDRAM(a->codec, PSP_CODEC_AAC);
    if (rc < 0) goto fail;
    a->edram = 1;
    a->codec[10] = (unsigned long)a->rate;
    rc = sceAudiocodecInit(a->codec, PSP_CODEC_AAC);
    if (rc < 0) goto fail;
    a->decoder_ready = 1;
    return 0;

fail:
    logLine("ym_aac: hardware init failed rc=0x%08X\n", rc);
    return rc;
}

YmAac *ym_aac_open_file(AudioReader *reader)
{
    YmAac *a = &s_aac;
    const unsigned char *asc;
    int asc_len = 0;
    int frequency_index;
    int channels;
    static const int rates[] = {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000
    };

    const char *path;
    if (!reader) return NULL;
    path = audio_reader_source(reader)->path;
    if (s_aac_busy) {
        logLine("ym_aac: already open, refusing\n");
        return NULL;
    }
    memset(a, 0, sizeof(*a));
    a->reader = reader;
    a->t = mp4_track_open_reader(reader);
    asc = mp4_track_asc(a->t, &asc_len);
    /* AOT 2 with standard frequency index and no GASpecificConfig flags.
     * Extended ASC is not treated as LC: Yandex HE also starts with AOT 2. */
    frequency_index = asc && asc_len >= 2 ? ((asc[0] & 7) << 1) | (asc[1] >> 7) : 15;
    channels = asc && asc_len >= 2 ? (asc[1] >> 3) & 15 : 0;
    if (!asc || asc_len != 2 || (asc[0] >> 3) != 2 || (asc[1] & 7) != 0 ||
        frequency_index >= (int)(sizeof(rates) / sizeof(rates[0])) ||
        channels < 1 || channels > 2) {
        logLine("ym_aac: unsupported AAC-LC config '%s'\n", path);
        if (a->t) {
            mp4_track_close(a->t);
            a->t = NULL;
        }
        return NULL;
    }
    a->rate = rates[frequency_index];
    a->total_samples = mp4_track_sample_count(a->t);
    if (a->total_samples <= 0 || aac_init_decoder(a) != 0) {
        if (a->edram) sceAudiocodecReleaseEDRAM(a->codec);
        a->edram = 0;
        mp4_track_close(a->t);
        a->t = NULL;
        return NULL;
    }
    s_aac_busy = 1;
    logLine("ym_aac: hardware AAC-LC open '%s' samples=%d rate=%d input_channels=%d output_channels=2\n",
            path, a->total_samples, a->rate, channels);
    return a;
}

int ym_aac_format(YmAac *a, YmAacFormat *out)
{
    if (!a || !out || !a->t) return -1;
    out->sample_rate = a->rate;
    out->channels = 2;
    out->total_frames = mp4_track_total_pcm(a->t, a->rate);
    if (out->total_frames < 0) out->total_frames = 0;
    return 0;
}

/* One MP4 access unit; the common reader waits for its complete payload. */
static int aac_decode_one(YmAac *a, short *pcm)
{
    int64_t off = 0;
    int size = 0, decode_rc;
    if (mp4_track_sample_at(a->t, a->next_sample, &off, &size) != 0 ||
        size <= 0 || size > (int)sizeof(a->sample_buf)) return -1;
    if (audio_reader_read_at(a->reader, off, a->sample_buf, size) != size) return -1;
    /* Raw AAC has no ADTS header. Firmware reads a fixed 0x600-byte
     * window even when the access unit is shorter. */
    memset(a->sample_buf + size, 0, sizeof(a->sample_buf) - (size_t)size);
    a->codec[6] = (unsigned long)a->sample_buf;
    a->codec[7] = (unsigned long)size;
    a->codec[8] = (unsigned long)pcm;
    a->codec[9] = YM_AAC_PCM_BYTES;
    a->codec[2] = 0;
    sceKernelDcacheWritebackRange(a->sample_buf, sizeof(a->sample_buf));
    sceKernelDcacheWritebackInvalidateRange(pcm, YM_AAC_PCM_BYTES);
    decode_rc = sceAudiocodecDecode(a->codec, PSP_CODEC_AAC);
    sceKernelDcacheInvalidateRange(pcm, YM_AAC_PCM_BYTES);
    if (decode_rc < 0 || a->codec[2] != 0 ||
        (a->codec[9] != 0 && a->codec[9] != YM_AAC_PCM_BYTES)) {
        logLine("ym_aac: hardware decode failed sample=%d rc=0x%08X err=0x%08lX consumed=%lu pcm=%lu\n",
                a->next_sample, decode_rc, a->codec[2], a->codec[7], a->codec[9]);
        return -1;
    }
    if (a->next_sample == 0) {
        logLine("ym_aac: hardware first AU consumed=%lu pcm=%lu\n",
                a->codec[7], a->codec[9]);
    }
    a->next_sample++;
    return a->codec[9] == 0 ? 0 : YM_AAC_FRAME_SAMPLES;
}

int ym_aac_read_s16(YmAac *a, short *pcm)
{
    if (!a || !pcm || !a->decoder_ready) return -1;
    while (a->next_sample < a->total_samples) {
        int rc = aac_decode_one(a, pcm);
        if (rc != 0) return rc;
    }
    return 0;
}

int ym_aac_seek_frame(YmAac *a, int64_t frame)
{
    int idx;
    if (!a || !a->t || frame < 0) return -2;
    idx = mp4_track_sample_for_pcm(a->t, frame, a->rate);
    if (idx < 0) return -2;
    if (idx >= a->total_samples) idx = a->total_samples - 1;
    if (idx < 0) return -2;
    /* Сбрасываем аппаратный декодер перед новой позицией. */
    if (aac_init_decoder(a) != 0) return -2;
    a->next_sample = idx;
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
    if (a->edram) {
        int rc = sceAudiocodecReleaseEDRAM(a->codec);
        if (rc < 0) logLine("ym_aac: ReleaseEDRAM failed rc=0x%08X\n", rc);
    }
    if (a->t) mp4_track_close(a->t);
    memset(a, 0, sizeof(*a));
    s_aac_busy = 0;
}
