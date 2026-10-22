#ifndef YM_SERVICES_MP4_TRACK_H
#define YM_SERVICES_MP4_TRACK_H

#include <stdint.h>

/* Разбор MP4 (ISOBMFF) с одним аудио-треком: flac-mp4 (fLaC-сэмплы) или
 * aac-mp4 (mp4a/esds). Используется тремя путями:
 *  - проба заголовка потока перед закачкой (mp4_probe_buffer);
 *  - демукс flac-mp4 -> сырой fLaC при загрузке (open + выборка сэмплов);
 *  - путь плеера для aac-mp4 (esds ASC + сэмплы -> faad2).
 * moov разбирается целиком в RAM (у трековых файлов ~10 КБ). */

typedef struct Mp4Track Mp4Track;

typedef enum {
    MP4_PROBE_NONE = 0,
    MP4_PROBE_FLAC = 1, /* fLaC-сэмплы + dfLa (STREAMINFO) */
    MP4_PROBE_AAC = 2   /* mp4a + esds (AudioSpecificConfig) */
} Mp4ProbeKind;

typedef struct {
    Mp4ProbeKind kind;
    int rate; /* flac: из STREAMINFO */
    int ch;
    int bps;
} Mp4ProbeInfo;

/* Проба первых КБ потока: 0 = распознано, <0 = не MP4/нужного типа.
 * Таблицы сэмплов не требуются — валидируется путь до dfLa/esds. */
int mp4_probe_buffer(const unsigned char *buf, int len, Mp4ProbeInfo *out);

/* Открыть файл, разобрать moov и таблицы сэмплов. NULL при ошибке. */
Mp4Track *mp4_track_open(const char *path);
void mp4_track_close(Mp4Track *t);

int mp4_track_is_flac(const Mp4Track *t);
int mp4_track_is_aac(const Mp4Track *t);

/* FLAC: готовый заголовок сырого fLaC ("fLaC" + метаблоки из dfLa). */
int mp4_track_flac_header(const Mp4Track *t, unsigned char *out, int cap);
int mp4_track_flac_format(const Mp4Track *t, int *rate, int *ch, int *bps);

/* AAC: AudioSpecificConfig из esds (для NeAACDecInit2). */
const unsigned char *mp4_track_asc(const Mp4Track *t, int *len);

int mp4_track_sample_count(const Mp4Track *t);

/* Сэмпл idx -> абсолютное смещение в файле и размер. Внутренний курсор
 * ускоряет последовательное чтение; случайный доступ пересчитывает.
 * 0 = ок; -1 = битая таблица/индекс; -2 = за пределами известного файла
 * (прогрессивная докачка) — off/size заполнены, курсор обновлён. */
int mp4_track_sample_at(Mp4Track *t, int idx, int64_t *out_off, int *out_size);

/* Связь позиции декодера (pcm-фреймы на выходной частоте) с номером
 * сэмпла через stts/mdhd. Для HE-AAC выход может быть вдвое выше
 * частоты трека — маппинг чисто временной. */
int mp4_track_sample_for_pcm(const Mp4Track *t, int64_t pcm, int rate);
int64_t mp4_track_pcm_for_sample(const Mp4Track *t, int idx, int rate);

/* Полная длительность трека в pcm-фреймах на частоте rate (stts). */
int64_t mp4_track_total_pcm(const Mp4Track *t, int rate);

#endif
