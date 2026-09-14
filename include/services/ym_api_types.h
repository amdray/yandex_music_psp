#ifndef YM_SERVICES_YM_API_TYPES_H
#define YM_SERVICES_YM_API_TYPES_H

#include <stddef.h>

#include "app/track.h"

typedef enum {
    YM_OK = 0,
    YM_ERR_HTTP = -1,
    YM_ERR_AUTH = -2,
    YM_ERR_PARSE = -3,
    YM_ERR_NOT_FOUND = -4,
    YM_ERR_UNSUPPORTED = -5,
    YM_ERR_NO_MEMORY = -6,
    YM_ERR_NETWORK = -7
} YmStatus;

typedef struct {
    const char *oauth_token;
    int timeout_ms;
} YmApiContext;

typedef struct {
    char url[512];
    char codec[16];
    char transport[16];
    int bitrate_kbps;
    int encrypted;
} YmDownloadVariant;

typedef enum {
    YM_API_STREAM_CONTINUE = 0,
    YM_API_STREAM_STOP = 1,
    YM_API_STREAM_ERROR = -1
} YmApiStreamDecision;

typedef YmApiStreamDecision (*YmApiTrackCallback)(
    const TrackEntry *entry,
    void *user_data
);

typedef YmApiStreamDecision (*YmApiTrackIdCallback)(
    const char *track_id,
    void *user_data
);

typedef struct YmPlaylistTracksParser {
    int result_key_match;
    int waiting_result_object;
    int in_result_object;
    int key_match;
    int waiting_array_start;
    int in_tracks_array;
    int capturing_object;
    int object_depth;
    int in_string;
    int escape_next;
    char *obj_buf;
    int obj_len;
    int obj_cap;
    YmApiTrackIdCallback on_track_id;
    void *user_data;
} YmPlaylistTracksParser;

#endif
