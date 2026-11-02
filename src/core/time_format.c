/* Файл зафиксирован и проверен. */
#include "core/time_format.h"

#include <stdio.h>
#include <time.h>

int ym_time_format(char *out, size_t size, YmTimeFormat format)
{
    struct tm parts;
    time_t now;
    int written;

    if (!out || (format != YM_TIME_UTC && format != YM_TIME_LOCAL) ||
        size < (format == YM_TIME_UTC ? 32 : sizeof("00:00"))) {
        return -1;
    }
    now = time(NULL);
    if (format == YM_TIME_LOCAL) {
        if (!localtime_r(&now, &parts)) return -1;
        written = snprintf(out, size, "%02d:%02d", parts.tm_hour, parts.tm_min);
    } else {
        if (now <= 0 || !gmtime_r(&now, &parts)) {
            return -1;
        }
        written = snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                           parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                           parts.tm_hour, parts.tm_min, parts.tm_sec);
    }
    return written > 0 && (size_t)written < size ? 0 : -1;
}
