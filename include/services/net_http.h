#ifndef YM_SERVICES_NET_HTTP_H
#define YM_SERVICES_NET_HTTP_H

#include "services/net_tls.h"

// HTTP ответ (in-memory путь). body выделяется через malloc,
// освобождается через net_http_response_free.
typedef struct {
    int status_code;        // HTTP статус код (200, 404, ...)
    char *body;             // Тело ответа
    int body_size;          // Размер тела
    int is_chunked;         // Был ли ответ chunked
} NetHttpResponse;

// on_chunk: вызывается на каждый блок body-байтов (для chunked — уже раскодированных).
//   Возврат 0 — продолжать; NET_HTTP_STREAM_CANCELLED — отменить (не ошибка);
//   любое другое ненулевое — ошибка.
// on_progress: вызывается после каждого блока, а также один раз с (0, content_length)
//   сразу после заголовков (до первого байта тела).
typedef int (*NetHttpStreamChunkCallback)(const char *data, int size, void *user_data);
typedef int (*NetHttpDownloadProgressCallback)(int written_total, int content_length, void *user_data);

#define NET_HTTP_STREAM_CANCELLED (-2)
#define NET_HTTP_ERR_INVALID      (-21001)
#define NET_HTTP_ERR_OFFLINE      (-21002)
#define NET_HTTP_ERR_DNS          (-21003)
#define NET_HTTP_ERR_CONNECT      (-21004)
#define NET_HTTP_ERR_TLS          (-21005)
#define NET_HTTP_ERR_PROTOCOL     (-21006)

int net_http_error_is_network(int error_code);

#define HTTP_TLS_BUFFER_SMALL   (16 * 1024)
#define HTTP_TLS_BUFFER_MEDIUM  (64 * 1024)

typedef enum {
    HTTP_GET  = 0,
    HTTP_POST = 1
} HttpMethod;

// Описание HTTP-запроса. Инициализируется через designated initializers;
// незаполненные поля должны быть нулевыми.
typedef struct {
    HttpMethod  method;         // HTTP_GET / HTTP_POST
    const char *url;            // полный HTTPS URL
    const char *token;          // OAuth-токен или NULL (без Authorization)
    const char *accept;         // Accept-заголовок; NULL => дефолт по функции
    const char *content_type;   // только POST; NULL => "application/json"
    const char *payload;        // только POST
    int         payload_size;   // размер payload; <0 => strlen(payload)
    int         initial_buffer; // только http_request; <=0 => HTTP_TLS_BUFFER_MEDIUM
    int         identity_encoding; // отправить Accept-Encoding: identity
    int         range_enabled;  // только GET/http_stream; отправить Range
    int         range_start;    // абсолютное начало Range: bytes=<offset>-
    int         range_total;    // ожидаемый полный размер объекта для строгой проверки 206
} HttpRequest;

// Приёмник тела для потокового запроса. Любое поле опционально;
// должно быть задано хотя бы одно из file_path / on_chunk / on_progress.
typedef struct {
    const char *file_path;                       // != NULL => писать тело в файл
    NetHttpStreamChunkCallback on_chunk;         // != NULL => блоки тела в callback
    NetHttpDownloadProgressCallback on_progress; // != NULL => прогресс + сигнал content_length
    void *user;                                  // user_data для callback'ов
} HttpSink;

// In-memory запрос (JSON API): тело целиком копится в RAM.
// Всегда отправляет заголовок X-Yandex-Music-Client (как десктоп-клиент).
// out нужно освободить через net_http_response_free.
// Возвращает 0 при успехе, <0 при ошибке.
int http_request(const HttpRequest *req, NetHttpResponse *out);

// Потоковый запрос (аудио/файлы/обложки): тело не копится в RAM, а идёт в sink.
// Прозрачно обрабатывает и chunked, и content-length framing.
// Возвращает 0 при успехе, NET_HTTP_STREAM_CANCELLED при отмене из callback,
// <0 при ошибке.
int http_stream(const HttpRequest *req, const HttpSink *sink);

// Освобождение in-memory ответа.
void net_http_response_free(NetHttpResponse *response);

#endif
