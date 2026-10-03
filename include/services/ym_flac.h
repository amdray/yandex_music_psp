#ifndef YM_SERVICES_YM_FLAC_H
#define YM_SERVICES_YM_FLAC_H

#include <stddef.h>
#include <stdint.h>

#include "services/audio_stream_buf.h"

/* FLAC-декодер поверх dr_flac (public domain, src/services/dr_flac.h).
 * PSP не умеет FLAC железом (sceMp3 только MP3), поэтому софт-декод в PCM.
 * По мануалу PSP: только стерео 16 бит / 44.1 кГц — иное отклоняется
 * вызывателем (fallback на MP3), здесь только транспорт+декод.
 *
 * Источник — либо живой RAM-буфер (asb), либо открытый файл (fd) с
 * известной полной длиной; позиция EOF для недокачанного — текущий
 * downloaded (передаётся колбэком снаружи через геттер).
 * Одновременно открыт не более одного декодера (статический пул, без malloc).
 */

typedef struct YmFlac YmFlac;

typedef struct {
    int sample_rate;
    int channels;
    int bits_per_sample;
    int64_t total_frames; /* 0, если STREAMINFO без total (стрим) */
} YmFlacFormat;

/* Длина доступного префикса (для недокачанного — текущий downloaded). */
typedef int64_t (*YmFlacAvailFn)(void *user);

YmFlac *ym_flac_open_ram(AudioStreamBuf *sbuf, YmFlacAvailFn avail, void *avail_user);
YmFlac *ym_flac_open_file(int fd, int64_t content_length,
                          YmFlacAvailFn avail, void *avail_user);

/* 0 ok, <0 ошибка/не FLAC. Формат — только чтение заголовка, без декода. */
int ym_flac_format(YmFlac *f, YmFlacFormat *out);

/* Декодирует до frames сэмплов (interleaved s16). Возвращает реально
 * прочитанные фреймы (меньше = нужны ещё данные (стрим) или конец). */
int64_t ym_flac_read_s16(YmFlac *f, short *pcm, int frames);

/* 0 ok, -1 нужны ещё данные, -2 неустранимо. */
int ym_flac_seek_frame(YmFlac *f, int64_t frame);

void ym_flac_close(YmFlac *f);

#endif /* YM_SERVICES_YM_FLAC_H */
