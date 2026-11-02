#include "services/audio_storage.h"

#include <stdio.h>
#include <string.h>

int audio_storage_build_path(const char *track_id, const char *extension, AudioQuality quality,
                             char *out, size_t out_size)
{
    int n;
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    if (!track_id || !track_id[0] || !extension ||
        (strcmp(extension, "mp3") != 0 && strcmp(extension, "flac") != 0 &&
         strcmp(extension, "aac") != 0 && strcmp(extension, "mp4") != 0)) return -1;
    if (quality == AUDIO_QUALITY_UNKNOWN) {
        n = snprintf(out, out_size, "cache_music/%s.%s", track_id, extension);
    } else {
        const char *name = audio_quality_name(quality);
        if (!name) return -1;
        n = snprintf(out, out_size, "cache_music/%s.%s.%s", track_id, name, extension);
    }
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

int audio_storage_complete_path(const char *path, char *out, size_t out_size)
{
    size_t length;
    static const char suffix[] = ".part";
    if (!path || !out || out_size == 0) return -1;
    length = strlen(path);
    if (length >= sizeof(suffix) - 1 &&
        strcmp(path + length - (sizeof(suffix) - 1), suffix) == 0) {
        length -= sizeof(suffix) - 1;
    }
    if (length == 0 || length >= out_size) return -1;
    memcpy(out, path, length);
    out[length] = '\0';
    return 0;
}

