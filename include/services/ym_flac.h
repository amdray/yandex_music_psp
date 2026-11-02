#ifndef YM_SERVICES_YM_FLAC_H
#define YM_SERVICES_YM_FLAC_H
#include "services/audio_reader.h"

/* One dr_flac decoder with a static allocation pool. */
typedef struct YmFlac YmFlac;
typedef struct {
    int sample_rate;
    int channels;
    int bits_per_sample;
    int64_t total_frames;
} YmFlacFormat;

YmFlac *ym_flac_open_file(AudioReader *reader);
/* Virtual fLaC stream: dfLa header followed by MP4 sample payloads. */
YmFlac *ym_flac_open_mp4(AudioReader *reader);
int ym_flac_format(YmFlac *f, YmFlacFormat *out);
/* PCM frames; 0 = EOF, negative = error/cancel. */
int64_t ym_flac_read_s16(YmFlac *f, short *pcm, int frames);
int ym_flac_seek_frame(YmFlac *f, int64_t frame);
int ym_flac_at_end(YmFlac *f);
void ym_flac_close(YmFlac *f);
#endif
