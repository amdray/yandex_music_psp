#ifndef YM_SERVICES_YM_API_TYPES_H
#define YM_SERVICES_YM_API_TYPES_H

#include <stddef.h>

#include "app/track.h"
#include "app/audio_quality.h"

typedef enum {
    YM_OK = 0,
    YM_ERR_HTTP = -1,
    YM_ERR_AUTH = -2,
    YM_ERR_PARSE = -3,
    YM_ERR_NOT_FOUND = -4,
    YM_ERR_UNSUPPORTED = -5,
    YM_ERR_NO_MEMORY = -6,
    YM_ERR_NETWORK = -7,
    YM_ERR_CANCELLED = -8
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
    AudioQuality quality;
    int encrypted;                 /* 1 = transport encraw, поток AES-128-CTR */
    unsigned char aes_key[16];     /* ключ из downloadInfo.key (hex), nonce = 0 */
} YmDownloadVariant;

typedef enum {
    YM_API_STREAM_CONTINUE = 0,
    YM_API_STREAM_STOP = 1,
    YM_API_STREAM_ERROR = -1
} YmApiStreamDecision;

typedef YmApiStreamDecision (*YmApiTrackCallback)(
    const TrackRef *ref,
    const TrackEntry *entry,
    void *user_data
);


#endif
