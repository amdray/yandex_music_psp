#ifndef YM_SERVICES_LIBC_GUARD_H
#define YM_SERVICES_LIBC_GUARD_H

#include <stddef.h>
#include "cjson/cJSON.h"

// newlib на PSP (pspsdk/libcglue) живёт с ОДНИМ общим _impure_ptr на все
// потоки. Кэш больших чисел внутри него (_Balloc/_Bfree freelist), который
// используют strtod (парсинг чисел в cJSON) и dtoa (printf %f/%g), НЕ
// защищён блокировкой: вытеснение посреди операции другим потоком портит
// список -> Invalid Memory Access в _Balloc (ловилось в «Сейчас играет»
// на snprintf("%.1f kHz") при параллельном парсинге JSON воркерами).
//
// Правила проекта:
//  - НЕ использовать %f/%g/%e в printf-семействе (форматировать целыми,
//    см. ym_fmt_fixed);
//  - cJSON_Parse*/strtod — только через обёртки ниже (общий мьютекс).
// Реализация — в api_parser.c (Makefile защищён, новый .o не добавить).

void ym_libc_guard_init(void);   // вызвать в main() до старта потоков
void ym_libc_lock(void);
void ym_libc_unlock(void);

cJSON *ym_json_parse(const char *text);
cJSON *ym_json_parse_len(const char *text, size_t len);
double ym_strtod(const char *s, char **end);

// v с decimals (0..6) знаками после точки, без printf-float. Возврат как snprintf.
int ym_fmt_fixed(char *out, size_t out_size, double v, int decimals);

#endif
