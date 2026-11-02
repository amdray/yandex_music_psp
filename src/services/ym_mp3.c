#include "services/ym_mp3.h"

#include <pspkernel.h>
#include <pspmp3.h>
#include <psputility_modules.h>
#include <string.h>

#include "core/logger.h"

#define MP3_BUF_SIZE (16 * 1024)
#define PCM_BUF_SIZE ((4096 + 32) * 4)
#define MP3_DECODE_EOS ((int)0x80671402u)

static unsigned char s_mp3_buf[MP3_BUF_SIZE] __attribute__((aligned(64)));
static unsigned char s_pcm_buf[PCM_BUF_SIZE] __attribute__((aligned(64)));
static int s_handle = -1;
static int s_resource_initialized;
static const AudioSourceSnapshot *s_source;
static AudioReader *s_reader;
static int64_t s_input_end;

int ym_mp3_init(void)
{
    int rc = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    if (rc < 0 && (unsigned int)rc != SCE_ERROR_MODULE_ALREADY_LOADED) return rc;
    rc = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    if (rc < 0 && (unsigned int)rc != SCE_ERROR_MODULE_ALREADY_LOADED) return rc;
    rc = sceMp3InitResource();
    if (rc < 0) return rc;
    s_resource_initialized = 1;
    return 0;
}

void ym_mp3_shutdown(void)
{
    if (s_resource_initialized) {
        sceMp3TermResource();
        s_resource_initialized = 0;
    }
}

void ym_mp3_close(void)
{
    if (s_handle >= 0) {
        sceMp3ReleaseMp3Handle(s_handle);
        s_handle = -1;
    }
}

static int mp3_reserve(int64_t start)
{
    SceMp3InitArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.mp3StreamStart = start;
    arg.mp3StreamEnd = s_source->content_length;
    memset(s_mp3_buf, 0, sizeof(s_mp3_buf));
    memset(s_pcm_buf, 0, sizeof(s_pcm_buf));
    sceKernelDcacheWritebackRange(s_mp3_buf, sizeof(s_mp3_buf));
    sceKernelDcacheWritebackRange(s_pcm_buf, sizeof(s_pcm_buf));
    arg.mp3Buf = s_mp3_buf;
    arg.mp3BufSize = sizeof(s_mp3_buf);
    arg.pcmBuf = s_pcm_buf;
    arg.pcmBufSize = sizeof(s_pcm_buf);
    s_handle = sceMp3ReserveMp3Handle(&arg);
    return s_handle < 0 ? s_handle : 0;
}

static int mp3_feed(void)
{
    unsigned char *dst = NULL;
    SceInt32 size = 0, pos = 0;
    int got, rc = sceMp3GetInfoToAddStreamData(s_handle, &dst, &size, &pos);
    if (rc < 0) return rc;
    if (size < 0) return YM_MP3_ERR_SOURCE;
    if (size == 0) return 0;
    got = audio_reader_read_at(s_reader, pos, dst, size);
    if (got < 0) return got;
    if (got == 0) return 0;
    sceKernelDcacheWritebackRange(dst, got);
    rc = sceMp3NotifyAddStreamData(s_handle, got);
    if (rc < 0) return rc;
    s_input_end = (int64_t)pos + got;
    return got;
}

static int mp3_format(YmMp3Format *format)
{
    int rc = sceMp3GetSamplingRate(s_handle);
    if (rc < 0) return rc;
    format->sample_rate = rc;
    rc = sceMp3GetMp3ChannelNum(s_handle);
    if (rc < 0) return rc;
    format->channels = rc;
    return 0;
}

int ym_mp3_open(AudioReader *reader, YmMp3Format *format)
{
    int rc;
    const AudioSourceSnapshot *source = audio_reader_source(reader);
    if (!source || !format) return YM_MP3_ERR_SOURCE;
    s_source = source;
    s_reader = reader;
    s_input_end = 0;
    rc = mp3_reserve(0);
    if (rc < 0) return rc;
    logLine("ap: sceMp3ReserveMp3Handle ok track_id='%s' handle=%d stream_end=%d\n",
            source->track_id, s_handle,
            (int)source->content_length);
    rc = mp3_feed();
    if (rc <= 0) return rc < 0 ? rc : YM_MP3_ERR_SOURCE;
    rc = sceMp3Init(s_handle);
    if (rc < 0) return rc;
    logLine("ap: sceMp3Init ok track_id='%s' handle=%d\n", source->track_id, s_handle);
    format->bitrate_kbps = sceMp3GetBitRate(s_handle);
    rc = sceMp3SetLoopNum(s_handle, 0);
    if (rc < 0) return rc;
    logLine("ap: loop mode track_id='%s' loop=%d\n", source->track_id, sceMp3GetLoopNum(s_handle));
    return mp3_format(format);
}

int ym_mp3_read_s16(short **pcm)
{
    for (;;) {
        int input_eof = 0;
        int rc = sceMp3CheckStreamDataNeeded(s_handle);
        if (rc < 0) return rc;
        if (rc > 0) {
            rc = mp3_feed();
            if (rc < 0) return rc;
            input_eof = rc == 0;
        }
        rc = sceMp3Decode(s_handle, pcm);
        if (rc == MP3_DECODE_EOS || rc == 0) {
            unsigned char byte;
            if (s_input_end < s_source->content_length) {
                if (input_eof) return YM_MP3_ERR_SOURCE;
                rc = sceMp3CheckStreamDataNeeded(s_handle);
                if (rc < 0) return rc;
                if (rc > 0) continue;
                return YM_MP3_ERR_SOURCE;
            }
            rc = audio_reader_read_at(s_reader, s_source->content_length, &byte, 1);
            return rc < 0 ? rc : rc == 0 ? YM_MP3_EOF : YM_MP3_ERR_SOURCE;
        }
        if (rc > 0) sceKernelDcacheInvalidateRange(*pcm, rc);
        return rc;
    }
}

int ym_mp3_seek(int target_ms, int duration_ms, YmMp3Format *format, int *position_ms)
{
    int count, rc;
    unsigned frame;
    *position_ms = -1;
    if (duration_ms <= 0 || target_ms < 0) return YM_MP3_ERR_SOURCE;
    if (target_ms >= duration_ms) return YM_MP3_EOF;
    if (target_ms == 0) {
        rc = sceMp3ResetPlayPosition(s_handle);
    } else {
        count = sceMp3GetFrameNum(s_handle);
        if (count <= 0) return count < 0 ? count : YM_MP3_ERR_SOURCE;
        frame = (unsigned)((int64_t)count * target_ms / duration_ms);
        rc = sceMp3ResetPlayPositionByFrame(s_handle, frame);
    }
    if (rc < 0) return rc;
    rc = mp3_format(format);
    if (rc < 0) return rc;
    *position_ms = target_ms;
    return 0;
}
