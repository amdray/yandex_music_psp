#ifndef YM_CORE_TIME_FORMAT_H
#define YM_CORE_TIME_FORMAT_H

#include <stddef.h>

typedef enum {
    YM_TIME_UTC,
    YM_TIME_LOCAL
} YmTimeFormat;

/* UTC: ISO8601 with .000Z; local: HH:MM in the console's time zone.
 * 0 success, -1 invalid arguments, unavailable time or conversion failure.
 * UTC requires 32 bytes, preserving the reporting timestamp contract. */
int ym_time_format(char *out, size_t size, YmTimeFormat format);

#endif
