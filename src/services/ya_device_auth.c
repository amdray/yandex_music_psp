#include "services/ya_device_auth.h"
#include "services/ya_oauth_client.h"
#include "core/json.h"
#include "services/net_http.h"
#include "core/logger.h"

#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YM_OAUTH_HOST "oauth.yandex.ru"
#define AUTH_BUF 2048

void ya_device_id(char *out, int out_sz) {
    /* стабильный ID приставки: вызвать один раз и положить в config */
    snprintf(out, out_sz, "PSP-%08X",
             (unsigned)sceKernelGetSystemTimeLow());
}

/* Свои данные приложения; по умолчанию — встроенный публичный ID. */
struct YaDeviceAuth {
    char client_id[48];
    char client_secret[80];
    char response[AUTH_BUF];
};

YaDeviceAuth *ya_device_create(void)
{
    YaDeviceAuth *ctx = calloc(1, sizeof(*ctx));
    if (ctx) {
        memcpy(ctx->client_id, YM_CLIENT_ID, sizeof(YM_CLIENT_ID));
        memcpy(ctx->client_secret, YM_CLIENT_SECRET, sizeof(YM_CLIENT_SECRET));
    }
    return ctx;
}

void ya_device_destroy(YaDeviceAuth *ctx)
{
    if (!ctx) return;
    volatile unsigned char *p = (volatile unsigned char *)ctx;
    for (size_t i = 0; i < sizeof(*ctx); ++i) p[i] = 0;
    free(ctx);
}

void ya_oauth_set(YaDeviceAuth *ctx, const char *client_id, const char *client_secret) {
    if (!ctx) return;
    /* Пустые и мусорные (пробелы/\r от CRLF-правок) значения игнорируем,
     * иначе затрём встроенный ключ и сервер ответит invalid_client. */
    if (client_id) {
        while (*client_id == ' ' || *client_id == '\t' ||
               *client_id == '\r' || *client_id == '\n')
            client_id++;
        if (client_id[0]) {
            strncpy(ctx->client_id, client_id, sizeof(ctx->client_id) - 1);
            ctx->client_id[sizeof(ctx->client_id) - 1] = 0;
        }
    }
    if (client_secret) {
        while (*client_secret == ' ' || *client_secret == '\t' ||
               *client_secret == '\r' || *client_secret == '\n')
            client_secret++;
        if (client_secret[0]) {
            strncpy(ctx->client_secret, client_secret, sizeof(ctx->client_secret) - 1);
            ctx->client_secret[sizeof(ctx->client_secret) - 1] = 0;
        }
    }
}

// Один form-POST на oauth-хост. Возврат 0 = есть тело в resp (NUL),
// иначе код ошибки http_request (<0). Не-200 тоже отдаём телом:
// ошибки OAuth приходят JSON-ом с полем "error".
static int yauth_post(const char *path, const char *body,
                      char *resp, int resp_sz) {
    NetHttpResponse hr;
    char url[160];
    int r;

    if (!path || !body || !resp || resp_sz <= 1) return -1;
    memset(&hr, 0, sizeof(hr));
    snprintf(url, sizeof(url), "https://%s%s", YM_OAUTH_HOST, path);
    r = http_request(&(HttpRequest){
        .method = HTTP_POST,
        .url = url,
        .content_type = "application/x-www-form-urlencoded",
        .payload = body,
        .payload_size = -1,
        .initial_buffer = HTTP_TLS_BUFFER_SMALL,
    }, &hr);
    if (r != 0) {
        net_http_response_free(&hr);
        return r;
    }
    if (hr.body && hr.body_size > 0) {
        int n = hr.body_size;
        if (n > resp_sz - 1) n = resp_sz - 1;
        memcpy(resp, hr.body, (size_t)n);
        resp[n] = 0;
    } else {
        resp[0] = 0;
    }
    net_http_response_free(&hr);
    return 0;
}

int ya_device_begin(YaDeviceAuth *ctx, const char *device_id, ya_device_codes *out) {
    char *resp;
    char body[256];
    int r;
    cJSON *root, *field;
    if (!ctx || !device_id || !out) return -1;
    resp = ctx->response;
    memset(out, 0, sizeof(*out));
    snprintf(body, sizeof(body),
             "client_id=%s&device_id=%s&device_name=PSP-YandexMusic",
             ctx->client_id, device_id);
    r = yauth_post("/device/code", body, resp, sizeof(ctx->response));
    if (r != 0) {
        /* один ретрай (в оригинале — с новой сессией) */
        r = yauth_post("/device/code", body, resp, sizeof(ctx->response));
        if (r != 0) return r;
    }
    root = ym_json_parse(resp, NULL);
    if (ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "device_code"), out->device_code,
                      sizeof(out->device_code)) != 0 ||
        ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "user_code"), out->user_code,
                      sizeof(out->user_code)) != 0) {
        /* кодов нет — запомним текст ошибки сервера для экрана */
        if (ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "error"), out->err, sizeof(out->err)) != 0)
            out->err[0] = 0;
        cJSON_Delete(root);
        return -3;
    }
    if (ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "verification_url"), out->verify_url,
                      sizeof(out->verify_url)) != 0)
        strncpy(out->verify_url, "https://oauth.yandex.com/device",
                sizeof(out->verify_url) - 1);
    out->interval = 5;
    out->expires_in = 300;
    field = cJSON_GetObjectItemCaseSensitive(root, "interval");
    if (cJSON_IsNumber(field) && field->valuedouble == (double)field->valueint)
        out->interval = field->valueint;
    field = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    if (cJSON_IsNumber(field) && field->valuedouble == (double)field->valueint)
        out->expires_in = field->valueint;
    cJSON_Delete(root);
    logLine("yauth: begin ok: %s\n", out->user_code);
    logger_flush();
    return 0;
}

int ya_device_poll(YaDeviceAuth *ctx, const char *device_code, char *token, int token_sz) {
    char *resp;
    char body[384];
    int r;
    char errb[64];
    cJSON *root;
    if (!ctx || !device_code || !token || token_sz <= 1) return -1;
    resp = ctx->response;
    /* каждый опрос — отдельным запросом (в оригинале — свежая сессия:
     * полузакрытые переиспользованные висят на некоторых стеках) */
    logLine("yauth: poll try\n");
    logger_flush();
    snprintf(body, sizeof(body), "grant_type=device_code&code=%s&client_id=%s",
             device_code, ctx->client_id);
    if (ctx->client_secret[0]) {
        /* секрет — в тело (доки разрешают; иначе нужен Basic-заголовок) */
        size_t n = strlen(body);
        snprintf(body + n, sizeof(body) - n, "&client_secret=%s", ctx->client_secret);
    }
    r = yauth_post("/token", body, resp, sizeof(ctx->response));
    logLine("yauth: poll post -> %d\n", r);
    logger_flush();
    if (r != 0) return r;
    root = ym_json_parse(resp, NULL);
    if (ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "access_token"), token, token_sz) == 0) {
        cJSON_Delete(root);
        logLine("yauth: poll: TOKEN OK\n");
        logger_flush();
        return 1; /* есть токен! */
    }
    if (ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "error"), errb, sizeof(errb)) == 0) {
        cJSON_Delete(root);
        logLine("yauth: poll: %s\n", errb);
        logger_flush();
        if (!strcmp(errb, "authorization_pending")) return 0;
        /* authorization_denied / expired_token / invalid_grant — заново */
        return -2;
    }
    cJSON_Delete(root);
    return -1;
}

