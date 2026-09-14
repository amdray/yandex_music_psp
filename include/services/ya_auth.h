#ifndef YM_SERVICES_YA_AUTH_H
#define YM_SERVICES_YA_AUTH_H

// Device Flow Яндекса (перенос ya_auth из psp_yandex без изменений логики):
//  1. PSP просит пару кодов: POST oauth.yandex.ru/device/code
//  2. На экране PSP: "Введи код XXXX-XXXX на oauth.yandex.ru/device"
//  3. Пользователь вводит КОРОТКИЙ код на телефоне и жмёт Разрешить
//  4. PSP сама опрашивает oauth.yandex.ru/token и забирает токен
//
// Никаких длинных токенов через OSK.
// Документация: https://yandex.ru/dev/id/doc/ru/codes/screen-code-oauth

#define YM_OAUTH_HOST "oauth.yandex.ru"
// Данные Android-приложения Яндекс.Музыки (как в yandex-music-api):
// device flow официально разрешён для этого client_id.
#define YM_CLIENT_ID "23cabbbdc6cd418abb4b39c32c41195d"
#define YM_CLIENT_SECRET "53bc75238f0c4d08a118e51fe9203300"

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
void ya_oauth_set(const char *client_id, const char *client_secret);

// Шаг 1: запросить пару кодов. Возврат 0 = ок.
int ya_device_begin(const char *device_id, ya_device_codes *out);

// Шаг 2: опросить токен. Возврат: 1 = токен получен (в token),
// 0 = ещё не подтвердили (подожди interval и повтори),
// <0 = ошибка/протух (начни заново).
int ya_device_poll(const char *device_code, char *token, int token_sz);

// Сгенерировать/прочитать device_id (6-50 ASCII, стабильный на приставке).
// Вызвать один раз и положить в config. Хранится в out (sz>=32).
void ya_device_id(char *out, int out_sz);

// Извлечь строковое поле из плоского JSON {"k":"v"} (без jsmn, ASCII).
// Возврат 0 = найдено.
int ya_json_field(const char *json, const char *key, char *out, int out_sz);

#endif
