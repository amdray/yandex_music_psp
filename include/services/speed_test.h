#ifndef YM_SERVICES_SPEED_TEST_H
#define YM_SERVICES_SPEED_TEST_H

#include <stddef.h>

/* Фоновый тест скорости сети: качает ~10 с реальный файл с CDN приложения
 * (тот же TLS/хост, что и музыка) и пишет avg/peak KB/s в лог (speedtest:).
 * Запуск из настроек (X). Не блокирует вызывающий поток. */
void speed_test_request(void);

/* Строковый статус для UI-строки: "X: запустить" / "Идёт тест..." /
 * "123/456 KB/s" / "Нет трека" / "Ошибка". */
void speed_test_status(char *out, size_t out_size);

#endif /* YM_SERVICES_SPEED_TEST_H */
