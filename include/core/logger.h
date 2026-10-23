#ifndef YM_CORE_LOGGER_H
#define YM_CORE_LOGGER_H

#include <psptypes.h>
#include <pspiofilemgr.h>

void logger_init(const char *path);
void logger_shutdown(void);
void logger_flush(void);      /* лениво: см. logger.c (запись по порогу/таймеру) */
void logger_flush_now(void);  /* принудительно записать и sync (сон/питание) */
void logger_set_realtime_mode(int enabled);

/* Логи можно отключить в настройках (config/logs.cfg, "logs=0/1"): при
 * выключенных logLine() сразу возвращается — ни форматирования, ни записи на
 * карту. Состояние читается из конфига при logger_init(). */
int logger_enabled(void);
void logger_set_enabled(int enabled);
void logLine(const char *fmt, ...);
/* Контрольная точка: строка в лог + немедленная запись на карту (переживёт вылет/выключение). */
void logger_checkpoint(const char *fmt, ...);

/* Отладочные дампы JSON-ответов API в data/logs/<имя>_response.json.
 * 0 по умолчанию: на реальной PSP с медленной картой каждый дамп — секунды
 * записи под общим мьютексом fs (блокировал авторизацию/плейлисты/аудио).
 * Нигде не читаются; включать только для отладки парсера. */
#ifndef YM_DEBUG_RESPONSE_DUMPS
#define YM_DEBUG_RESPONSE_DUMPS 0
#endif

#define LOG_FS(fmt, ...) logLine("fs: " fmt, ##__VA_ARGS__)

#endif
