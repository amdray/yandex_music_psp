#include "services/download_settings.h"

#include <stdio.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"

#define DOWNLOAD_CFG_PATH "config/download.cfg"
#define LEGACY_CFG_PATH "config/eq.cfg"

#define SETTINGS_ROW(codec) { { codec, DOWNLOAD_QUALITY_LQ }, \
                              { codec, DOWNLOAD_QUALITY_NQ }, \
                              { codec, DOWNLOAD_QUALITY_HQ } }
static const DownloadSettings kSettings[DOWNLOAD_CODEC_COUNT][DOWNLOAD_QUALITY_COUNT] = {
    SETTINGS_ROW(DOWNLOAD_CODEC_MP3),
    SETTINGS_ROW(DOWNLOAD_CODEC_AAC),
    SETTINGS_ROW(DOWNLOAD_CODEC_FLAC)
};
#undef SETTINGS_ROW

static const char *const kCodecNames[] = { "mp3", "aac", "flac" };
static const DownloadSettings *volatile s_current =
    &kSettings[DOWNLOAD_CODEC_AAC][DOWNLOAD_QUALITY_NQ];

DownloadSettings download_settings_get(void)
{
    const DownloadSettings *current = s_current;
    return *current;
}

const char *download_codec_name(DownloadCodec codec)
{
    return codec >= 0 && codec < DOWNLOAD_CODEC_COUNT ? kCodecNames[codec] : NULL;
}

AudioQuality download_settings_required_quality(DownloadSettings settings)
{
    return settings.codec == DOWNLOAD_CODEC_FLAC
        ? AUDIO_QUALITY_LOSSLESS : (AudioQuality)settings.quality;
}

const char *download_quality_name(DownloadQuality quality)
{
    return quality >= 0 && quality < DOWNLOAD_QUALITY_COUNT
        ? audio_quality_name((AudioQuality)quality) : NULL;
}

static void download_settings_save(void)
{
    DownloadSettings current = download_settings_get();
    char record[sizeof("codec=flac\nquality=hq\n")];
    int length = snprintf(record, sizeof(record), "codec=%s\nquality=%s\n",
                          download_codec_name(current.codec),
                          download_quality_name(current.quality));
    if (length > 0 && length < (int)sizeof(record)) {
        fs_write_deferred(DOWNLOAD_CFG_PATH, record, (size_t)length);
    }
}

void download_settings_set_codec(DownloadCodec codec)
{
    DownloadSettings current = download_settings_get();
    if (codec < 0 || codec >= DOWNLOAD_CODEC_COUNT || codec == current.codec) return;
    s_current = &kSettings[codec][current.quality];
    download_settings_save();
}

void download_settings_set_quality(DownloadQuality quality)
{
    DownloadSettings current = download_settings_get();
    if (quality < 0 || quality >= DOWNLOAD_QUALITY_COUNT || quality == current.quality) return;
    s_current = &kSettings[current.codec][quality];
    download_settings_save();
}

static int read_record(const char *path, char *record, size_t capacity)
{
    SceUID fd = fs_open(path, PSP_O_RDONLY, 0777);
    int count;
    if (fd < 0) return 0;
    count = fs_read(fd, record, capacity - 1);
    fs_close(fd);
    if (count <= 0) return -1;
    record[count] = '\0';
    return 1;
}

static int parse_record(char *record, DownloadSettings *settings, int legacy)
{
    char *line = record;
    int fields = 0;
    while (line && *line) {
        char *next = strchr(line, '\n');
        size_t length;
        if (next) *next++ = '\0';
        length = strlen(line);
        if (length && line[length - 1] == '\r') line[length - 1] = '\0';
        if (!legacy && strncmp(line, "codec=", sizeof("codec=") - 1) == 0) {
            int codec;
            for (codec = 0; codec < DOWNLOAD_CODEC_COUNT; codec++) {
                if (strcmp(line + sizeof("codec=") - 1, kCodecNames[codec]) == 0) {
                    settings->codec = (DownloadCodec)codec;
                    fields++;
                    break;
                }
            }
        } else if (strncmp(line, "quality=", sizeof("quality=") - 1) == 0) {
            const char *value = line + sizeof("quality=") - 1;
            int quality;
            for (quality = 0; quality < DOWNLOAD_QUALITY_COUNT; quality++) {
                if (strcmp(value, download_quality_name((DownloadQuality)quality)) == 0) {
                    settings->quality = (DownloadQuality)quality;
                    fields++;
                    break;
                }
            }
            if (legacy && strcmp(value, "mp3") == 0) {
                settings->codec = DOWNLOAD_CODEC_MP3;
                settings->quality = DOWNLOAD_QUALITY_NQ;
                fields++;
            } else if (legacy && strcmp(value, "flac") == 0) {
                settings->codec = DOWNLOAD_CODEC_FLAC;
                settings->quality = DOWNLOAD_QUALITY_HQ;
                fields++;
            }
        }
        line = next;
    }
    return fields;
}

void download_settings_init(void)
{
    DownloadSettings settings = kSettings[DOWNLOAD_CODEC_AAC][DOWNLOAD_QUALITY_NQ];
    char record[sizeof("codec=flac\r\nquality=hq\r\n")];
    int loaded = read_record(DOWNLOAD_CFG_PATH, record, sizeof(record));
    int migrate = 0;
    if (loaded > 0) {
        if (parse_record(record, &settings, 0) == 0) {
            logLine("download_settings: invalid config\n");
        }
    } else if (loaded == 0) {
        char legacy[192]; /* Capacity of the existing eq.cfg reader. */
        if (read_record(LEGACY_CFG_PATH, legacy, sizeof(legacy)) > 0) {
            migrate = parse_record(legacy, &settings, 1) > 0;
        }
    } else {
        logLine("download_settings: cannot read config\n");
    }
    s_current = &kSettings[settings.codec][settings.quality];
    if (migrate) download_settings_save();
    logLine("download_settings: codec=%s quality=%s\n",
            download_codec_name(settings.codec), download_quality_name(settings.quality));
}
