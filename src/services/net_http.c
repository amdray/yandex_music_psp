#include "services/net_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include "core/logger.h"
#include "core/fs.h"
#include "mbedtls/ssl.h"
#include "services/net_activity.h"

// ------------------------------------------------------------------ helpers
static int parse_https_url(const char *url, char *host, size_t host_len,
                           int *out_port, char *path, size_t path_len);
static int parse_http_response(const char *response, int response_size, int *out_status,
                                const char **out_body, int *out_body_size, int *out_chunked,
                                int *out_content_length);
static int parse_content_range(const char *response, int response_size,
                               int *out_start, int *out_end, int *out_total);
static int decode_chunked_body(const char *body, int body_size, char **out_body, int *out_size);
static char *read_tls_response(NetTlsConnection *conn, int initial_capacity, int *out_size);
static int http_request_length(const char *context, int requested, size_t *out_size);
static void http_log_large_alloc(const char *context, size_t size);
static int parse_chunk_size_hex(const char *line, int len, int *out_size);
static int stream_chunked_consume(const char *data, int size,
                                  NetHttpStreamChunkCallback on_chunk,
                                  void *user_data,
                                  int *state,
                                  int *chunk_remaining,
                                  char *line_buf,
                                  int *line_len,
                                  int *out_done);
static int write_fully(SceUID fd, const void *buf, int size);

#define HTTP_ALLOC_WARN_THRESHOLD (512 * 1024)
#define HTTP_CHUNK_LIMIT (256 * 1024)

#define HTTP_STREAM_STATE_CHUNK_SIZE_LINE 0
#define HTTP_STREAM_STATE_CHUNK_DATA 1
#define HTTP_STREAM_STATE_CHUNK_DATA_CR 2
#define HTTP_STREAM_STATE_CHUNK_DATA_LF 3
#define HTTP_STREAM_STATE_TRAILER_LINE 4

// Streaming header-accumulation buffer. HTTP response headers for these
// endpoints stay well under 2 KB in practice (calibrated 2026-04-03 against
// data/logs: max observed ~1.5 KB); 8 KB leaves wide margin.
#define HTTP_STREAM_HDR_BUF_SIZE (8 * 1024)

// ------------------------------------------------------- request/connection

// Build the HTTP request header block for `req` into `buf`.
// `accept` is the resolved Accept value; `send_client_header` adds the
// Yandex desktop-client identifier. Returns 0 on success, -1 if truncated.
static int http_build_request(char *buf, size_t bufsz, const HttpRequest *req,
                              const char *host, int port, const char *path,
                              const char *accept, int payload_length,
                              int send_client_header)
{
    size_t off = 0;
    int n;

    if (port == 443) {
        n = snprintf(buf + off, bufsz - off,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: PSP\r\n",
                     req->method == HTTP_POST ? "POST" : "GET", path, host);
    } else {
        n = snprintf(buf + off, bufsz - off,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: PSP\r\n",
                     req->method == HTTP_POST ? "POST" : "GET", path, host, port);
    }
    if (n < 0 || (size_t)n >= bufsz - off) return -1;
    off += (size_t)n;

    if (send_client_header) {
        n = snprintf(buf + off, bufsz - off,
                     "X-Yandex-Music-Client: YandexMusicDesktopAppWindows/5.23.2\r\n");
        if (n < 0 || (size_t)n >= bufsz - off) return -1;
        off += (size_t)n;
    }

    n = snprintf(buf + off, bufsz - off, "Accept: %s\r\n", accept);
    if (n < 0 || (size_t)n >= bufsz - off) return -1;
    off += (size_t)n;

    if (req->method == HTTP_POST) {
        n = snprintf(buf + off, bufsz - off,
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n",
                     req->content_type ? req->content_type : "application/json",
                     payload_length);
        if (n < 0 || (size_t)n >= bufsz - off) return -1;
        off += (size_t)n;
    }

    if (req->token) {
        n = snprintf(buf + off, bufsz - off, "Authorization: OAuth %s\r\n", req->token);
        if (n < 0 || (size_t)n >= bufsz - off) return -1;
        off += (size_t)n;
    }

    if (req->identity_encoding) {
        n = snprintf(buf + off, bufsz - off, "Accept-Encoding: identity\r\n");
        if (n < 0 || (size_t)n >= bufsz - off) return -1;
        off += (size_t)n;
    }

    if (req->method == HTTP_GET && req->range_enabled) {
        n = snprintf(buf + off, bufsz - off, "Range: bytes=%d-\r\n",
                     req->range_start);
        if (n < 0 || (size_t)n >= bufsz - off) return -1;
        off += (size_t)n;
    }

    n = snprintf(buf + off, bufsz - off, "Connection: close\r\n\r\n");
    if (n < 0 || (size_t)n >= bufsz - off) return -1;

    return 0;
}

// Write exactly `len` bytes over TLS, yielding on WANT_READ/WANT_WRITE.
// Returns 0 on success, -1 on TLS error.
static int http_write_all(NetTlsConnection *conn, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t remaining = len;
    while (remaining > 0) {
        int err = net_tls_write(conn, p, remaining);
        if (err > 0) {
            p += err;
            remaining -= (size_t)err;
            continue;
        }
        if (err == MBEDTLS_ERR_SSL_WANT_READ || err == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* Yield to other threads - prevents 100% CPU during network I/O */
            sceKernelDelayThread(5 * 1000);
            continue;
        }
        return -1;
    }
    return 0;
}

// Parse URL, open TLS, send request header (+ payload for POST).
// On success returns 0 with `conn` connected; on failure returns -1 with
// `conn` already disconnected (caller must not touch it).
static int map_tls_connect_error(int rc)
{
    switch (rc) {
    case NET_TLS_ERR_OFFLINE: return NET_HTTP_ERR_OFFLINE;
    case NET_TLS_ERR_DNS: return NET_HTTP_ERR_DNS;
    case NET_TLS_ERR_CONNECT: return NET_HTTP_ERR_CONNECT;
    case NET_TLS_ERR_CANCELLED: return NET_HTTP_STREAM_CANCELLED;
    default: return NET_HTTP_ERR_TLS;
    }
}

int net_http_error_is_network(int error_code)
{
    return error_code == NET_HTTP_ERR_OFFLINE ||
           error_code == NET_HTTP_ERR_DNS ||
           error_code == NET_HTTP_ERR_CONNECT ||
           error_code == NET_HTTP_ERR_TLS;
}

static int http_open(const HttpRequest *req, const char *accept,
                     int send_client_header, NetTlsConnection *conn)
{
    char host[128];
    char path[256];
    char request[1536];
    int port = 443;
    int payload_length = 0;

    if (parse_https_url(req->url, host, sizeof(host), &port,
                        path, sizeof(path)) != 0) {
        logLine("http: bad url\n");
        return -1;
    }

    if (req->method == HTTP_POST) {
        if (req->payload && req->payload_size >= 0) {
            payload_length = req->payload_size;
        } else if (req->payload) {
            payload_length = (int)strlen(req->payload);
        }
    }

    {
        int connect_rc = net_tls_connect(host, port, conn);
        if (connect_rc != 0) {
            int mapped = map_tls_connect_error(connect_rc);
            logLine("http: tls connect failed (%s) rc=%d mapped=%d\n",
                    host, connect_rc, mapped);
            return mapped;
        }
    }

    if (http_build_request(request, sizeof(request), req, host, port, path,
                           accept, payload_length, send_client_header) != 0) {
        logLine("http: request header too large\n");
        net_tls_disconnect(conn);
        return -1;
    }

    if (http_write_all(conn, request, strlen(request)) != 0) {
        logLine("http: write header failed\n");
        net_tls_disconnect(conn);
        return NET_HTTP_ERR_TLS;
    }

    if (req->method == HTTP_POST && req->payload && payload_length > 0) {
        if (http_write_all(conn, req->payload, (size_t)payload_length) != 0) {
            logLine("http: write payload failed\n");
            net_tls_disconnect(conn);
            return NET_HTTP_ERR_TLS;
        }
    }

    return 0;
}

// ------------------------------------------------------------ public: RAM

int http_request(const HttpRequest *req, NetHttpResponse *out)
{
    if (!req || !req->url || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    const char *accept = req->accept ? req->accept : "application/json";
    int initial_buffer = req->initial_buffer > 0 ? req->initial_buffer : HTTP_TLS_BUFFER_MEDIUM;

    NetTlsConnection conn;
    {
        int open_rc = http_open(req, accept, /*send_client_header=*/1, &conn);
        if (open_rc != 0) {
            return open_rc;
        }
    }

    int ret = NET_HTTP_ERR_PROTOCOL;
    char *response = NULL;
    int response_size = 0;

    response = read_tls_response(&conn, initial_buffer, &response_size);
    if (!response) {
        logLine("http: response read failed\n");
        ret = NET_HTTP_ERR_TLS;
        goto cleanup;
    }

    {
        const char *body_ptr = NULL;
        int body_len = 0;
        int chunked = 0;
        int content_length = -1;
        int status = 0;
        char *decoded = NULL;
        int decoded_size = 0;

        if (parse_http_response(response, response_size, &status, &body_ptr, &body_len,
                                &chunked, &content_length) != 0) {
            logLine("http: parse failed\n");
            goto cleanup;
        }
        logLine("http: status=%d body_len=%d cl=%d chunked=%d\n",
                status, body_len, content_length, chunked);

        if (chunked) {
            if (decode_chunked_body(body_ptr, body_len, &decoded, &decoded_size) != 0) {
                logLine("http: chunked decode failed\n");
                goto cleanup;
            }
            out->body = decoded;
            out->body_size = decoded_size;
        } else {
            int copy_size = body_len;
            if (content_length >= 0 && content_length < copy_size) {
                copy_size = content_length;
            }
            size_t alloc_size = 0;
            if (http_request_length("http response body", copy_size, &alloc_size) != 0) {
                goto cleanup;
            }
            out->body = (char *)malloc(alloc_size + 1);
            if (!out->body) {
                logLine("http: malloc failed %zu\n", alloc_size);
                goto cleanup;
            }
            memcpy(out->body, body_ptr, alloc_size);
            out->body[alloc_size] = '\0';
            out->body_size = (int)alloc_size;
        }

        out->status_code = status;
        out->is_chunked = chunked;
        ret = 0;
    }

cleanup:
    if (ret == 0 && !net_tls_connection_is_current(&conn)) {
        ret = NET_HTTP_ERR_OFFLINE;
    }
    if (response) {
        free(response);
    }
    net_tls_disconnect(&conn);
    if (ret != 0 && out->body) {
        free(out->body);
        memset(out, 0, sizeof(*out));
    }
    return ret;
}

// -------------------------------------------------------- public: streaming

// Per-request body sink context passed through the chunked/content-length
// consumers. Funnels each body segment to file, callback and progress.
typedef struct {
    const HttpSink *sink;
    const NetTlsConnection *conn;
    SceUID fd;              // -1 if not writing to a file
    int total;             // body bytes emitted so far
    int content_length;    // -1 if unknown (chunked)
    int progress_base;     // absolute offset of this response body
    int progress_length;   // complete object length reported to callback
    int activity_admitted;
} StreamEmitCtx;

// Emit one body segment. Returns 0 to continue, NET_HTTP_STREAM_CANCELLED if a
// callback asked to cancel, -1 on write error or generic callback failure.
// Signature matches NetHttpStreamChunkCallback so it can drive the chunked
// consumer directly.
static int stream_emit(const char *data, int size, void *user)
{
    StreamEmitCtx *e = (StreamEmitCtx *)user;

    if (size <= 0) {
        return 0;
    }
    if (!net_tls_connection_is_current(e->conn)) {
        return NET_HTTP_ERR_OFFLINE;
    }
    if (!e->activity_admitted) {
        net_activity_begin();
        e->activity_admitted = 1;
    }
    net_activity_body_bytes(size);
    if (e->fd >= 0) {
        if (write_fully(e->fd, data, size) != 0) {
            return -1;
        }
    }
    if (e->sink->on_chunk) {
        int rc = e->sink->on_chunk(data, size, e->sink->user);
        if (rc != 0) {
            return rc;   // propagate cancel/error code
        }
    }
    e->total += size;
    if (e->sink->on_progress) {
        int rc = e->sink->on_progress(e->progress_base + e->total,
                                      e->progress_length, e->sink->user);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

int http_stream(const HttpRequest *req, const HttpSink *sink)
{
    if (!req || !req->url || !sink) {
        return -1;
    }
    if (!sink->file_path && !sink->on_chunk && !sink->on_progress) {
        return -1;   // nowhere to put the body
    }

    const char *accept = req->accept ? req->accept : "*/*";

    char *hdr_buf = (char *)malloc((size_t)HTTP_STREAM_HDR_BUF_SIZE + 1);
    if (!hdr_buf) {
        logLine("http: stream hdr alloc failed\n");
        return -1;
    }

    NetTlsConnection conn;
    {
        int open_rc = http_open(req, accept, /*send_client_header=*/0, &conn);
        if (open_rc != 0) {
            free(hdr_buf);
            return open_rc;
        }
    }

    int ret = NET_HTTP_ERR_PROTOCOL;
    SceUID fd = -1;
    int hdr_len = 0;
    int status = 0;
    int chunked = 0;
    int content_length = -1;
    int progress_base = 0;
    int progress_length = -1;
    const char *body_ptr = NULL;
    int body_len = 0;
    StreamEmitCtx emit = { 0 };

    // --- read response headers into hdr_buf (may carry initial body bytes) ---
    {
        int hdr_done = 0;
        while (!hdr_done && hdr_len < HTTP_STREAM_HDR_BUF_SIZE) {
            int space = HTTP_STREAM_HDR_BUF_SIZE - hdr_len;
            int err = net_tls_read(&conn, (unsigned char *)hdr_buf + hdr_len, (size_t)space);
            if (err == 0 || err == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                break;
            }
            if (err < 0) {
                if (err == MBEDTLS_ERR_SSL_WANT_READ || err == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    sceKernelDelayThread(5 * 1000);
                    continue;
                }
                logLine("http: stream hdr read failed %d\n", err);
                ret = NET_HTTP_ERR_TLS;
                goto cleanup;
            }
            hdr_len += err;
            hdr_buf[hdr_len] = '\0';
            if (strstr(hdr_buf, "\r\n\r\n")) {
                hdr_done = 1;
            }
        }
        if (!hdr_done) {
            logLine("http: stream header not complete (hdr_len=%d)\n", hdr_len);
            goto cleanup;
        }
    }

    if (parse_http_response(hdr_buf, hdr_len, &status, &body_ptr, &body_len,
                            &chunked, &content_length) != 0) {
        logLine("http: stream header parse failed\n");
        goto cleanup;
    }
    logLine("http: stream status=%d chunked=%d cl=%d tail=%d\n",
            status, chunked, content_length, body_len);

    if (status < 200 || status >= 300) {
        logLine("http: stream bad status %d\n", status);
        goto cleanup;
    }

    if (req->range_enabled) {
        int range_start = -1;
        int range_end = -1;
        int range_total = -1;
        if (status == 200 && req->range_start == 0 &&
            req->range_total > 0 && content_length == req->range_total) {
            /* A server may legally ignore Range. Starting at zero remains
             * safe: the cache layer verifies the existing prefix bytewise. */
            progress_base = 0;
            progress_length = content_length;
        } else {
            if (status != 206 ||
                parse_content_range(hdr_buf, hdr_len, &range_start, &range_end,
                                    &range_total) != 0 ||
                range_start != req->range_start || range_end < range_start ||
                range_total <= range_end || range_end != range_total - 1 ||
                (req->range_total > 0 && range_total != req->range_total) ||
                (content_length >= 0 &&
                 content_length != range_end - range_start + 1)) {
                logLine("http: invalid range response status=%d requested=%d got=%d-%d/%d cl=%d\n",
                        status, req->range_start, range_start, range_end,
                        range_total, content_length);
                goto cleanup;
            }
            progress_base = range_start;
            progress_length = range_total;
        }
    } else {
        progress_length = content_length;
    }

    // --- file cache-hit: existing file already matches content length ---
    if (sink->file_path && content_length > 0) {
        SceIoStat st;
        memset(&st, 0, sizeof(st));
        if (fs_getstat(sink->file_path, &st) == 0 && (int)st.st_size == content_length) {
            logLine("http: stream cache hit '%s' size=%d\n", sink->file_path, content_length);
            if (!net_tls_connection_is_current(&conn)) goto cleanup;
            if (sink->on_progress &&
                sink->on_progress(content_length, content_length, sink->user) != 0) {
                goto cleanup;
            }
            ret = 0;
            goto cleanup;
        }
    }

    if (sink->file_path) {
        fs_ensure_dir(sink->file_path);
        fd = fs_open(sink->file_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd < 0) {
            logLine("file: open failed '%s' 0x%08X\n", sink->file_path, fd);
            goto cleanup;
        }
    }

    emit = (StreamEmitCtx){ sink, &conn, fd, 0, content_length,
                            progress_base, progress_length, 0 };

    // Signal content_length to the consumer before the first body byte.
    if (sink->on_progress) {
        if (!net_tls_connection_is_current(&conn)) {
            ret = NET_HTTP_ERR_OFFLINE;
            goto cleanup;
        }
        int rc = sink->on_progress(progress_base, progress_length, sink->user);
        if (rc != 0) {
            if (rc == NET_HTTP_STREAM_CANCELLED) {
                ret = NET_HTTP_STREAM_CANCELLED;
            }
            goto cleanup;
        }
    }

    if (chunked) {
        int st_state = HTTP_STREAM_STATE_CHUNK_SIZE_LINE;
        int chunk_remaining = 0;
        char chunk_line[64];
        int chunk_line_len = 0;
        int stream_done = 0;
        char read_buf[4096];

        if (body_len > 0) {
            if (stream_chunked_consume(body_ptr, body_len, stream_emit, &emit,
                                       &st_state, &chunk_remaining,
                                       chunk_line, &chunk_line_len, &stream_done) != 0) {
                logLine("http: stream chunk decode failed (header tail)\n");
                goto cleanup;
            }
        }

        while (!stream_done) {
            int err = net_tls_read(&conn, (unsigned char *)read_buf, sizeof(read_buf));
            if (err == 0 || err == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                break;
            }
            if (err < 0) {
                if (err == MBEDTLS_ERR_SSL_WANT_READ || err == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    sceKernelDelayThread(5 * 1000);
                    continue;
                }
                logLine("http: stream body read failed %d\n", err);
                ret = NET_HTTP_ERR_TLS;
                goto cleanup;
            }
            if (stream_chunked_consume(read_buf, err, stream_emit, &emit,
                                       &st_state, &chunk_remaining,
                                       chunk_line, &chunk_line_len, &stream_done) != 0) {
                logLine("http: stream chunk decode failed (len=%d)\n", err);
                goto cleanup;
            }
        }

        if (!stream_done) {
            logLine("http: stream terminated before terminal chunk\n");
            goto cleanup;
        }
        ret = 0;
    } else {
        char read_buf[4096];

        // Body bytes already present in the header buffer.
        if (body_len > 0) {
            int write_now = body_len;
            if (content_length >= 0 && write_now > content_length) {
                write_now = content_length;
            }
            int rc = stream_emit(body_ptr, write_now, &emit);
            if (rc != 0) {
                if (rc == NET_HTTP_STREAM_CANCELLED) {
                    ret = NET_HTTP_STREAM_CANCELLED;
                }
                goto cleanup;
            }
        }

        while (content_length < 0 || emit.total < content_length) {
            int want = (int)sizeof(read_buf);
            if (content_length >= 0) {
                int remain = content_length - emit.total;
                if (remain < want) {
                    want = remain;
                }
            }
            int err = net_tls_read(&conn, (unsigned char *)read_buf, (size_t)want);
            if (err == 0 || err == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                break;
            }
            if (err < 0) {
                if (err == MBEDTLS_ERR_SSL_WANT_READ || err == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    sceKernelDelayThread(5 * 1000);
                    continue;
                }
                logLine("http: stream body read failed %d\n", err);
                ret = NET_HTTP_ERR_TLS;
                goto cleanup;
            }
            int rc = stream_emit(read_buf, err, &emit);
            if (rc != 0) {
                if (rc == NET_HTTP_STREAM_CANCELLED) {
                    ret = NET_HTTP_STREAM_CANCELLED;
                }
                goto cleanup;
            }
        }

        if (content_length >= 0 && emit.total < content_length) {
            logLine("http: stream incomplete %d/%d\n", emit.total, content_length);
            ret = NET_HTTP_ERR_TLS;
        } else {
            ret = 0;
        }
    }

cleanup:
    if (ret == 0 && !net_tls_connection_is_current(&conn)) {
        ret = NET_HTTP_ERR_OFFLINE;
    }
    if (fd >= 0) {
        fs_close(fd);
        if (ret != 0 && sink->file_path) {
            logLine("file: removing incomplete '%s'\n", sink->file_path);
            fs_remove(sink->file_path);
        }
    }
    free(hdr_buf);
    net_tls_disconnect(&conn);
    if (emit.activity_admitted) net_activity_end();
    return ret;
}

void net_http_response_free(NetHttpResponse *response)
{
    if (!response) {
        return;
    }
    if (response->body) {
        free(response->body);
        response->body = NULL;
    }
    response->body_size = 0;
    response->status_code = 0;
    response->is_chunked = 0;
}

// ============================================================ helper bodies

static int parse_https_url(const char *url, char *host, size_t host_len,
                           int *out_port, char *path, size_t path_len)
{
    const char *p = url;
    const char *slash;
    const char *colon;
    const char *port_text;
    char *port_end;
    long parsed_port;
    size_t host_size;

    if (!out_port || strncmp(p, "https://", 8) != 0) {
        return -1;
    }
    p += 8;
    slash = strchr(p, '/');
    if (!slash) {
        return -1;
    }
    colon = memchr(p, ':', (size_t)(slash - p));
    host_size = colon ? (size_t)(colon - p) : (size_t)(slash - p);
    if (host_size == 0 || host_size + 1 > host_len) {
        return -1;
    }
    memcpy(host, p, host_size);
    host[host_size] = '\0';

    *out_port = 443;
    if (colon) {
        port_text = colon + 1;
        if (port_text == slash) return -1;
        parsed_port = strtol(port_text, &port_end, 10);
        if (port_end != slash || parsed_port < 1 || parsed_port > 65535) {
            return -1;
        }
        *out_port = (int)parsed_port;
    }

    if (strlen(slash) + 1 > path_len) {
        return -1;
    }
    strcpy(path, slash);
    return 0;
}

static int parse_http_response(const char *response, int response_size, int *out_status,
                                const char **out_body, int *out_body_size, int *out_chunked,
                                int *out_content_length)
{
    const char *p;
    const char *end;
    const char *line_end = NULL;
    int status = 0;
    int chunked = 0;
    int content_length = -1;

    if (!response || response_size <= 0) {
        return -1;
    }
    p   = response;
    end = response + response_size;

    line_end = strstr(p, "\r\n");
    if (!line_end) {
        return -1;
    }
    if (sscanf(p, "HTTP/%*s %d", &status) != 1) {
        status = 0;
    }
    p = line_end + 2;

    while (p < end) {
        const char *next = strstr(p, "\r\n");
        size_t len;
        if (!next) {
            return -1;
        }
        if (next == p) {
            p = next + 2;
            break;
        }
        len = (size_t)(next - p);
        if (len > 0) {
            if (len >= 17 && strncasecmp(p, "Transfer-Encoding", 17) == 0) {
                const char *value = strchr(p, ':');
                if (value) {
                    value++;
                    while (*value == ' ' || *value == '\t') {
                        value++;
                    }
                    if (strstr(value, "chunked")) {
                        chunked = 1;
                    }
                }
            } else if (len >= 14 && strncasecmp(p, "Content-Length", 14) == 0) {
                const char *value = strchr(p, ':');
                if (value) {
                    content_length = atoi(value + 1);
                }
            }
        }
        p = next + 2;
    }

    if (p > end) {
        return -1;
    }

    if (out_status) {
        *out_status = status;
    }
    if (out_body) {
        *out_body = p;
    }
    if (out_body_size) {
        *out_body_size = (int)(end - p);
    }
    if (out_chunked) {
        *out_chunked = chunked;
    }
    if (out_content_length) {
        *out_content_length = content_length;
    }
    return 0;
}

static int parse_content_range(const char *response, int response_size,
                               int *out_start, int *out_end, int *out_total)
{
    const char *p = response;
    const char *end = response + response_size;

    if (!response || response_size <= 0 || !out_start || !out_end || !out_total) {
        return -1;
    }

    while (p < end) {
        const char *next = strstr(p, "\r\n");
        size_t len;
        long long start;
        long long finish;
        long long total;

        if (!next) return -1;
        if (next == p) break;
        len = (size_t)(next - p);
        if (len >= 13 && strncasecmp(p, "Content-Range", 13) == 0) {
            const char *value = memchr(p, ':', len);
            if (!value) return -1;
            value++;
            while (value < next && (*value == ' ' || *value == '\t')) value++;
            if (sscanf(value, "bytes %lld-%lld/%lld", &start, &finish, &total) != 3 ||
                start < 0 || finish < 0 || total <= 0 ||
                start > 0x7fffffffLL || finish > 0x7fffffffLL ||
                total > 0x7fffffffLL) {
                return -1;
            }
            *out_start = (int)start;
            *out_end = (int)finish;
            *out_total = (int)total;
            return 0;
        }
        p = next + 2;
    }
    return -1;
}

static int decode_chunked_body(const char *body, int body_size, char **out_body, int *out_size)
{
    const char *p;
    const char *end;
    int capacity = body_size > 0 ? body_size : 256;
    int size = 0;
    char *out = (char *)malloc((size_t)capacity + 1);

    if (!body || body_size <= 0) {
        free(out);
        return -1;
    }
    if (!out) {
        return -1;
    }
    p   = body;
    end = body + body_size;
    http_log_large_alloc("chunk decoder (initial)", (size_t)capacity);

    while (p < end) {
        const char *line_end = strstr(p, "\r\n");
        int chunk_size = 0;
        int have_digit = 0;
        int after_size = 0;
        if (!line_end) {
            logLine("http: chunked no CRLF at p+%d (body_size=%d)\n", (int)(p - body), body_size);
            free(out);
            return -1;
        }
        for (const char *c = p; c < line_end; c++) {
            if (*c == ';') {
                break;
            }
            if (isxdigit((unsigned char)*c)) {
                int digit = 0;
                if (after_size) {
                    logLine("http: invalid chunk size char after whitespace 0x%02X\n", (unsigned char)*c);
                    free(out);
                    return -1;
                }
                if (*c >= '0' && *c <= '9') {
                    digit = *c - '0';
                } else if (*c >= 'a' && *c <= 'f') {
                    digit = 10 + (*c - 'a');
                } else if (*c >= 'A' && *c <= 'F') {
                    digit = 10 + (*c - 'A');
                }
                if (chunk_size > (HTTP_CHUNK_LIMIT - digit) / 16) {
                    logLine("http: suspicious chunk size overflow (limit=%d)\n", HTTP_CHUNK_LIMIT);
                    free(out);
                    return -1;
                }
                chunk_size = chunk_size * 16 + digit;
                have_digit = 1;
            } else if (*c == ' ' || *c == '\t') {
                if (have_digit) {
                    after_size = 1;
                }
            } else {
                logLine("http: invalid chunk size char 0x%02X\n", (unsigned char)*c);
                free(out);
                return -1;
            }
        }
        if (!have_digit || chunk_size > HTTP_CHUNK_LIMIT) {
            logLine("http: suspicious chunk size %d (limit=%d)\n", chunk_size, HTTP_CHUNK_LIMIT);
            free(out);
            return -1;
        }
        p = line_end + 2;
        if (chunk_size == 0) {
            break;
        }
        if (p + chunk_size + 2 > end) {
            logLine("http: chunked overflow chunk=%d remaining=%d decoded=%d\n", chunk_size, (int)(end - p), size);
            free(out);
            return -1;
        }
        if (size + chunk_size > capacity) {
            int new_capacity = capacity * 2;
            if (new_capacity < size + chunk_size) {
                new_capacity = size + chunk_size;
            }
            if (new_capacity < 0) {
                logLine("http: chunk capacity overflow\n");
                free(out);
                return -1;
            }
            http_log_large_alloc("chunk decoder (resize)", (size_t)new_capacity);
            char *next = (char *)realloc(out, (size_t)new_capacity + 1);
            if (!next) {
                free(out);
                return -1;
            }
            out = next;
            capacity = new_capacity;
        }
        memcpy(out + size, p, (size_t)chunk_size);
        size += chunk_size;
        p += chunk_size;
        if (p + 2 > end || p[0] != '\r' || p[1] != '\n') {
            free(out);
            return -1;
        }
        p += 2;
    }

    out[size] = '\0';
    if (out_body) {
        *out_body = out;
    } else {
        free(out);
    }
    if (out_size) {
        *out_size = size;
    }
    return 0;
}

static char *read_tls_response(NetTlsConnection *conn, int initial_capacity, int *out_size)
{
    char *buffer = NULL;
    int capacity = initial_capacity;
    int size = 0;
    int headers_done = 0;
    int content_length = -1;
    int is_chunked = 0;
    int activity_admitted = 0;

    if (capacity <= 0) {
        logLine("tls: invalid initial capacity %d\n", capacity);
        return NULL;
    }

    buffer = (char *)malloc((size_t)capacity + 1);
    if (!buffer) {
        logLine("tls: malloc failed %d\n", capacity);
        return NULL;
    }
    http_log_large_alloc("tls buffer (initial)", (size_t)capacity);

    while (1) {
        int had_headers = headers_done;
        /* Stop reading if we have received all body bytes per Content-Length. */
        if (headers_done && !is_chunked && content_length >= 0) {
            const char *header_end = strstr(buffer, "\r\n\r\n");
            if (header_end) {
                int header_size = (int)(header_end + 4 - buffer);
                int body_received = size - header_size;
                if (body_received >= content_length) {
                    break;
                }
            }
        }

        /* Stop reading if chunked transfer is complete.
         * The terminal chunk is "0\r\n\r\n" (or "0\r\n" + trailers + "\r\n").
         * We search for "\r\n0\r\n\r\n" after the header block, which marks
         * the end of all data chunks. storage.mds.yandex.net uses chunked
         * encoding and keeps the connection alive, so without this check
         * read_tls_response blocks forever waiting for PEER_CLOSE_NOTIFY. */
        if (headers_done && is_chunked) {
            const char *hdr_end = strstr(buffer, "\r\n\r\n");
            if (hdr_end) {
                const char *body = hdr_end + 4;
                if (strstr(body, "\r\n0\r\n\r\n")) {
                    break;
                }
            }
        }

        int read = net_tls_read(conn, (unsigned char *)buffer + size, capacity - size);
        if (read == 0 || read == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        }
        if (read < 0) {
            if (read == MBEDTLS_ERR_SSL_WANT_READ || read == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            logLine("tls: read FAILED %d\n", read);
            if (activity_admitted) net_activity_end();
            free(buffer);
            return NULL;
        }
        size += read;
        buffer[size] = '\0';
        if (had_headers) {
            if (!activity_admitted) {
                net_activity_begin();
                activity_admitted = 1;
            }
            net_activity_body_bytes(read);
        }

        /* After headers arrive, parse content-length/chunked once. */
        if (!headers_done) {
            if (strstr(buffer, "\r\n\r\n")) {
                const char *activity_header_end = strstr(buffer, "\r\n\r\n");
                headers_done = 1;
                if (activity_header_end) {
                    int body_bytes =
                        size - (int)(activity_header_end + 4 - buffer);
                    if (body_bytes > 0) {
                        net_activity_begin();
                        activity_admitted = 1;
                        net_activity_body_bytes(body_bytes);
                    }
                }
                /* HTTP headers are case-insensitive but Yandex servers send
                 * standard casing, so strstr is sufficient here. */
                const char *cl = strstr(buffer, "Content-Length:");
                if (!cl) cl = strstr(buffer, "content-length:");
                if (cl) {
                    content_length = atoi(cl + 15);
                }
                const char *te = strstr(buffer, "Transfer-Encoding:");
                if (!te) te = strstr(buffer, "transfer-encoding:");
                if (te && strstr(te, "chunked")) {
                    is_chunked = 1;
                }
            }
        }

        if (size >= capacity) {
            int new_capacity = capacity * 2;
            char *next = (char *)realloc(buffer, (size_t)new_capacity + 1);
            if (!next) {
                logLine("tls: realloc failed %d\n", new_capacity);
                if (activity_admitted) net_activity_end();
                free(buffer);
                return NULL;
            }
            http_log_large_alloc("tls buffer (resize)", (size_t)new_capacity);
            buffer = next;
            capacity = new_capacity;
        }
    }

    buffer[size] = '\0';
    if (out_size) {
        *out_size = size;
    }
    if (activity_admitted) net_activity_end();
    return buffer;
}

static int parse_chunk_size_hex(const char *line, int len, int *out_size)
{
    int value = 0;
    int saw_digit = 0;
    int i;

    if (!line || len <= 0 || !out_size) {
        return -1;
    }

    for (i = 0; i < len; ++i) {
        char c = line[i];
        int digit = 0;

        if (c == ';') {
            break;
        }
        if (c == ' ' || c == '\t') {
            if (saw_digit) {
                break;
            }
            continue;
        }

        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            digit = 10 + (c - 'A');
        } else {
            return -1;
        }

        saw_digit = 1;
        if (value > (HTTP_CHUNK_LIMIT / 16)) {
            return -1;
        }
        value = value * 16 + digit;
        if (value > HTTP_CHUNK_LIMIT) {
            return -1;
        }
    }

    if (!saw_digit) {
        return -1;
    }

    *out_size = value;
    return 0;
}

static int stream_chunked_consume(const char *data, int size,
                                  NetHttpStreamChunkCallback on_chunk,
                                  void *user_data,
                                  int *state,
                                  int *chunk_remaining,
                                  char *line_buf,
                                  int *line_len,
                                  int *out_done)
{
    int i = 0;

    if (!data || size < 0 || !on_chunk || !state || !chunk_remaining || !line_buf || !line_len || !out_done) {
        return -1;
    }

    while (i < size) {
        if (*state == HTTP_STREAM_STATE_CHUNK_SIZE_LINE) {
            char c = data[i++];
            if (*line_len >= 62) {
                return -1;
            }
            line_buf[*line_len] = c;
            (*line_len)++;

            if (c == '\n') {
                int parse_len = *line_len;
                int chunk_size = 0;

                if (parse_len >= 2 && line_buf[parse_len - 2] == '\r') {
                    parse_len -= 2;
                } else if (parse_len >= 1) {
                    parse_len -= 1;
                }

                if (parse_chunk_size_hex(line_buf, parse_len, &chunk_size) != 0) {
                    return -1;
                }

                *line_len = 0;
                if (chunk_size == 0) {
                    *state = HTTP_STREAM_STATE_TRAILER_LINE;
                } else {
                    *chunk_remaining = chunk_size;
                    *state = HTTP_STREAM_STATE_CHUNK_DATA;
                }
            }
            continue;
        }

        if (*state == HTTP_STREAM_STATE_CHUNK_DATA) {
            int left = size - i;
            int consume = left;
            if (consume > *chunk_remaining) {
                consume = *chunk_remaining;
            }

            if (consume <= 0) {
                return -1;
            }

            if (on_chunk(data + i, consume, user_data) != 0) {
                return -1;
            }

            i += consume;
            *chunk_remaining -= consume;

            if (*chunk_remaining == 0) {
                *state = HTTP_STREAM_STATE_CHUNK_DATA_CR;
            }
            continue;
        }

        if (*state == HTTP_STREAM_STATE_CHUNK_DATA_CR) {
            if (data[i++] != '\r') {
                return -1;
            }
            *state = HTTP_STREAM_STATE_CHUNK_DATA_LF;
            continue;
        }

        if (*state == HTTP_STREAM_STATE_CHUNK_DATA_LF) {
            if (data[i++] != '\n') {
                return -1;
            }
            *state = HTTP_STREAM_STATE_CHUNK_SIZE_LINE;
            continue;
        }

        if (*state == HTTP_STREAM_STATE_TRAILER_LINE) {
            char c = data[i++];
            if (*line_len >= 62) {
                return -1;
            }
            line_buf[*line_len] = c;
            (*line_len)++;

            if (c == '\n') {
                if (*line_len == 2 && line_buf[0] == '\r' && line_buf[1] == '\n') {
                    *out_done = 1;
                    return 0;
                }
                *line_len = 0;
            }
            continue;
        }

        return -1;
    }

    return 0;
}

// Write exactly `size` bytes to fd in a retry loop.
// sceIoWrite() may return less than requested on some PSP firmware builds;
// this helper ensures the full chunk is committed before returning.
// Returns 0 on success, -1 if a write error or zero-progress write occurs.
static int write_fully(SceUID fd, const void *buf, int size)
{
    const char *p = (const char *)buf;
    int remaining = size;
    while (remaining > 0) {
        int rc = fs_write(fd, p, (SceSize)remaining);
        if (rc < 0) {
            logLine("file: write error 0x%08X\n", rc);
            return -1;
        }
        if (rc == 0) {
            logLine("file: write returned 0, disk full?\n");
            return -1;
        }
        p += rc;
        remaining -= rc;
    }
    return 0;
}

static int http_request_length(const char *context, int requested, size_t *out_size)
{
    if (requested < 0) {
        logLine("http: bad %s length %d\n", context, requested);
        return -1;
    }
    size_t size = (size_t)requested;
    if (size >= HTTP_ALLOC_WARN_THRESHOLD) {
        logLine("http: %s allocation %zu bytes\n", context, size);
    }
    if (out_size) {
        *out_size = size;
    }
    return 0;
}

static void http_log_large_alloc(const char *context, size_t size)
{
    if (size >= HTTP_ALLOC_WARN_THRESHOLD) {
        logLine("http: %s allocation %zu bytes\n", context, size);
    }
}
