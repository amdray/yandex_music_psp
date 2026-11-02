#ifndef YM_SERVICES_YA_DEVICE_AUTH_H
#define YM_SERVICES_YA_DEVICE_AUTH_H

/* Device-code protocol. One context per login attempt, used by its worker. */
typedef struct YaDeviceAuth YaDeviceAuth;
YaDeviceAuth *ya_device_create(void);
void ya_device_destroy(YaDeviceAuth *ctx);

typedef struct {
    char device_code[96];
    char user_code[24];
    char verify_url[96];
    int interval;    // пауза между опросами, сек
    int expires_in;  // время жизни кодов, сек
    char err[48];    // короткая ошибка сервера ("invalid_client")
} ya_device_codes;

// Свои OAuth-данные (из https://oauth.yandex.ru/client/new).
// Пустые строки = значения по умолчанию.
void ya_oauth_set(YaDeviceAuth *ctx, const char *client_id, const char *client_secret);

// Шаг 1: запросить пару кодов. Возврат 0 = ок.
int ya_device_begin(YaDeviceAuth *ctx, const char *device_id, ya_device_codes *out);

// Шаг 2: опросить токен. Возврат: 1 = токен получен (в token),
// 0 = ещё не подтвердили (подожди interval и повтори),
// <0 = ошибка/протух (начни заново).
int ya_device_poll(YaDeviceAuth *ctx, const char *device_code, char *token, int token_sz);

// Сгенерировать/прочитать device_id (6-50 ASCII, стабильный на приставке).
// Вызвать один раз и положить в config. Хранится в out (sz>=32).
void ya_device_id(char *out, int out_sz);

#endif
