#include "services/video_player.h"

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmpeg.h>
#include <psputility.h>
#include <psputility_modules.h>

#include "core/fs.h"
#include "core/logger.h"

#define PSMF_HEADER_SIZE 2048
#define PACKET_SIZE 2048
#define RING_PACKETS 64
#define DECODE_STRIDE 512
#define TEXTURE_HEIGHT 256
#define MPEG_NO_DATA ((int)0x80618001)

struct VideoPlayer {
    SceUID fd;
    SceOff stream_offset, next_offset;
    int stream_size, remaining;
    int width, height, frame_index;
    int first_frame_logged, loop_count, failed;
    unsigned int frame_duration_us;
    int mpeg_inited, ring_constructed, mpeg_created, stream_registered;
    SceMpegRingbuffer ring;
    SceMpeg mpeg;
    SceMpegStream *stream;
    SceMpegAu au;
    SceInt32 au_unknown, decode_status;
    void *ring_data, *mpeg_data, *es_data, *rgb;
};

static SceInt32 ring_read(ScePVoid data, SceInt32 packets, ScePVoid param)
{
    VideoPlayer *vp = (VideoPlayer *)param;
    int wanted = packets * PACKET_SIZE;
    int total = 0;
    if (wanted > vp->remaining) wanted = vp->remaining;
    wanted &= ~(PACKET_SIZE - 1);
    if (wanted <= 0) return 0;
    if (sceIoLseek(vp->fd, vp->next_offset, PSP_SEEK_SET) < 0) return -1;
    while (total < wanted) {
        int rc = sceIoRead(vp->fd, (unsigned char *)data + total, wanted - total);
        if (rc <= 0) break;
        total += rc;
    }
    total &= ~(PACKET_SIZE - 1);
    vp->next_offset += total;
    vp->remaining -= total;
    return total / PACKET_SIZE;
}

static int next_au(VideoPlayer *vp)
{
    int tries;
    for (tries = 0; tries < 64; ++tries) {
        int available = sceMpegRingbufferAvailableSize(&vp->ring);
        int rc;
        if (available < 0) return available;
        if (available > 0 && vp->remaining > 0) {
            int request = available > 32 ? 32 : available;
            rc = sceMpegRingbufferPut(&vp->ring, request, available);
            if (rc < 0) return rc;
        }
        rc = sceMpegGetAvcAu(&vp->mpeg, vp->stream, &vp->au, &vp->au_unknown);
        if (rc == 0) return 0;
        if (rc != MPEG_NO_DATA) return rc;
        if (vp->remaining == 0 &&
            sceMpegRingbufferAvailableSize(&vp->ring) == RING_PACKETS) {
            return MPEG_NO_DATA;
        }
    }
    return MPEG_NO_DATA;
}

static int restart(VideoPlayer *vp)
{
    int rc = sceMpegFlushAllStream(&vp->mpeg);
    if (rc < 0) return rc;
    vp->next_offset = vp->stream_offset;
    vp->remaining = vp->stream_size;
    vp->frame_index = 0;
    return 0;
}

VideoPlayer *video_player_open(const char *path)
{
    unsigned char header[PSMF_HEADER_SIZE] __attribute__((aligned(64)));
    SceIoStat stat;
    VideoPlayer *vp = (VideoPlayer *)calloc(1, sizeof(*vp));
    SceInt32 stream_offset = 0, stream_size = 0;
    int ring_size, mpeg_size, rc = -1;
    if (!vp) return NULL;
    vp->fd = -1;
    vp->fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (vp->fd < 0 || sceIoRead(vp->fd, header, sizeof(header)) != sizeof(header) ||
        memcmp(header, "PSMF", 4) != 0 || fs_getstat(path, &stat) != 0) goto fail;
    vp->width = (int)header[0x8E] * 16;
    vp->height = (int)header[0x8F] * 16;
    if (vp->width != 208 || vp->height != 208) goto fail;

    rc = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    if (rc < 0 && (unsigned)rc != 0x80111102u) goto fail;
    rc = sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE);
    if (rc < 0 && (unsigned)rc != 0x80111102u) goto fail;
    rc = sceMpegInit();
    if (rc < 0) goto fail;
    vp->mpeg_inited = 1;
    ring_size = sceMpegRingbufferQueryMemSize(RING_PACKETS);
    mpeg_size = sceMpegQueryMemSize(0);
    if (ring_size <= 0 || mpeg_size <= 0) goto fail;
    vp->ring_data = memalign(64, (size_t)ring_size);
    vp->mpeg_data = memalign(64, (size_t)((mpeg_size + 63) & ~63));
    vp->rgb = memalign(64, (size_t)DECODE_STRIDE * TEXTURE_HEIGHT * 4);
    if (!vp->ring_data || !vp->mpeg_data || !vp->rgb) goto fail;
    memset(vp->rgb, 0, (size_t)DECODE_STRIDE * TEXTURE_HEIGHT * 4);
    rc = sceMpegRingbufferConstruct(&vp->ring, RING_PACKETS, vp->ring_data,
                                    ring_size, ring_read, vp);
    if (rc < 0) goto fail;
    vp->ring_constructed = 1;
    rc = sceMpegCreate(&vp->mpeg, vp->mpeg_data, mpeg_size, &vp->ring,
                       DECODE_STRIDE, 0, 0);
    if (rc < 0) goto fail;
    vp->mpeg_created = 1;
    rc = sceMpegQueryStreamOffset(&vp->mpeg, header, &stream_offset);
    if (rc < 0) goto fail;
    rc = sceMpegQueryStreamSize(header, &stream_size);
    if (rc < 0 || (SceOff)stream_offset + stream_size > stat.st_size) goto fail;
    vp->stream_offset = stream_offset;
    vp->stream_size = stream_size;
    vp->next_offset = stream_offset;
    vp->remaining = stream_size;
    vp->stream = sceMpegRegistStream(&vp->mpeg, 0, 0);
    if (!vp->stream) goto fail;
    vp->stream_registered = 1;
    vp->es_data = sceMpegMallocAvcEsBuf(&vp->mpeg);
    if (!vp->es_data) goto fail;
    memset(&vp->au, 0xFF, sizeof(vp->au));
    rc = sceMpegInitAu(&vp->mpeg, vp->es_data, &vp->au);
    if (rc < 0) goto fail;
    {
        SceMpegAvcMode mode = { -1, PSP_DISPLAY_PIXEL_FORMAT_8888 };
        rc = sceMpegAvcDecodeMode(&vp->mpeg, &mode);
        if (rc < 0) goto fail;
    }
    vp->frame_duration_us = 33367;
    logLine("vp: PSMF ready path=%s bytes=%d mode=0 no_custom_prx=1\n",
            path, stream_size);
    return vp;
fail:
    logLine("vp: PSMF open failed rc=0x%08X path=%s\n", (unsigned)rc, path);
    video_player_close(vp);
    return NULL;
}

const void *video_player_next_frame(VideoPlayer *vp, int *out_w, int *out_h)
{
    void *output;
    int rc;
    if (!vp) return NULL;
    rc = next_au(vp);
    if (rc == MPEG_NO_DATA) {
        int restart_rc = restart(vp);
        if (restart_rc < 0) {
            vp->failed = 1;
            logLine("vp: loop restart failed rc=0x%08X\n", (unsigned)restart_rc);
            return NULL;
        }
        vp->loop_count++;
        logLine("vp: loop restart ok count=%d\n", vp->loop_count);
        rc = next_au(vp);
    }
    if (rc < 0) {
        vp->failed = 1;
        logLine("vp: GetAvcAu failed rc=0x%08X frame=%d loop=%d\n",
                (unsigned)rc, vp->frame_index, vp->loop_count);
        return NULL;
    }
    output = vp->rgb;
    rc = sceMpegAvcDecode(&vp->mpeg, &vp->au, DECODE_STRIDE, &output,
                          &vp->decode_status);
    vp->frame_index++;
    if (rc < 0 || !output) {
        vp->failed = 1;
        logLine("vp: AvcDecode failed rc=0x%08X status=%d frame=%d loop=%d\n",
                (unsigned)rc, (int)vp->decode_status, vp->frame_index,
                vp->loop_count);
        return NULL;
    }
    sceKernelDcacheInvalidateRange(output,
        (unsigned int)((size_t)DECODE_STRIDE * vp->height * 4));
    if (!vp->first_frame_logged) {
        vp->first_frame_logged = 1;
        logLine("vp: first frame ok au_size=%u output=%p init=%d\n",
                (unsigned)vp->au.iAuSize, output, (int)vp->decode_status);
    }
    if (out_w) *out_w = vp->width;
    if (out_h) *out_h = vp->height;
    return output;
}

int video_player_fps(const VideoPlayer *vp) { return vp ? 30 : 0; }
int video_player_failed(const VideoPlayer *vp) { return vp ? vp->failed : 1; }
unsigned int video_player_frame_duration_us(const VideoPlayer *vp)
{ return vp ? vp->frame_duration_us : 0; }
int video_player_tex_dim(const VideoPlayer *vp) { (void)vp; return DECODE_STRIDE; }

void video_player_close(VideoPlayer *vp)
{
    if (!vp) return;
    if (vp->mpeg_created) {
        SceInt32 status = 0;
        void *output = vp->rgb;
        sceMpegAvcDecodeStop(&vp->mpeg, DECODE_STRIDE, &output, &status);
    }
    if (vp->es_data && vp->mpeg_created) sceMpegFreeAvcEsBuf(&vp->mpeg, vp->es_data);
    if (vp->stream_registered) sceMpegUnRegistStream(&vp->mpeg, vp->stream);
    if (vp->mpeg_created) sceMpegDelete(&vp->mpeg);
    if (vp->ring_constructed) sceMpegRingbufferDestruct(&vp->ring);
    if (vp->mpeg_inited) sceMpegFinish();
    if (vp->fd >= 0) sceIoClose(vp->fd);
    free(vp->rgb);
    free(vp->mpeg_data);
    free(vp->ring_data);
    free(vp);
}
