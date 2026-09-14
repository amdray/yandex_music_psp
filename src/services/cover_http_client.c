#include "services/cover_http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include "core/fs.h"
#include "core/logger.h"
#include "mbedtls/ssl.h"
#include "services/net_stack.h"
#include "services/net_activity.h"

#define COVER_HOST "avatars.yandex.net"
#define COVER_PORT 443
#define HDR_BUF_SIZE (8 * 1024)
#define READ_BUF_SIZE 4096

static int tls_write_all(NetTlsConnection *conn, const char *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        int rc = net_tls_write(conn, p, remaining);
        if (rc > 0) {
            p += rc;
            remaining -= (size_t)rc;
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            sceKernelDelayThread(5 * 1000);
            continue;
        }
        return -1;
    }
    return 0;
}

static int write_fully(SceUID fd, const void *buf, int size)
{
    const char *p = (const char *)buf;
    int remaining = size;
    while (remaining > 0) {
        int rc = fs_write(fd, p, (SceSize)remaining);
        if (rc < 0) {
            logLine("cover_http: file write error 0x%08X\n", rc);
            return -1;
        }
        if (rc == 0) {
            logLine("cover_http: file write returned 0, disk full?\n");
            return -1;
        }
        p += rc;
        remaining -= rc;
    }
    return 0;
}

static void client_disconnect(CoverHttpClient *client, const char *reason)
{
    if (client->connected) {
        logLine("cover_http: disconnect (%s)\n", reason);
        net_tls_disconnect(&client->conn);
        client->connected = 0;
    }
}

static int client_connect(CoverHttpClient *client)
{
    logLine("cover_http: connect -> " COVER_HOST ":%d\n", COVER_PORT);
    if (net_tls_connect(COVER_HOST, COVER_PORT, &client->conn) != 0) {
        logLine("cover_http: connect failed\n");
        return -1;
    }
    client->connected = 1;
    logLine("cover_http: connected (new TLS handshake)\n");
    return 0;
}

static int do_download(CoverHttpClient *client, const char *path, const char *file_path)
{
    int activity_admitted = 0;
    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: " COVER_HOST "\r\n"
             "User-Agent: PSP\r\n"
             "Accept: */*\r\n"
             "Connection: keep-alive\r\n"
             "\r\n",
             path);
    request[sizeof(request) - 1] = '\0';

    logLine("cover_http: GET %s\n", path);

    if (tls_write_all(&client->conn, request, strlen(request)) != 0) {
        logLine("cover_http: write failed\n");
        return -1;
    }

    // Read headers into a heap buffer to avoid stack pressure on PSP.
    char *hdr_buf = (char *)malloc((size_t)HDR_BUF_SIZE + 1);
    if (!hdr_buf) {
        logLine("cover_http: hdr_buf alloc failed\n");
        return -1;
    }
    int hdr_len = 0;
    int hdr_done = 0;

    while (!hdr_done && hdr_len < HDR_BUF_SIZE) {
        int space = HDR_BUF_SIZE - hdr_len;
        int rc = net_tls_read(&client->conn, (unsigned char *)hdr_buf + hdr_len, (size_t)space);
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        }
        if (rc < 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                sceKernelDelayThread(5 * 1000);
                continue;
            }
            logLine("cover_http: hdr read failed %d\n", rc);
            free(hdr_buf);
            return -1;
        }
        hdr_len += rc;
        hdr_buf[hdr_len] = '\0';
        if (strstr(hdr_buf, "\r\n\r\n")) {
            hdr_done = 1;
        }
    }

    if (!hdr_done) {
        logLine("cover_http: headers not complete (hdr_len=%d)\n", hdr_len);
        free(hdr_buf);
        return -1;
    }

    // Parse status line.
    int status = 0;
    if (sscanf(hdr_buf, "HTTP/%*s %d", &status) != 1) {
        status = 0;
    }

    // Parse Content-Length and Connection: close.
    int content_length = -1;
    int conn_close = 0;
    {
        const char *p = hdr_buf;
        const char *header_end = strstr(hdr_buf, "\r\n\r\n");
        const char *end = header_end ? header_end : hdr_buf + hdr_len;
        // Skip status line.
        const char *nl = strstr(p, "\r\n");
        if (nl) p = nl + 2;
        while (p < end) {
            const char *next = strstr(p, "\r\n");
            if (!next || next == p) break;
            size_t line_len = (size_t)(next - p);
            if (line_len >= 14 && strncasecmp(p, "Content-Length", 14) == 0) {
                const char *colon = (const char *)memchr(p, ':', line_len);
                if (colon) content_length = atoi(colon + 1);
            } else if (line_len >= 10 && strncasecmp(p, "Connection", 10) == 0) {
                const char *colon = (const char *)memchr(p, ':', line_len);
                if (colon) {
                    const char *val = colon + 1;
                    while (*val == ' ' || *val == '\t') val++;
                    if (strncasecmp(val, "close", 5) == 0) {
                        conn_close = 1;
                    }
                }
            }
            p = next + 2;
        }
    }

    logLine("cover_http: status=%d content_length=%d conn_close=%d\n",
            status, content_length, conn_close);

    if (status < 200 || status >= 300) {
        logLine("cover_http: bad status %d\n", status);
        free(hdr_buf);
        return -1;
    }
    if (content_length < 0) {
        logLine("cover_http: no Content-Length, refusing\n");
        free(hdr_buf);
        return -1;
    }

    // Body bytes already in hdr_buf after the header block.
    const char *header_end_ptr = strstr(hdr_buf, "\r\n\r\n");
    const char *body_ptr = header_end_ptr ? header_end_ptr + 4 : hdr_buf + hdr_len;
    int body_tail = (int)(hdr_buf + hdr_len - body_ptr);
    if (body_tail < 0) body_tail = 0;
    if (body_tail > content_length) body_tail = content_length;

    fs_ensure_dir(file_path);
    SceUID fd = fs_open(file_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        logLine("cover_http: open failed '%s' 0x%08X\n", file_path, fd);
        free(hdr_buf);
        return -1;
    }

    int total_written = 0;
    int ret = -1;

    if (body_tail > 0) {
        if (write_fully(fd, body_ptr, body_tail) != 0) {
            logLine("cover_http: write failed (header tail)\n");
            goto done;
        }
        total_written += body_tail;
        net_activity_begin();
        activity_admitted = 1;
        net_activity_body_bytes(body_tail);
    }

    // Stream remaining body bytes.
    {
        char read_buf[READ_BUF_SIZE];
        while (total_written < content_length) {
            int want = READ_BUF_SIZE;
            int remain = content_length - total_written;
            if (remain < want) want = remain;
            int rc = net_tls_read(&client->conn, (unsigned char *)read_buf, (size_t)want);
            if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                logLine("cover_http: connection closed before body complete (%d/%d)\n",
                        total_written, content_length);
                goto done;
            }
            if (rc < 0) {
                if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    sceKernelDelayThread(5 * 1000);
                    continue;
                }
                logLine("cover_http: body read failed %d\n", rc);
                goto done;
            }
            if (write_fully(fd, read_buf, rc) != 0) {
                logLine("cover_http: write failed (body chunk)\n");
                goto done;
            }
            total_written += rc;
            if (!activity_admitted) {
                net_activity_begin();
                activity_admitted = 1;
            }
            net_activity_body_bytes(rc);
        }
    }

    logLine("cover_http: body written=%d\n", total_written);
    ret = 0;

done:
    if (activity_admitted) net_activity_end();
    free(hdr_buf);
    if (ret == 0 && !net_tls_connection_is_current(&client->conn)) {
        ret = -1;
    }
    fs_close(fd);
    if (ret != 0) {
        logLine("cover_http: removing incomplete file '%s'\n", file_path);
        fs_remove(file_path);
    }
    if (conn_close) {
        client_disconnect(client, "server Connection: close");
    }
    return ret;
}

void cover_http_client_init(CoverHttpClient *client)
{
    memset(client, 0, sizeof(CoverHttpClient));
    client->connected = 0;
}

void cover_http_client_shutdown(CoverHttpClient *client)
{
    client_disconnect(client, "shutdown");
}

int cover_http_client_download(CoverHttpClient *client,
                               const char *url,
                               const char *file_path)
{
    if (!client || !url || !file_path) return -1;

    // Only handle avatars.yandex.net.
    const char *prefix = "https://" COVER_HOST "/";
    if (strncmp(url, prefix, strlen(prefix)) != 0) {
        logLine("cover_http: unsupported host in url '%s'\n", url);
        return -1;
    }

    // Extract path (starts with '/').
    const char *path = url + strlen("https://" COVER_HOST);

    if (client->connected &&
        client->conn.network_generation != net_stack_get_generation()) {
        client_disconnect(client, "stale network generation");
    }

    // Ensure connection is up; reconnect if not.
    if (!client->connected) {
        if (client_connect(client) != 0) return -1;
    } else {
        logLine("cover_http: reusing connection\n");
    }

    int rc = do_download(client, path, file_path);
    if (rc != 0 && client->connected) {
        // Error on existing connection — disconnect and retry once.
        client_disconnect(client, "download error, retry");
        logLine("cover_http: retry after reconnect\n");
        if (client_connect(client) != 0) return -1;
        rc = do_download(client, path, file_path);
        if (rc != 0) {
            client_disconnect(client, "retry failed");
        }
    }
    return rc;
}
