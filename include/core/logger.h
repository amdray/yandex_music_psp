#ifndef YM_CORE_LOGGER_H
#define YM_CORE_LOGGER_H

#include <psptypes.h>
#include <pspiofilemgr.h>

void logger_init(const char *path);
void logger_shutdown(void);
void logger_flush(void);
void logger_set_realtime_mode(int enabled);
void logLine(const char *fmt, ...);

#define LOG_FS(fmt, ...) logLine("fs: " fmt, ##__VA_ARGS__)

#endif
