#ifndef YM_AUDIO_QUALITY_H
#define YM_AUDIO_QUALITY_H

typedef enum {
    AUDIO_QUALITY_UNKNOWN = -1,
    AUDIO_QUALITY_LQ,
    AUDIO_QUALITY_NQ,
    AUDIO_QUALITY_HQ,
    AUDIO_QUALITY_LOSSLESS,
    AUDIO_QUALITY_COUNT
} AudioQuality;

const char *audio_quality_name(AudioQuality quality);
AudioQuality audio_quality_parse(const char *name);

#endif
