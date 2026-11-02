#ifndef YM_SERVICES_YM_AAC_H
#define YM_SERVICES_YM_AAC_H
#include "services/audio_reader.h"

/* One hardware AAC-LC decoder; raw access units come from mp4_track. */
typedef struct YmAac YmAac;
#define YM_AAC_FRAME_SAMPLES 1024
typedef struct {
    int sample_rate;
    int channels;
    int64_t total_frames;
} YmAacFormat;

YmAac *ym_aac_open_file(AudioReader *reader);
int ym_aac_format(YmAac *a, YmAacFormat *out);
/* PCM frames; 0 = EOF, negative = error/cancel. */
int ym_aac_read_s16(YmAac *a, short *pcm);
/* 0 = success, negative = error/cancel. */
int ym_aac_seek_frame(YmAac *a, int64_t frame);
int ym_aac_at_end(YmAac *a);
void ym_aac_close(YmAac *a);
#endif
