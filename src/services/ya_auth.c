// Перенос ya_auth.c из psp_yandex (логика один в один).
// Отличие только в транспорте: ym_https_post -> http_request.
// Документация: https://yandex.ru/dev/id/doc/ru/codes/screen-code-oauth
#include "services/ya_auth.h"

#include "services/net_http.h"
#include "core/logger.h"

#include <pspkernel.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define AUTH_BUF 2048

int ya_json_field(const char *json, const char *key, char *out, int out_sz) {
    char pat[64], *p;
    int n = 0;
    if (!json || !key || out_sz <= 1) return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < out_sz) {
            if (*p == '\\' && p[1]) {
                /* токены/коды — ASCII, escapes не встречаются; берём как есть */
                p++;
            }
            out[n++] = *p++;
        }
        if (*p != '"') return -1;
        out[n] = 0;
        return 0;
    }
    /* число */
    while (((*p >= '0' && *p <= '9') || *p == '-') && n + 1 < out_sz)
        out[n++] = *p++;
    if (!n) return -1;
    out[n] = 0;
    return 0;
}

void ya_device_id(char *out, int out_sz) {
    /* стабильный ID приставки: вызвать один раз и положить в config */
    snprintf(out, out_sz, "PSP-%08X",
             (unsigned)sceKernelGetSystemTimeLow());
}

/* Свои данные приложения; по умолчанию — встроенный публичный ID. */
static char g_cid[48] = YM_CLIENT_ID;
static char g_csec[80] = YM_CLIENT_SECRET;

void ya_oauth_set(const char *client_id, const char *client_secret) {
    /* Пустые и мусорные (пробелы/\r от CRLF-правок) значения игнорируем,
     * иначе затрём встроенный ключ и сервер ответит invalid_client. */
    if (client_id) {
        while (*client_id == ' ' || *client_id == '\t' ||
               *client_id == '\r' || *client_id == '\n')
            client_id++;
        if (client_id[0]) {
            strncpy(g_cid, client_id, sizeof(g_cid) - 1);
            g_cid[sizeof(g_cid) - 1] = 0;
        }
    }
    if (client_secret) {
        while (*client_secret == ' ' || *client_secret == '\t' ||
               *client_secret == '\r' || *client_secret == '\n')
            client_secret++;
        if (client_secret[0]) {
            strncpy(g_csec, client_secret, sizeof(g_csec) - 1);
            g_csec[sizeof(g_csec) - 1] = 0;
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

int ya_device_begin(const char *device_id, ya_device_codes *out) {
    static char resp[AUTH_BUF];
    char body[256];
    int r;
    char tmp[32];
    if (!device_id || !out) return -1;
    memset(out, 0, sizeof(*out));
    snprintf(body, sizeof(body),
             "client_id=%s&device_id=%s&device_name=PSP-YandexMusic",
             g_cid, device_id);
    r = yauth_post("/device/code", body, resp, sizeof(resp));
    if (r != 0) {
        /* один ретрай (в оригинале — с новой сессией) */
        r = yauth_post("/device/code", body, resp, sizeof(resp));
        if (r != 0) return r;
    }
    if (ya_json_field(resp, "device_code", out->device_code,
                      sizeof(out->device_code)) != 0 ||
        ya_json_field(resp, "user_code", out->user_code,
                      sizeof(out->user_code)) != 0) {
        /* кодов нет — запомним текст ошибки сервера для экрана */
        if (ya_json_field(resp, "error", out->err, sizeof(out->err)) != 0)
            out->err[0] = 0;
        return -3;
    }
    if (ya_json_field(resp, "verification_url", out->verify_url,
                      sizeof(out->verify_url)) != 0)
        strncpy(out->verify_url, "https://oauth.yandex.com/device",
                sizeof(out->verify_url) - 1);
    out->interval = 5;
    out->expires_in = 300;
    if (ya_json_field(resp, "interval", tmp, sizeof(tmp)) == 0)
        out->interval = atoi(tmp);
    if (ya_json_field(resp, "expires_in", tmp, sizeof(tmp)) == 0)
        out->expires_in = atoi(tmp);
    logLine("yauth: begin ok: %s\n", out->user_code);
    logger_flush();
    return 0;
}

int ya_device_poll(const char *device_code, char *token, int token_sz) {
    static char resp[AUTH_BUF];
    char body[384];
    int r;
    char errb[64];
    if (!device_code || !token) return -1;
    /* каждый опрос — отдельным запросом (в оригинале — свежая сессия:
     * полузакрытые переиспользованные висят на некоторых стеках) */
    logLine("yauth: poll try\n");
    logger_flush();
    snprintf(body, sizeof(body), "grant_type=device_code&code=%s&client_id=%s",
             device_code, g_cid);
    if (g_csec[0]) {
        /* секрет — в тело (доки разрешают; иначе нужен Basic-заголовок) */
        size_t n = strlen(body);
        snprintf(body + n, sizeof(body) - n, "&client_secret=%s", g_csec);
    }
    r = yauth_post("/token", body, resp, sizeof(resp));
    logLine("yauth: poll post -> %d\n", r);
    logger_flush();
    if (r != 0) return r;
    if (ya_json_field(resp, "access_token", token, token_sz) == 0) {
        logLine("yauth: poll: TOKEN OK\n");
        logger_flush();
        return 1; /* есть токен! */
    }
    if (ya_json_field(resp, "error", errb, sizeof(errb)) == 0) {
        logLine("yauth: poll: %s\n", errb);
        logger_flush();
        if (!strcmp(errb, "authorization_pending")) return 0;
        /* authorization_denied / expired_token / invalid_grant — заново */
        return -2;
    }
    return -1;
}
