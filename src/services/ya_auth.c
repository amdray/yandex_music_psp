// Перенос ya_auth.c из psp_yandex (логика один в один).
// Отличие только в транспорте: ym_https_post -> http_request.
// Документация: https://yandex.ru/dev/id/doc/ru/codes/screen-code-oauth
#include "services/ya_auth.h"

#include "services/net_http.h"
#include "services/ya_qr_auth.h"
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

// ===================================================================
// Вход по QR через Яндекс Паспорт (протокол — см. ya_qr_auth.h).
// Живёт в этом файле, чтобы не трогать список OBJS в Makefile.
// ===================================================================

#define QR_PASSPORT_HOST "passport.yandex.ru"
#define QR_PWL "https://" QR_PASSPORT_HOST "/pwl-yandex"
// Приложение «Яндекс» для Android (как в YandexStation): обмен Session_id
// на x_token разрешён только этому client_id.
#define QR_XTOKEN_CLIENT_ID "c0ebe342af7d48fbbbfcf2d2eedb8f9e"
#define QR_XTOKEN_CLIENT_SECRET "ad0a908f0aa341a182a37ecd75bc319e"

// Паспорт отдаёт веб-страницы; UA «PSP» выглядит как бот — берём
// нейтральный браузерный.
#define QR_UA "Mozilla/5.0 (PlayStation Portable; YMPSP) AppleWebKit/537.36"

#define QR_JSON_MAX 2048

static HttpCookieJar s_jar;
static char s_csrf[160];
static char s_auth_json[QR_JSON_MAX];   // ответ submit целиком (тело status)
static char s_track_id[96];

void ya_qr_reset(void)
{
    http_jar_clear(&s_jar);
    s_csrf[0] = '\0';
    s_auth_json[0] = '\0';
    s_track_id[0] = '\0';
}

static void qr_err(char *err, int err_sz, const char *fmt, int code)
{
    if (err && err_sz > 0) snprintf(err, (size_t)err_sz, fmt, code);
}

// Один запрос к паспорту с jar/CSRF. body_out — NUL-строка (обрезается).
// Возврат: HTTP-статус (>0) или код ошибки http_request (<0).
static int qr_call(HttpMethod method, const char *url, const char *ctype,
                   const char *payload, const char *extra_headers,
                   int big_body, char *body_out, int body_sz)
{
    NetHttpResponse hr;
    char hdrs[320];
    int r;

    memset(&hr, 0, sizeof(hr));
    if (s_csrf[0]) {
        snprintf(hdrs, sizeof(hdrs), "X-CSRF-Token: %s\r\n%s", s_csrf,
                 extra_headers ? extra_headers : "");
    } else {
        snprintf(hdrs, sizeof(hdrs), "%s", extra_headers ? extra_headers : "");
    }
    r = http_request(&(HttpRequest){
        .method = method,
        .url = url,
        .accept = big_body ? "text/html,*/*" : "application/json, */*",
        .content_type = ctype,
        .payload = payload,
        .payload_size = -1,
        .initial_buffer = big_body ? HTTP_TLS_BUFFER_MEDIUM : HTTP_TLS_BUFFER_SMALL,
        .extra_headers = hdrs,
        .user_agent = QR_UA,
        .no_client_header = 1,
        .cookie_jar = &s_jar,
    }, &hr);
    if (r != 0) {
        net_http_response_free(&hr);
        return r;
    }
    if (body_out && body_sz > 0) {
        int n = hr.body_size;
        if (!hr.body || n < 0) n = 0;
        if (n > body_sz - 1) n = body_sz - 1;
        if (n > 0) memcpy(body_out, hr.body, (size_t)n);
        body_out[n] = '\0';
    }
    r = hr.status_code;
    net_http_response_free(&hr);
    return r;
}

// Шаг 1: CSRF со страницы pwl-yandex. Страница ~130 КБ — читаем целиком
// и ищем маркер, не копируя всё в стек.
static int qr_fetch_csrf(void)
{
    NetHttpResponse hr;
    const char *p, *q;
    int r;
    size_t n;

    memset(&hr, 0, sizeof(hr));
    r = http_request(&(HttpRequest){
        .method = HTTP_GET,
        .url = QR_PWL,
        .accept = "text/html,*/*",
        .initial_buffer = HTTP_TLS_BUFFER_MEDIUM,
        .user_agent = QR_UA,
        .no_client_header = 1,
        .cookie_jar = &s_jar,
    }, &hr);
    if (r != 0) {
        net_http_response_free(&hr);
        logLine("qr: pwl get failed %d\n", r);
        return r;
    }
    logLine("qr: pwl status=%d len=%d cookies=%d\n",
            hr.status_code, hr.body_size, s_jar.count);
    r = -10;
    p = hr.body ? strstr(hr.body, "__CSRF__") : NULL;
    if (p) {
        p = strchr(p, '"');
        if (p) {
            p++;
            q = strchr(p, '"');
            if (q && q > p) {
                n = (size_t)(q - p);
                if (n < sizeof(s_csrf)) {
                    memcpy(s_csrf, p, n);
                    s_csrf[n] = '\0';
                    r = 0;
                }
            }
        }
    }
    net_http_response_free(&hr);
    if (r != 0) logLine("qr: csrf not found\n");
    return r;
}

int ya_qr_begin(char *link, int link_sz, char *err, int err_sz)
{
    static char resp[QR_JSON_MAX];
    char body[192];
    int st;

    if (!link || link_sz <= 1) return -1;
    link[0] = '\0';
    if (err && err_sz > 0) err[0] = '\0';
    ya_qr_reset();

    st = qr_fetch_csrf();
    if (st != 0) {
        qr_err(err, err_sz, "Паспорт недоступен (%d)", st);
        return st < 0 ? st : -1;
    }

    // Шаг 2: старт трека авторизации.
    st = qr_call(HTTP_POST, QR_PWL "/api/passport/auth/password/submit",
                 "application/json", "{\"retpath\":\"https://passport.yandex.ru/\"}",
                 NULL, 0, s_auth_json, sizeof(s_auth_json));
    if (st != 200 ||
        ya_json_field(s_auth_json, "track_id", s_track_id,
                      sizeof(s_track_id)) != 0) {
        logLine("qr: submit failed st=%d body=%.160s\n", st, s_auth_json);
        qr_err(err, err_sz, "Паспорт: нет трека (%d)", st);
        return -3;
    }
    logLine("qr: track ok\n");

    // Шаг 3: magic-ссылка для QR.
    snprintf(body, sizeof(body), "location_id=0&magic_track_id=%s&track_id=",
             s_track_id);
    st = qr_call(HTTP_POST, QR_PWL "/api/passport/auth/magic/code",
                 "application/x-www-form-urlencoded", body, NULL, 0,
                 resp, sizeof(resp));
    if (st != 200 || ya_json_field(resp, "link", link, link_sz) != 0 ||
        strncmp(link, "https://", 8) != 0) {
        logLine("qr: magic code failed st=%d body=%.160s\n", st, resp);
        link[0] = '\0';
        qr_err(err, err_sz, "Паспорт: нет QR (%d)", st);
        return -4;
    }
    logLine("qr: link ready len=%d\n", (int)strlen(link));
    logger_flush();
    return 0;
}

// Шаг 6: cookies сессии -> x_token.
static int qr_xtoken(char *xtoken, int xtoken_sz)
{
    static char resp[QR_JSON_MAX];
    char *cookies;
    char *hdrs;
    int cl, st, cap;
    NetHttpResponse hr;
    int r;

    cap = s_jar.count * (HTTP_JAR_NAME + HTTP_JAR_VALUE + 4) + 16;
    cookies = (char *)malloc((size_t)cap);
    hdrs = (char *)malloc((size_t)cap + 128);
    if (!cookies || !hdrs) {
        free(cookies);
        free(hdrs);
        return -1;
    }
    cl = http_jar_format(&s_jar, cookies, cap);
    if (cl <= 0) {
        free(cookies);
        free(hdrs);
        return -1;
    }
    snprintf(hdrs, (size_t)cap + 128,
             "Ya-Client-Host: " QR_PASSPORT_HOST "\r\nYa-Client-Cookie: %s\r\n",
             cookies);
    free(cookies);

    memset(&hr, 0, sizeof(hr));
    r = http_request(&(HttpRequest){
        .method = HTTP_POST,
        .url = "https://mobileproxy.passport.yandex.net/1/bundle/oauth/token_by_sessionid",
        .content_type = "application/x-www-form-urlencoded",
        .payload = "client_id=" QR_XTOKEN_CLIENT_ID
                   "&client_secret=" QR_XTOKEN_CLIENT_SECRET,
        .payload_size = -1,
        .initial_buffer = HTTP_TLS_BUFFER_SMALL,
        .extra_headers = hdrs,
        .no_client_header = 1,
    }, &hr);
    free(hdrs);
    if (r != 0) {
        net_http_response_free(&hr);
        logLine("qr: xtoken req failed %d\n", r);
        return r;
    }
    st = hr.status_code;
    resp[0] = '\0';
    if (hr.body && hr.body_size > 0) {
        int n = hr.body_size < (int)sizeof(resp) - 1 ? hr.body_size
                                                     : (int)sizeof(resp) - 1;
        memcpy(resp, hr.body, (size_t)n);
        resp[n] = '\0';
    }
    net_http_response_free(&hr);
    if (ya_json_field(resp, "access_token", xtoken, xtoken_sz) != 0) {
        logLine("qr: xtoken missing st=%d body=%.160s\n", st, resp);
        return -6;
    }
    logLine("qr: xtoken ok\n");
    return 0;
}

// Шаг 7: x_token -> токен Музыки (client_id Музыки из ya_auth.h).
static int qr_music_token(const char *xtoken, char *token, int token_sz)
{
    static char resp[QR_JSON_MAX];
    char body[512];
    NetHttpResponse hr;
    int r, st;

    snprintf(body, sizeof(body),
             "client_id=" YM_CLIENT_ID "&client_secret=" YM_CLIENT_SECRET
             "&grant_type=x-token&access_token=%s", xtoken);
    memset(&hr, 0, sizeof(hr));
    r = http_request(&(HttpRequest){
        .method = HTTP_POST,
        .url = "https://oauth.mobile.yandex.net/1/token",
        .content_type = "application/x-www-form-urlencoded",
        .payload = body,
        .payload_size = -1,
        .initial_buffer = HTTP_TLS_BUFFER_SMALL,
        .no_client_header = 1,
    }, &hr);
    memset(body, 0, sizeof(body));  // x_token не держим в стеке
    if (r != 0) {
        net_http_response_free(&hr);
        logLine("qr: music token req failed %d\n", r);
        return r;
    }
    st = hr.status_code;
    resp[0] = '\0';
    if (hr.body && hr.body_size > 0) {
        int n = hr.body_size < (int)sizeof(resp) - 1 ? hr.body_size
                                                     : (int)sizeof(resp) - 1;
        memcpy(resp, hr.body, (size_t)n);
        resp[n] = '\0';
    }
    net_http_response_free(&hr);
    if (ya_json_field(resp, "access_token", token, token_sz) != 0) {
        logLine("qr: music token missing st=%d\n", st);
        return -7;
    }
    logLine("qr: music token ok\n");
    return 0;
}

int ya_qr_poll(char *token, int token_sz, char *err, int err_sz)
{
    static char resp[QR_JSON_MAX];
    char state[48];
    char track2[96];
    char body[160];
    char xtoken[256];
    int st, r;

    if (!token || token_sz <= 1) return -1;
    if (!s_auth_json[0] || !s_csrf[0]) return -2;

    // Шаг 4: статус magic-ссылки.
    st = qr_call(HTTP_POST, QR_PWL "/api/passport/auth/magic/code/status",
                 "application/json", s_auth_json, NULL, 0, resp, sizeof(resp));
    if (st < 0) {
        qr_err(err, err_sz, "Ошибка сети (%d)", st);
        return st;
    }
    if (ya_json_field(resp, "state", state, sizeof(state)) != 0) {
        state[0] = '\0';
    }
    if (strcmp(state, "otp_auth_finished") != 0) {
        // Протухший/чужой трек: 4xx или ошибка про track.* в "errors".
        // Остальное (в т.ч. неизвестные состояния) — «ещё ждём»; экран сам
        // обновляет QR по таймеру.
        if ((st >= 400 && st != 429) ||
            (strstr(resp, "\"errors\"") && strstr(resp, "track"))) {
            logLine("qr: status st=%d body=%.160s\n", st, resp);
            qr_err(err, err_sz, "QR устарел, обновляю (%d)", st);
            return -2;
        }
        return 0;  // ещё не подтвердили
    }
    logLine("qr: confirmed on phone\n");

    // Шаг 5: выдать сессию (Session_id/sessionid2 в jar).
    if (ya_json_field(resp, "trackId", track2, sizeof(track2)) != 0) {
        snprintf(track2, sizeof(track2), "%s", s_track_id);
    }
    snprintf(body, sizeof(body), "track_id=%s", track2);
    st = qr_call(HTTP_POST, QR_PWL "/api/passport/sessions/get_session",
                 "application/x-www-form-urlencoded", body, NULL, 0,
                 resp, sizeof(resp));
    logLine("qr: get_session st=%d cookies=%d sid=%d\n", st, s_jar.count,
            http_jar_get(&s_jar, "Session_id") ? 1 : 0);
    if (st != 200 || !http_jar_get(&s_jar, "Session_id")) {
        qr_err(err, err_sz, "Паспорт: нет сессии (%d)", st);
        return -5;
    }

    // Шаги 6-7.
    r = qr_xtoken(xtoken, sizeof(xtoken));
    if (r != 0) {
        qr_err(err, err_sz, "Ошибка x_token (%d)", r);
        return r < 0 ? r : -6;
    }
    r = qr_music_token(xtoken, token, token_sz);
    memset(xtoken, 0, sizeof(xtoken));
    if (r != 0) {
        qr_err(err, err_sz, "Ошибка токена (%d)", r);
        return r < 0 ? r : -7;
    }
    ya_qr_reset();
    return 1;
}
