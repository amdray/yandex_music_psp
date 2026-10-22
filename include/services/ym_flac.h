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

/* Ожидание фронтира докачки прямо в читателе: возвращается, пока
 * need_bytes не станет доступно. 0 — повторить чтение, <0 — прервать
 * (остановка/ошибка трека). Без этого dr_flac получал 0 посреди трека,
 * ловил слепок «конец потока» и уходил в ложный EOF -> скип трека. */
typedef int (*YmFlacWaitFn)(void *user, int64_t need_bytes);

YmFlac *ym_flac_open_ram(AudioStreamBuf *sbuf, YmFlacAvailFn avail, void *avail_user);
/* fd — указатель на fd владельца (закроет его сам): открытый на PSP fd
 * не видит роста файла во время докачки, поэтому декодер переоткрывает
 * его через этот указатель; path — путь файла (нужен и RAM read-ahead
 * на отдельном fd). */
YmFlac *ym_flac_open_file(int *fd, const char *path,
                          YmFlacAvailFn avail, void *avail_user);
/* flac-mp4 без демукса: виртуальный поток = заголовок fLaC из dfLa,
 * затем полезные нагрузки сэмплов mp4 (те же байты, что писал бы
 * демукс). Читается напрямую из частично закачанного файла: готовность
 * сэмпла сверяется с avail (downloaded), недокачанное = пауза чтения. */
YmFlac *ym_flac_open_mp4(int *fd, const char *path,
                         YmFlacAvailFn avail, void *avail_user);

/* Установить колбэк ожидания данных (вызывается из потока декодера). */
void ym_flac_set_wait(YmFlac *f, YmFlacWaitFn wait, void *wait_user);

/* 1 = декодер у реального конца трека (got==0 здесь = настоящий EOF). */
int ym_flac_at_end(YmFlac *f);

/* 0 ok, <0 ошибка/не FLAC. Формат — только чтение заголовка, без декода. */
int ym_flac_format(YmFlac *f, YmFlacFormat *out);

/* Декодирует до frames сэмплов (interleaved s16). Возвращает реально
 * прочитанные фреймы (меньше = нужны ещё данные (стрим) или конец). */
int64_t ym_flac_read_s16(YmFlac *f, short *pcm, int frames);

/* 0 ok, -1 нужны ещё данные, -2 неустранимо. */
int ym_flac_seek_frame(YmFlac *f, int64_t frame);

void ym_flac_close(YmFlac *f);

#endif /* YM_SERVICES_YM_FLAC_H */
