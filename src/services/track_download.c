#include "services/track_download.h"

#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/net_http.h"

int track_download_build_path(const char *track_id, char *out, size_t out_size)
{
    return track_download_build_path_for_codec(track_id, "mp3", out, out_size);
}

int track_download_build_path_for_codec(const char *track_id, const char *codec,
                                        char *out, size_t out_size)
{
    int rc;
    const char *ext;

    if (!track_id || !out || out_size == 0) {
        return -1;
    }
    if (codec && strcmp(codec, "flac") == 0) {
        ext = "flac";
    } else if (codec && strcmp(codec, "aac") == 0) {
        ext = "aac";   /* контейнер MP4, читается плеером на лету */
    } else if (codec && strcmp(codec, "mp4") == 0) {
        ext = "mp4";   /* flac-mp4: кэш и источник плеера без демукса */
    } else {
        ext = "mp3";
    }

    /* Cache path contract: cache_music/<track_id>.<mp3|flac|aac|mp4>
     * Calibrated: "cache_music/" (12) + observed max track_id (10 digits) + ".flac" (5) = 27 chars.
     * out_size of 64 bytes is the documented minimum; callers use 64. */
    rc = snprintf(out, out_size, "cache_music/%s.%s", track_id, ext);
    if (rc < 0 || rc >= (int)out_size) {
        logLine("track_dl: path too long for id='%s'\n", track_id);
        return -1;
    }
    return 0;
}

int track_download_mp3_to_ram(const char *url,
                              NetHttpStreamChunkCallback on_chunk,
                              NetHttpDownloadProgressCallback on_progress,
                              void *user_data)
{
    return track_download_mp3_to_ram_from(url, 0, 0, on_chunk, on_progress,
                                          user_data);
}

int track_download_mp3_to_ram_from(const char *url, int offset, int expected_total,
                                   NetHttpStreamChunkCallback on_chunk,
                                   NetHttpDownloadProgressCallback on_progress,
                                   void *user_data)
{
    if (!url || offset < 0 || (offset > 0 && expected_total <= offset)) return -1;
    logLine("track_dl: ram stream url len=%d offset=%d\n",
            (int)strlen(url), offset);
    return http_stream(&(HttpRequest){ .method = HTTP_GET, .url = url,
                                      .identity_encoding = 1,
                                      .range_enabled = expected_total > 0,
                                      .range_start = offset,
                                      .range_total = expected_total },
                       &(HttpSink){ .on_chunk = on_chunk,
                                    .on_progress = on_progress,
                                    .user = user_data });
}
