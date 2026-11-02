#ifndef YM_SERVICES_YM_MP3_H
#define YM_SERVICES_YM_MP3_H

#include "services/audio_reader.h"

enum {
    YM_MP3_ERR_CACHE = -1003,
    YM_MP3_ERR_SOURCE = -1004,
    YM_MP3_EOF = -1005
};

typedef struct {
    int sample_rate;
    int channels;
    int bitrate_kbps;
} YmMp3Format;

int ym_mp3_init(void);
void ym_mp3_shutdown(void);
/* One hardware decoder; the common reader outlives it. */
int ym_mp3_open(AudioReader *reader, YmMp3Format *format);
/* PCM bytes, YM_MP3_EOF or a negative error. */
int ym_mp3_read_s16(short **pcm);
/* Native SDK position reset; position_ms is set after successful reset. */
int ym_mp3_seek(int target_ms, int duration_ms, YmMp3Format *format, int *position_ms);
void ym_mp3_close(void);

#endif
