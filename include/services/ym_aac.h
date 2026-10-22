#ifndef YM_SERVICES_YM_AAC_H
#define YM_SERVICES_YM_AAC_H

#include <stddef.h>
#include <stdint.h>

/* AAC-декодер поверх faad2 (GPLv2, vendor/faad2-master) + выборка сэмплов
 * из контейнера MP4 (services/mp4_track.h). Сервер отдаёт aac-mp4 /
 * he-aac-mp4 (esds ASC), поэтому контейнер demux-ится на лету: каждый
 * сэмпл mp4a — сырой AAC access unit, декодер конфигурируется ASC через
 * NeAACDecInit2. Файл на диске остаётся MP4 (<id>.aac).
 *
 * Всегда выдаёт 44100/stereo s16 (SBR-удвоение даёт faad, моно поднимается
 * до стерео, иные частоты ресемплятся) — под SRC/EQ-путь плеера.
 * Источник — открытый файл (fd, не закрывается) + путь для разбора moov.
 * Данные за пределами скачанного префикса возвращаются как "нужны ещё"
 * (avail-колбэк, как у ym_flac). Один декодер за раз (статический). */

typedef struct YmAac YmAac;

typedef struct {
    int sample_rate;    /* всегда 44100 */
    int channels;       /* всегда 2 */
    int64_t total_frames;
} YmAacFormat;

/* Длина доступного префикса файла (для недокачанного — downloaded). */
typedef int64_t (*YmAacAvailFn)(void *user);

/* fd — указатель на fd владельца (закроет его сам): открытый на PSP fd
 * не видит роста файла во время докачки, поэтому декодер переоткрывает
 * его через этот указатель; path — тот же файл. */
YmAac *ym_aac_open_file(int *fd, const char *path,
                        YmAacAvailFn avail, void *avail_user);

/* 0 ok, <0 ошибка. Формат известен сразу после open (ASC + stts). */
int ym_aac_format(YmAac *a, YmAacFormat *out);

/* Декодирует до frames фреймов (interleaved s16, 44100/stereo). Возвращает
 * реально выданные фреймы (0 = данные ещё качаются или конец файла;
 * <0 = фатальная ошибка). */
int64_t ym_aac_read_s16(YmAac *a, short *pcm, int frames);

/* 0 ok, -1 нужны ещё данные (сэмпл цели за скачанным префиксом),
 * -2 неустранимо. Состояние SBR/PS сбрасывается (пере-init по ASC). */
int ym_aac_seek_frame(YmAac *a, int64_t frame);

/* 1 = все сэмплы отданы (got==0 здесь = настоящий EOF, а не буфер). */
int ym_aac_at_end(YmAac *a);

void ym_aac_close(YmAac *a);

#endif /* YM_SERVICES_YM_AAC_H */
