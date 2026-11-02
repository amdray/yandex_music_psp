#ifndef YM_SERVICES_AUDIO_STORAGE_H
#define YM_SERVICES_AUDIO_STORAGE_H

#include <stddef.h>
#include "app/track.h"
#include "app/audio_quality.h"

enum {
    AUDIO_STORAGE_PATH_CAPACITY =
        sizeof("cache_music/") - 1 + TRACK_ID_SIZE + sizeof(".lossless.flac.part") - 1
};

/* Downloads use .part; completed files keep it while held by the player. */
/* UNKNOWN addresses a legacy file without a quality component. */
int audio_storage_build_path(const char *track_id, const char *extension, AudioQuality quality,
                             char *out, size_t out_size);
int audio_storage_complete_path(const char *path, char *out, size_t out_size);
#endif
