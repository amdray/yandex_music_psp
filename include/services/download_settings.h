#ifndef YM_DOWNLOAD_SETTINGS_H
#define YM_DOWNLOAD_SETTINGS_H

#include "app/audio_quality.h"

typedef enum {
    DOWNLOAD_CODEC_MP3,
    DOWNLOAD_CODEC_AAC,
    DOWNLOAD_CODEC_FLAC,
    DOWNLOAD_CODEC_COUNT
} DownloadCodec;

typedef enum {
    DOWNLOAD_QUALITY_LQ = AUDIO_QUALITY_LQ,
    DOWNLOAD_QUALITY_NQ = AUDIO_QUALITY_NQ,
    DOWNLOAD_QUALITY_HQ = AUDIO_QUALITY_HQ,
    DOWNLOAD_QUALITY_COUNT = AUDIO_QUALITY_LOSSLESS
} DownloadQuality;

typedef struct {
    DownloadCodec codec;
    DownloadQuality quality;
} DownloadSettings;

/* init/set run on the UI thread; get copies one immutable settings record. */
void download_settings_init(void);
DownloadSettings download_settings_get(void);
AudioQuality download_settings_required_quality(DownloadSettings settings);
void download_settings_set_codec(DownloadCodec codec);
void download_settings_set_quality(DownloadQuality quality);
const char *download_codec_name(DownloadCodec codec);
const char *download_quality_name(DownloadQuality quality);

#endif
