#include "services/ya_qr_auth.h"
#include "services/ya_oauth_client.h"
#include "core/json.h"
#include "services/net_http.h"
#include "core/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct YaQrAuth {
    HttpCookieJar jar;
    char csrf[160];
    char auth_json[QR_JSON_MAX];
    char track_id[96];
    char response[QR_JSON_MAX];
};

YaQrAuth *ya_qr_create(void)
{
    return calloc(1, sizeof(YaQrAuth));
}

void ya_qr_reset(YaQrAuth *ctx)
{
    if (!ctx) return;
    volatile unsigned char *p = (volatile unsigned char *)ctx;
    for (size_t i = 0; i < sizeof(*ctx); ++i) p[i] = 0;
}

void ya_qr_destroy(YaQrAuth *ctx)
{
    ya_qr_reset(ctx);
    free(ctx);
}

static void qr_err(char *err, int err_sz, const char *fmt, int code)
{
    if (err && err_sz > 0) snprintf(err, (size_t)err_sz, fmt, code);
}

// Один запрос к паспорту с jar/CSRF. body_out — NUL-строка (обрезается).
// Возврат: HTTP-статус (>0) или код ошибки http_request (<0).
static int qr_call(YaQrAuth *ctx, HttpMethod method, const char *url, const char *ctype,
                   const char *payload, const char *extra_headers,
                   int big_body, char *body_out, int body_sz)
{
    NetHttpResponse hr;
    char hdrs[320];
    int r;

    memset(&hr, 0, sizeof(hr));
    if (ctx->csrf[0]) {
        snprintf(hdrs, sizeof(hdrs), "X-CSRF-Token: %s\r\n%s", ctx->csrf,
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
        .cookie_jar = &ctx->jar,
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
static int qr_fetch_csrf(YaQrAuth *ctx)
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
        .cookie_jar = &ctx->jar,
    }, &hr);
    if (r != 0) {
        net_http_response_free(&hr);
        logLine("qr: pwl get failed %d\n", r);
        return r;
    }
    logLine("qr: pwl status=%d len=%d cookies=%d\n",
            hr.status_code, hr.body_size, ctx->jar.count);
    r = -10;
    p = hr.body ? strstr(hr.body, "__CSRF__") : NULL;
    if (p) {
        p = strchr(p, '"');
        if (p) {
            p++;
            q = strchr(p, '"');
            if (q && q > p) {
                n = (size_t)(q - p);
                if (n < sizeof(ctx->csrf)) {
                    memcpy(ctx->csrf, p, n);
                    ctx->csrf[n] = '\0';
                    r = 0;
                }
            }
        }
    }
    net_http_response_free(&hr);
    if (r != 0) logLine("qr: csrf not found\n");
    return r;
}

int ya_qr_begin(YaQrAuth *ctx, char *link, int link_sz, char *err, int err_sz)
{
    if (!ctx) return -1;
    char *resp = ctx->response;
    char body[192];
    int st, parsed;
    cJSON *root;

    if (!link || link_sz <= 1) return -1;
    link[0] = '\0';
    if (err && err_sz > 0) err[0] = '\0';
    ya_qr_reset(ctx);

    st = qr_fetch_csrf(ctx);
    if (st != 0) {
        qr_err(err, err_sz, "Паспорт недоступен (%d)", st);
        return st < 0 ? st : -1;
    }

    // Шаг 2: старт трека авторизации.
    st = qr_call(ctx, HTTP_POST, QR_PWL "/api/passport/auth/password/submit",
                 "application/json", "{\"retpath\":\"https://passport.yandex.ru/\"}",
                 NULL, 0, ctx->auth_json, sizeof(ctx->auth_json));
    root = st == 200 ? ym_json_parse(ctx->auth_json, NULL) : NULL;
    parsed = ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "track_id"),
                                 ctx->track_id, sizeof(ctx->track_id));
    cJSON_Delete(root);
    if (st != 200 || parsed != 0) {
        logLine("qr: submit failed st=%d body=%.160s\n", st, ctx->auth_json);
        qr_err(err, err_sz, "Паспорт: нет трека (%d)", st);
        return -3;
    }
    logLine("qr: track ok\n");

    // Шаг 3: magic-ссылка для QR.
    snprintf(body, sizeof(body), "location_id=0&magic_track_id=%s&track_id=",
             ctx->track_id);
    st = qr_call(ctx, HTTP_POST, QR_PWL "/api/passport/auth/magic/code",
                 "application/x-www-form-urlencoded", body, NULL, 0,
                 resp, sizeof(ctx->response));
    root = st == 200 ? ym_json_parse(resp, NULL) : NULL;
    parsed = ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "link"), link, (size_t)link_sz);
    cJSON_Delete(root);
    if (st != 200 || parsed != 0 ||
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
static int qr_xtoken(YaQrAuth *ctx, char *xtoken, int xtoken_sz)
{
    char *resp = ctx->response;
    char *cookies;
    char *hdrs;
    int cl, st, cap;
    NetHttpResponse hr;
    int r;

    cap = ctx->jar.count * (HTTP_JAR_NAME + HTTP_JAR_VALUE + 4) + 16;
    cookies = (char *)malloc((size_t)cap);
    hdrs = (char *)malloc((size_t)cap + 128);
    if (!cookies || !hdrs) {
        free(cookies);
        free(hdrs);
        return -1;
    }
    cl = http_jar_format(&ctx->jar, cookies, cap);
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
        int n = hr.body_size < (int)sizeof(ctx->response) - 1 ? hr.body_size
                                                     : (int)sizeof(ctx->response) - 1;
        memcpy(resp, hr.body, (size_t)n);
        resp[n] = '\0';
    }
    net_http_response_free(&hr);
    cJSON *root = ym_json_parse(resp, NULL);
    int parsed = ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "access_token"),
                                     xtoken, (size_t)xtoken_sz);
    cJSON_Delete(root);
    if (parsed != 0) {
        logLine("qr: xtoken missing st=%d body=%.160s\n", st, resp);
        return -6;
    }
    logLine("qr: xtoken ok\n");
    return 0;
}

// Шаг 7: x_token -> токен Музыки (client_id Музыки из ya_auth.h).
static int qr_music_token(YaQrAuth *ctx, const char *xtoken, char *token, int token_sz)
{
    char *resp = ctx->response;
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
        int n = hr.body_size < (int)sizeof(ctx->response) - 1 ? hr.body_size
                                                     : (int)sizeof(ctx->response) - 1;
        memcpy(resp, hr.body, (size_t)n);
        resp[n] = '\0';
    }
    net_http_response_free(&hr);
    cJSON *root = ym_json_parse(resp, NULL);
    int parsed = ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "access_token"),
                                     token, (size_t)token_sz);
    cJSON_Delete(root);
    if (parsed != 0) {
        logLine("qr: music token missing st=%d\n", st);
        return -7;
    }
    logLine("qr: music token ok\n");
    return 0;
}

int ya_qr_poll(YaQrAuth *ctx, char *token, int token_sz, char *err, int err_sz)
{
    if (!ctx) return -1;
    char *resp = ctx->response;
    char state[48];
    char track2[96];
    char body[160];
    char xtoken[256];
    int st, r;

    if (!token || token_sz <= 1) return -1;
    if (!ctx->auth_json[0] || !ctx->csrf[0]) return -2;

    // Шаг 4: статус magic-ссылки.
    st = qr_call(ctx, HTTP_POST, QR_PWL "/api/passport/auth/magic/code/status",
                 "application/json", ctx->auth_json, NULL, 0, resp, sizeof(ctx->response));
    if (st < 0) {
        qr_err(err, err_sz, "Ошибка сети (%d)", st);
        return st;
    }
    cJSON *root = ym_json_parse(resp, NULL);
    if (ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "state"), state, sizeof(state)) != 0) {
        state[0] = '\0';
    }
    if (strcmp(state, "otp_auth_finished") != 0) {
        cJSON_Delete(root);
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
    if (ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(root, "trackId"), track2, sizeof(track2)) != 0) {
        snprintf(track2, sizeof(track2), "%s", ctx->track_id);
    }
    cJSON_Delete(root);
    snprintf(body, sizeof(body), "track_id=%s", track2);
    st = qr_call(ctx, HTTP_POST, QR_PWL "/api/passport/sessions/get_session",
                 "application/x-www-form-urlencoded", body, NULL, 0,
                 resp, sizeof(ctx->response));
    logLine("qr: get_session st=%d cookies=%d sid=%d\n", st, ctx->jar.count,
            http_jar_get(&ctx->jar, "Session_id") ? 1 : 0);
    if (st != 200 || !http_jar_get(&ctx->jar, "Session_id")) {
        qr_err(err, err_sz, "Паспорт: нет сессии (%d)", st);
        return -5;
    }

    // Шаги 6-7.
    r = qr_xtoken(ctx, xtoken, sizeof(xtoken));
    if (r != 0) {
        qr_err(err, err_sz, "Ошибка x_token (%d)", r);
        return r < 0 ? r : -6;
    }
    r = qr_music_token(ctx, xtoken, token, token_sz);
    memset(xtoken, 0, sizeof(xtoken));
    if (r != 0) {
        qr_err(err, err_sz, "Ошибка токена (%d)", r);
        return r < 0 ? r : -7;
    }
    ya_qr_reset(ctx);
    return 1;
}
