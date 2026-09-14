#include "services/track_download.h"

#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/net_http.h"

int track_download_build_path(const char *track_id, char *out, size_t out_size)
{
    int rc;

    if (!track_id || !out || out_size == 0) {
        return -1;
    }

    /* Cache path contract: cache_music/<track_id>.mp3
     * Calibrated: "cache_music/" (12) + observed max track_id (10 digits) + ".mp3" (4) = 26 chars.
     * out_size of 64 bytes is the documented minimum; callers use 64. */
    rc = snprintf(out, out_size, "cache_music/%s.mp3", track_id);
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
