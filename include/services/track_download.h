#ifndef YM_SERVICES_TRACK_DOWNLOAD_H
#define YM_SERVICES_TRACK_DOWNLOAD_H

#include <stddef.h>

#include "services/net_http.h"

// Stream an unencrypted MP3 from url into RAM only (no file I/O).
// on_chunk receives raw body bytes as they arrive.
// on_progress receives (written_total, content_length, user_data) after each chunk;
//   first call is (0, content_length, user_data) immediately after headers are parsed.
// Returns 0 on success, -1 on error.
int track_download_mp3_to_ram(const char *url,
                              NetHttpStreamChunkCallback on_chunk,
                              NetHttpDownloadProgressCallback on_progress,
                              void *user_data);

// Continue the same MP3 at byte offset. Progress values remain absolute for
// the complete object, so callers can append directly to an existing buffer.
int track_download_mp3_to_ram_from(const char *url, int offset, int expected_total,
                                   NetHttpStreamChunkCallback on_chunk,
                                   NetHttpDownloadProgressCallback on_progress,
                                   void *user_data);

// Build the final cache path for a track MP3.
//
// Current cache path contract:
//   cache path format: cache_music/<track_id>.mp3
//
// out must point to a caller-owned buffer of at least out_size bytes.
// Calibrated minimum: 64 bytes covers:
//   "cache_music/" (12) + max observed track_id (10 digits) + ".mp3" (4) = 26 chars;
//   64 gives safe headroom for longer IDs.
// Returns 0 on success, -1 if the buffer would truncate.
int track_download_build_path(const char *track_id, char *out, size_t out_size);

#endif /* YM_SERVICES_TRACK_DOWNLOAD_H */
