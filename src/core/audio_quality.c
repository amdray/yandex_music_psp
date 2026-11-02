#include "app/audio_quality.h"

#include <stddef.h>
#include <string.h>

static const char *const kNames[AUDIO_QUALITY_COUNT] = {
    "lq", "nq", "hq", "lossless"
};

const char *audio_quality_name(AudioQuality quality)
{
    return quality >= AUDIO_QUALITY_LQ && quality < AUDIO_QUALITY_COUNT
        ? kNames[quality] : NULL;
}

AudioQuality audio_quality_parse(const char *name)
{
    int quality;
    if (!name) return AUDIO_QUALITY_UNKNOWN;
    for (quality = AUDIO_QUALITY_LQ; quality < AUDIO_QUALITY_COUNT; quality++) {
        if (strcmp(name, kNames[quality]) == 0) return (AudioQuality)quality;
    }
    return AUDIO_QUALITY_UNKNOWN;
}
