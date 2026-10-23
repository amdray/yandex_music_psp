#include "services/net_tls.h"

#include <pspsdk.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspiofilemgr.h>
#include <pspsysmem.h>
#include <psputility.h>
#include <pspwlan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <mbedtls/error.h>
#include <mbedtls/debug.h>

#include "core/logger.h"
#include "services/dns.h"
#include "services/net_poll.h"
#include "services/net_stack.h"
#include "services/systemctrl_rng.h"

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long)0xFFFFFFFF)
#endif

#ifndef SO_RCVBUF
#define SO_RCVBUF 0x1002
#endif
#define NET_TLS_RCVBUF_BYTES (64 * 1024)
#ifndef SO_NONBLOCK
#define SO_NONBLOCK 0x1009
#endif

// Верхняя граница «тишины» одной операции чтения/записи. Сокет неблокирующий,
// bio сам пейсит повторы и по истечении окна возвращает жёсткую ошибку.
#define NET_TLS_IO_TIMEOUT_S 15

// Статическое состояние (только TLS — сеть и DNS вынесены в net_stack/dns)
static mbedtls_ssl_config s_tls_conf;
static int s_tls_conf_ready = 0;
static int s_verbose_logging = 0;
// Binary semaphore: only one TLS handshake at a time across all threads.
// PSP's mbedtls_ssl_handshake is not re-entrant when threads share s_tls_conf.
// Initial value = 1 (free); connect waits, disconnect signals.
static SceUID s_tls_handshake_sema = -1;

static const NetTlsConfig s_default_tls_config = {
    .verify_mode = MBEDTLS_SSL_VERIFY_NONE,
    .debug_threshold = 0,
    .enable_verbose_logging = 0
};

// Вспомогательные функции
static void log_mbedtls_error(const char *label, int err);
static void format_ipv4(const struct in_addr *addr, char *out, size_t out_size);
static void tls_connection_release(NetTlsConnection *conn);
static int dns_cancel_current_thread(void *ctx);

static void invalidate_current_address(const char *host,
                                       const struct in_addr *addr,
                                       int cacheable,
                                       unsigned int generation)
{
    NetStackSnapshot link;
    if (!cacheable) return;
    if (net_stack_get_snapshot(&link) < 0) return;
    if (link.apctl_state == PSP_NET_APCTL_STATE_GOT_IP &&
        link.generation == generation)
        dns_invalidate(host, addr);
}

static int link_generation_is_current(unsigned int generation)
{
    NetStackSnapshot link;
    return net_stack_get_snapshot(&link) == 0 &&
           link.apctl_state == PSP_NET_APCTL_STATE_GOT_IP &&
           link.generation == generation;
}

static int link_capture_online_generation(unsigned int *out_generation)
{
    NetStackSnapshot link;
    if (!out_generation || net_stack_get_snapshot(&link) < 0 ||
        link.apctl_state != PSP_NET_APCTL_STATE_GOT_IP)
        return -1;
    *out_generation = link.generation;
    return 0;
}

// Кастомные I/O функции для mbedTLS
static int psp_net_send(void *ctx, const unsigned char *buf, size_t len);
static int psp_net_recv(void *ctx, unsigned char *buf, size_t len);

// Debug callback для mbedTLS
static void mbedtls_debug_cb(void *ctx, int level, const char *file, int line, const char *str);

// Сокет принадлежит только этому модулю: сырой sce-дескриптор закрывается
// исключительно через sceNetInetClose. mbedtls_net_free здесь запрещён — его
// close() трактует сырой sce-номер как индекс fd-таблицы libcglue и закрывает
// чужой дескриптор (mbedtls_net_context — это только int fd, утечек нет).
/* Возобновление TLS-сессий (session ticket / session id): полное рукопожатие
 * на PSP стоит ~1-1.5 с (RSA/ECDHE на 333 МГц + RTT), возобновление — один
 * круг RTT. Кэш на несколько хостов (api, обложки, CDN); все обращения идут
 * под s_tls_handshake_sema (рукопожатия и так сериализованы). Если сервер
 * отверг сессию, mbedtls сам делает полное рукопожатие. */
#define TLS_SESS_SLOTS 4
#define TLS_SESS_MAX_AGE_US (20ULL * 60ULL * 1000000ULL)
typedef struct {
    char host[64];
    mbedtls_ssl_session sess;
    int used;
    u64 saved_us;
} TlsSessSlot;
static TlsSessSlot s_sess[TLS_SESS_SLOTS];

static void tls_sess_try_resume(mbedtls_ssl_context *ssl, const char *host)
{
    int i;
    u64 now = sceKernelGetSystemTimeWide();
    for (i = 0; i < TLS_SESS_SLOTS; i++) {
        if (s_sess[i].used && strcmp(s_sess[i].host, host) == 0) {
            if (now - s_sess[i].saved_us > TLS_SESS_MAX_AGE_US) {
                mbedtls_ssl_session_free(&s_sess[i].sess);
                s_sess[i].used = 0;
                return;
            }
            if (mbedtls_ssl_set_session(ssl, &s_sess[i].sess) == 0) {
                logLine("tls: resume offered host=%s\n", host);
            }
            return;
        }
    }
}

static void tls_sess_store(mbedtls_ssl_context *ssl, const char *host)
{
    int i, slot = -1;
    u64 oldest = ~0ULL;
    for (i = 0; i < TLS_SESS_SLOTS; i++) {
        if (s_sess[i].used && strcmp(s_sess[i].host, host) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        for (i = 0; i < TLS_SESS_SLOTS; i++) {
            if (!s_sess[i].used) { slot = i; break; }
            if (s_sess[i].saved_us < oldest) { oldest = s_sess[i].saved_us; slot = i; }
        }
    }
    if (slot < 0) return;
    mbedtls_ssl_session_free(&s_sess[slot].sess);
    mbedtls_ssl_session_init(&s_sess[slot].sess);
    s_sess[slot].used = 0;
    if (mbedtls_ssl_get_session(ssl, &s_sess[slot].sess) == 0) {
        snprintf(s_sess[slot].host, sizeof(s_sess[slot].host), "%s", host);
        s_sess[slot].saved_us = sceKernelGetSystemTimeWide();
        s_sess[slot].used = 1;
    }
}

static void tls_sess_clear_all(void)
{
    int i;
    for (i = 0; i < TLS_SESS_SLOTS; i++) {
        mbedtls_ssl_session_free(&s_sess[i].sess);
        s_sess[i].used = 0;
    }
}

static void tls_conn_cleanup(NetTlsConnection *conn)
{
    int fd;
    if (!conn) return;
    fd = conn->socket_fd;
    conn->socket_fd = -1;
    conn->net.fd = -1;
    if (fd >= 0) sceNetInetClose(fd);
    mbedtls_ssl_free(&conn->ssl);
    tls_connection_release(conn);
}

// Реестр отмены: поток регистрирует флаг для себя, bio-колбэки и циклы
// ожидания читают только запись своего tid — записи чужих потоков не трогаются,
// поэтому чтение без блокировки безопасно.
#define TLS_CANCEL_SLOTS 16
typedef struct {
    volatile int tid;
    volatile int *flag;
} TlsCancelEntry;
static TlsCancelEntry s_cancel_table[TLS_CANCEL_SLOTS];
static SceLwMutexWorkarea s_cancel_mutex;
static int s_cancel_mutex_ready = 0;

#define TLS_CONNECTION_SLOTS 16
static NetTlsConnection *s_connections[TLS_CONNECTION_SLOTS];
static SceLwMutexWorkarea s_connections_mutex;
static int s_connections_mutex_ready = 0;
static int s_connection_admission_open = 0;

static int tls_connection_register(NetTlsConnection *conn)
{
    int i;

    if (!s_connections_mutex_ready) return -1;
    sceKernelLockLwMutex(&s_connections_mutex, 1, NULL);
    if (!s_connection_admission_open) {
        sceKernelUnlockLwMutex(&s_connections_mutex, 1);
        return -1;
    }
    for (i = 0; i < TLS_CONNECTION_SLOTS; i++) {
        if (!s_connections[i]) {
            s_connections[i] = conn;
            conn->network_lease_held = 1;
            sceKernelUnlockLwMutex(&s_connections_mutex, 1);
            return 0;
        }
    }
    sceKernelUnlockLwMutex(&s_connections_mutex, 1);
    logLine("tls: connection table full\n");
    return -1;
}

static void tls_connection_release(NetTlsConnection *conn)
{
    int i;

    if (s_connections_mutex_ready)
        sceKernelLockLwMutex(&s_connections_mutex, 1, NULL);
    if (conn) {
        if (conn->network_lease_held) {
            for (i = 0; i < TLS_CONNECTION_SLOTS; i++) {
                if (s_connections[i] == conn) {
                    s_connections[i] = NULL;
                    break;
                }
            }
            conn->network_lease_held = 0;
        }
    }
    if (s_connections_mutex_ready)
        sceKernelUnlockLwMutex(&s_connections_mutex, 1);
}

void net_tls_quiesce_begin(void)
{
    if (!s_connections_mutex_ready) return;
    sceKernelLockLwMutex(&s_connections_mutex, 1, NULL);
    s_connection_admission_open = 0;
    sceKernelUnlockLwMutex(&s_connections_mutex, 1);
}

int net_tls_quiesce_wait(unsigned int timeout_us)
{
    u64 deadline = sceKernelGetSystemTimeWide() + (u64)timeout_us;
    int active;

    if (!s_connections_mutex_ready) return 0;
    do {
        int i;
        active = 0;
        sceKernelLockLwMutex(&s_connections_mutex, 1, NULL);
        for (i = 0; i < TLS_CONNECTION_SLOTS; i++) {
            if (s_connections[i]) active++;
        }
        sceKernelUnlockLwMutex(&s_connections_mutex, 1);
        if (active == 0) return 0;
        sceKernelDelayThread(5 * 1000);
    } while ((u64)sceKernelGetSystemTimeWide() < deadline);

    logLine("tls: quiesce timeout active=%d\n", active);
    logger_flush();
    return -1;
}

void net_tls_quiesce_end(void)
{
    if (!s_connections_mutex_ready) return;
    sceKernelLockLwMutex(&s_connections_mutex, 1, NULL);
    s_connection_admission_open = 1;
    sceKernelUnlockLwMutex(&s_connections_mutex, 1);
}

void net_tls_cancel_bind(volatile int *flag)
{
    int i;
    int tid = sceKernelGetThreadId();
    if (!flag) return;
    if (s_cancel_mutex_ready) sceKernelLockLwMutex(&s_cancel_mutex, 1, NULL);
    for (i = 0; i < TLS_CANCEL_SLOTS; i++) {
        if (s_cancel_table[i].tid == tid) {
            s_cancel_table[i].flag = flag;
            if (s_cancel_mutex_ready) sceKernelUnlockLwMutex(&s_cancel_mutex, 1);
            return;
        }
    }
    for (i = 0; i < TLS_CANCEL_SLOTS; i++) {
        if (s_cancel_table[i].tid == 0) {
            s_cancel_table[i].flag = flag;
            s_cancel_table[i].tid = tid;
            if (s_cancel_mutex_ready) sceKernelUnlockLwMutex(&s_cancel_mutex, 1);
            return;
        }
    }
    if (s_cancel_mutex_ready) sceKernelUnlockLwMutex(&s_cancel_mutex, 1);
    logLine("tls: cancel table full, tid=%d unbound\n", tid);
}

void net_tls_cancel_unbind(void)
{
    int i;
    int tid = sceKernelGetThreadId();
    if (s_cancel_mutex_ready) sceKernelLockLwMutex(&s_cancel_mutex, 1, NULL);
    for (i = 0; i < TLS_CANCEL_SLOTS; i++) {
        if (s_cancel_table[i].tid == tid) {
            s_cancel_table[i].tid = 0;
            s_cancel_table[i].flag = NULL;
            if (s_cancel_mutex_ready) sceKernelUnlockLwMutex(&s_cancel_mutex, 1);
            return;
        }
    }
    if (s_cancel_mutex_ready) sceKernelUnlockLwMutex(&s_cancel_mutex, 1);
}

static int tls_cancel_pending(void)
{
    int i;
    int tid = sceKernelGetThreadId();
    for (i = 0; i < TLS_CANCEL_SLOTS; i++) {
        if (s_cancel_table[i].tid == tid) {
            volatile int *f = s_cancel_table[i].flag;
            return f ? (*f != 0) : 0;
        }
    }
    return 0;
}

// Инициализация TLS конфигурации
int net_tls_config_init(const NetTlsConfig *config)
{
    const NetTlsConfig *cfg = config ? config : &s_default_tls_config;
    int err;

    logLine("tls: rng source ARK-4 sctrlKernelRand\n");

    mbedtls_ssl_config_init(&s_tls_conf);
    err = mbedtls_ssl_config_defaults(
        &s_tls_conf,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT
    );
    if (err != 0) {
        logLine("tls: config defaults failed %d\n", err);
        mbedtls_ssl_config_free(&s_tls_conf);
        return -1;
    }

    mbedtls_ssl_conf_authmode(&s_tls_conf, cfg->verify_mode);
    logLine("tls: authmode %s\n", cfg->verify_mode == MBEDTLS_SSL_VERIFY_NONE ? "VERIFY_NONE" : "VERIFY_REQUIRED");
    mbedtls_ssl_conf_rng(&s_tls_conf, systemctrl_rng, NULL);
    mbedtls_ssl_conf_dbg(&s_tls_conf, mbedtls_debug_cb, NULL);
    mbedtls_debug_set_threshold(cfg->debug_threshold);

    s_tls_handshake_sema = sceKernelCreateSema("tls_hs", 0, 1, 1, NULL);
    if (s_tls_handshake_sema < 0) {
        logLine("tls: sema create failed 0x%08X\n", s_tls_handshake_sema);
        mbedtls_ssl_config_free(&s_tls_conf);
        return -1;
    }
    err = sceKernelCreateLwMutex(&s_cancel_mutex, "tls_cancel", 0, 0, NULL);
    if (err < 0) {
        logLine("tls: cancel mutex create failed 0x%08X\n", err);
        sceKernelDeleteSema(s_tls_handshake_sema);
        s_tls_handshake_sema = -1;
        mbedtls_ssl_config_free(&s_tls_conf);
        return -1;
    }
    s_cancel_mutex_ready = 1;
    err = sceKernelCreateLwMutex(&s_connections_mutex, "tls_connections", 0, 0, NULL);
    if (err < 0) {
        logLine("tls: connections mutex create failed 0x%08X\n", err);
        sceKernelDeleteLwMutex(&s_cancel_mutex);
        s_cancel_mutex_ready = 0;
        sceKernelDeleteSema(s_tls_handshake_sema);
        s_tls_handshake_sema = -1;
        mbedtls_ssl_config_free(&s_tls_conf);
        return -1;
    }
    memset(s_connections, 0, sizeof(s_connections));
    s_connection_admission_open = 1;
    s_connections_mutex_ready = 1;
    logLine("tls: handshake sema created\n");

    s_verbose_logging = cfg->enable_verbose_logging;
    s_tls_conf_ready = 1;
    return 0;
}

int net_tls_config_shutdown(void)
{
    logLine("tls: teardown start\n");
    net_tls_quiesce_begin();
    if (net_tls_quiesce_wait(3000000U) < 0) {
        logLine("tls: active owners remain; TLS runtime retained\n");
        return -1;
    }
    tls_sess_clear_all();
    if (s_tls_conf_ready) {
        mbedtls_ssl_config_free(&s_tls_conf);
        s_tls_conf_ready = 0;
    }
    if (s_tls_handshake_sema >= 0) {
        sceKernelDeleteSema(s_tls_handshake_sema);
        s_tls_handshake_sema = -1;
    }
    if (s_cancel_mutex_ready) {
        sceKernelDeleteLwMutex(&s_cancel_mutex);
        s_cancel_mutex_ready = 0;
    }
    if (s_connections_mutex_ready) {
        sceKernelDeleteLwMutex(&s_connections_mutex);
        s_connections_mutex_ready = 0;
    }
    logLine("tls: teardown done\n");
    return 0;
}

int net_tls_config_is_ready(void)
{
    return s_tls_conf_ready;
}

// Установка TLS соединения
int net_tls_connect(const char *hostname, int port, NetTlsConnection *out_conn)
{
    int err;
    int sock = -1;
    int address_cacheable = 0;
    unsigned int network_generation;
    struct in_addr addr;
    struct sockaddr_in sa;

    if (!s_tls_conf_ready) {
        logLine("tls: conf not ready\n");
        return NET_TLS_ERR_INVALID;
    }

    if (!hostname || !out_conn) {
        return NET_TLS_ERR_INVALID;
    }
    memset(out_conn, 0, sizeof(*out_conn));
    out_conn->socket_fd = -1;
    out_conn->net.fd = -1;
    mbedtls_ssl_init(&out_conn->ssl);
    if (link_capture_online_generation(&network_generation) < 0) {
        logLine("tls: network not online\n");
        mbedtls_ssl_free(&out_conn->ssl);
        return NET_TLS_ERR_OFFLINE;
    }
    if (tls_connection_register(out_conn) != 0) {
        mbedtls_ssl_free(&out_conn->ssl);
        return NET_TLS_ERR_OFFLINE;
    }
    logLine("tls: connect %s\n", hostname);
    err = dns_resolve(hostname, &addr, &address_cacheable,
                      dns_cancel_current_thread, NULL);
    if (err != 0) {
        logLine("tls: resolve failed rc=%d\n", err);
        tls_conn_cleanup(out_conn);
        if (err == DNS_ERR_OFFLINE) return NET_TLS_ERR_OFFLINE;
        if (err == DNS_ERR_CANCELLED) return NET_TLS_ERR_CANCELLED;
        return NET_TLS_ERR_DNS;
    }
    if (!link_generation_is_current(network_generation)) {
        tls_conn_cleanup(out_conn);
        return NET_TLS_ERR_OFFLINE;
    }
    if (addr.s_addr == 0 || addr.s_addr == INADDR_NONE) {
        logLine("tls: resolve invalid addr 0x%08X\n", (unsigned int)addr.s_addr);
        tls_conn_cleanup(out_conn);
        return NET_TLS_ERR_DNS;
    }
    if (s_verbose_logging) {
        char ip_str[16];
        format_ipv4(&addr, ip_str, sizeof(ip_str));
        logLine("tls: resolved %s -> %s\n", hostname, ip_str);
    }

    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        logLine("tls: socket failed errno=%d\n", sceNetInetGetErrno());
        logger_flush();
        tls_conn_cleanup(out_conn);
        return NET_TLS_ERR_CONNECT;
    }
    out_conn->socket_fd = sock;
    out_conn->net.fd = sock;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;

    if (s_verbose_logging) {
        char ip_str[16];
        format_ipv4(&addr, ip_str, sizeof(ip_str));
        logLine("tls: connect %s:%d\n", ip_str, port);
    }
    
    {
        int nb = 1;
        int so_rc = sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));
        if (so_rc != 0) logLine("tls: set SO_NONBLOCK rc=%d\n", so_rc);
        if (so_rc < 0) {
            tls_conn_cleanup(out_conn);
            return NET_TLS_ERR_CONNECT;
        }
    }
    {
        /* Окно приёма TCP: по умолчанию на PSP мало (~16 КБ), а RTT до CDN
         * 50-100 мс => потолок window/RTT ~150 КБ/с (так и было в тесте
         * скорости, полка 142-148 КБ/с). 64 КБ поднимает потолок >500 КБ/с,
         * выше всё равно упирается Wi-Fi 802.11b консоли. */
        int rcv = NET_TLS_RCVBUF_BYTES;
        static int s_rcvbuf_logged = 0;
        int so_rc = sceNetInetSetsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
        if (!s_rcvbuf_logged) {
            int got = 0;
            socklen_t gl = sizeof(got);
            int gr = sceNetInetGetsockopt(sock, SOL_SOCKET, SO_RCVBUF, &got, &gl);
            s_rcvbuf_logged = 1;
            logLine("tls: SO_RCVBUF set=%d rc=%d readback=%d rc=%d\n",
                    rcv, so_rc, got, gr);
        }
    }

    {
        u64 connect_start = sceKernelGetSystemTimeWide();
        int connect_rc = sceNetInetConnect(sock, (struct sockaddr *)&sa, sizeof(sa));
        if (connect_rc < 0) {
            int connect_errno = sceNetInetGetErrno();
            if (connect_errno != EINPROGRESS && connect_errno != EAGAIN &&
                connect_errno != EWOULDBLOCK) {
                logLine("tls: connect failed errno=%d\n", connect_errno);
                invalidate_current_address(hostname, &addr, address_cacheable,
                                           network_generation);
                tls_conn_cleanup(out_conn);
                return NET_TLS_ERR_CONNECT;
            }

            for (;;) {
                struct SceNetInetPollfd poll_fd;
                int poll_rc;
                memset(&poll_fd, 0, sizeof(poll_fd));
                poll_fd.fd = sock;
                poll_fd.events = SCE_NET_INET_POLLOUT;
                poll_rc = sceNetInetPoll(&poll_fd, 1, 200);
                if (!link_generation_is_current(network_generation)) {
                    tls_conn_cleanup(out_conn);
                    return NET_TLS_ERR_OFFLINE;
                }
                if (tls_cancel_pending()) {
                    tls_conn_cleanup(out_conn);
                    return NET_TLS_ERR_CANCELLED;
                }
                if (poll_rc > 0) {
                    int socket_error = 0;
                    socklen_t error_len = sizeof(socket_error);
                    if (sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR,
                                             &socket_error, &error_len) < 0 ||
                        socket_error != 0 ||
                        (poll_fd.revents & (SCE_NET_INET_POLLERR |
                                            SCE_NET_INET_POLLHUP |
                                            SCE_NET_INET_POLLNVAL)) ||
                        !(poll_fd.revents & SCE_NET_INET_POLLOUT)) {
                        logLine("tls: async connect failed so_error=%d revents=0x%X\n",
                                socket_error, (unsigned int)poll_fd.revents);
                        invalidate_current_address(hostname, &addr,
                                                   address_cacheable,
                                                   network_generation);
                        tls_conn_cleanup(out_conn);
                        return NET_TLS_ERR_CONNECT;
                    }
                    break;
                }
                if (poll_rc < 0 ||
                    (u64)sceKernelGetSystemTimeWide() - connect_start >= 10000000ULL) {
                    int timeout_errno = sceNetInetGetErrno();
                    logLine("tls: connect timeout/error poll_rc=%d errno=%d\n",
                            poll_rc, timeout_errno);
                    invalidate_current_address(hostname, &addr,
                                               address_cacheable,
                                               network_generation);
                    tls_conn_cleanup(out_conn);
                    return NET_TLS_ERR_CONNECT;
                }
            }
        }
        logLine("tls: connected fd=%d (took %llu ms)\n", sock,
                (sceKernelGetSystemTimeWide() - connect_start) / 1000);
        logger_flush();
    }

    if (!link_generation_is_current(network_generation)) {
        tls_conn_cleanup(out_conn);
        return NET_TLS_ERR_OFFLINE;
    }

    /* mbedtls_net_init не вызывать: это единственная ссылка в net_sockets.o,
     * который тянет BSD-обёртки libcglue и рвёт группы импорт-стабов
     * (fixup-imports "stubs out of order"). Контекст — это один int fd. */
    out_conn->network_generation = network_generation;
    out_conn->recv_deadline_us = 0;
    out_conn->send_deadline_us = 0;
    // Serialise handshakes: wait for exclusive access to s_tls_conf.
    // Total budget 30 s (cover_manager download can take up to ~10 s), sliced
    // into 200 ms waits so a bound cancel flag interrupts within one slice.
    if (s_tls_handshake_sema >= 0) {
        int waited_us = 0;
        int sema_rc;
        for (;;) {
            SceUInt slice_us = 200000U;
            sema_rc = sceKernelWaitSema(s_tls_handshake_sema, 1, &slice_us);
            if (sema_rc >= 0) break;
            waited_us += 200000;
            if (!link_generation_is_current(network_generation)) {
                tls_conn_cleanup(out_conn);
                return NET_TLS_ERR_OFFLINE;
            }
            if (tls_cancel_pending()) {
                logLine("tls: sema wait cancelled\n");
                logger_flush();
                tls_conn_cleanup(out_conn);
                return -1;
            }
            if (waited_us >= 30000000) {
                logLine("tls: sema wait failed 0x%08X\n", sema_rc);
                logger_flush();
                tls_conn_cleanup(out_conn);
                return -1;
            }
        }
    }

    err = mbedtls_ssl_setup(&out_conn->ssl, &s_tls_conf);
    if (err != 0) {
        log_mbedtls_error("tls: ssl_setup failed", err);
        logger_flush();
        if (s_tls_handshake_sema >= 0) sceKernelSignalSema(s_tls_handshake_sema, 1);
        tls_conn_cleanup(out_conn);
        return -1;
    }
    err = mbedtls_ssl_set_hostname(&out_conn->ssl, hostname);
    if (err != 0) {
        log_mbedtls_error("tls: set_hostname failed", err);
        logger_flush();
        if (s_tls_handshake_sema >= 0) sceKernelSignalSema(s_tls_handshake_sema, 1);
        tls_conn_cleanup(out_conn);
        return -1;
    }
    mbedtls_ssl_set_bio(&out_conn->ssl, &out_conn->net, psp_net_send, psp_net_recv, NULL);
    tls_sess_try_resume(&out_conn->ssl, hostname);

    {
        int hs_iter = 0;
        int hs_want_read = 0;
        int hs_want_write = 0;
        u64 hs_start = sceKernelGetSystemTimeWide();
        int hs_tid = sceKernelGetThreadId();
        int hs_free = sceKernelMaxFreeMemSize();
        logLine("tls: hs starting tid=%d fd=%d free=%d\n", hs_tid, sock, hs_free);
        logger_flush();

        while ((err = mbedtls_ssl_handshake(&out_conn->ssl)) != 0) {
            if (err == MBEDTLS_ERR_SSL_WANT_READ || err == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (tls_cancel_pending()) {
                    logLine("tls: handshake cancelled iter=%d\n", hs_iter);
                    logger_flush();
                    if (s_tls_handshake_sema >= 0) sceKernelSignalSema(s_tls_handshake_sema, 1);
                    tls_conn_cleanup(out_conn);
                    return -1;
                }
                if ((u64)sceKernelGetSystemTimeWide() - hs_start >= 30000000ULL) {
                    logLine("tls: handshake timeout iter=%d\n", hs_iter);
                    logger_flush();
                    if (s_tls_handshake_sema >= 0)
                        sceKernelSignalSema(s_tls_handshake_sema, 1);
                    tls_conn_cleanup(out_conn);
                    return NET_TLS_ERR_HANDSHAKE;
                }
                hs_iter++;
                if (err == MBEDTLS_ERR_SSL_WANT_READ) {
                    hs_want_read++;
                } else {
                    hs_want_write++;
                }
                if (hs_iter % 300 == 0) {
                    u64 hs_elapsed_ms = (sceKernelGetSystemTimeWide() - hs_start) / 1000;
                    logLine("tls: hs alive iter=%d elapsed=%llu ms rd=%d wr=%d\n",
                            hs_iter, hs_elapsed_ms, hs_want_read, hs_want_write);
                    logger_flush();
                }
                /* WANT_READ уже ждал в poll внутри recv — лишний сон не нужен. */
                if (err == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    sceKernelDelayThread(2 * 1000);
                }
                continue;
            }
            log_mbedtls_error("tls: handshake failed", err);
            logger_flush();
            if (s_tls_handshake_sema >= 0) sceKernelSignalSema(s_tls_handshake_sema, 1);
            tls_conn_cleanup(out_conn);
            return -1;
        }

        tls_sess_store(&out_conn->ssl, hostname);
        // Handshake done; release semaphore so next connect can proceed.
        if (s_tls_handshake_sema >= 0) sceKernelSignalSema(s_tls_handshake_sema, 1);
        logLine("tls: handshake ok iter=%d rd=%d wr=%d took=%llu ms\n",
                hs_iter, hs_want_read, hs_want_write,
                (unsigned long long)((sceKernelGetSystemTimeWide() - hs_start) / 1000));
        logger_flush();
    }
    if (!net_tls_connection_is_current(out_conn)) {
        tls_conn_cleanup(out_conn);
        return NET_TLS_ERR_OFFLINE;
    }
    return 0;
}

static int dns_cancel_current_thread(void *ctx)
{
    (void)ctx;
    return tls_cancel_pending();
}

int net_tls_connection_is_current(const NetTlsConnection *conn)
{
    NetStackSnapshot state;
    if (!conn || !conn->network_lease_held) return 0;
    if (net_stack_get_snapshot(&state) < 0) return 0;
    return state.apctl_state == PSP_NET_APCTL_STATE_GOT_IP &&
           conn->network_generation == state.generation;
}

void net_tls_disconnect(NetTlsConnection *conn)
{
    if (!conn) {
        return;
    }

    logLine("tls: disconnect fd=%d\n", conn->socket_fd);
    logger_flush();
    conn->close_notify_active = 1;
    mbedtls_ssl_close_notify(&conn->ssl);
    conn->close_notify_active = 0;
    tls_conn_cleanup(conn);
    logger_flush();
}

// Чтение/запись через TLS
int net_tls_read(NetTlsConnection *conn, unsigned char *buf, size_t len)
{
    static int s_tls_read_count = 0;
    if (!conn) {
        return -1;
    }
    if (!link_generation_is_current(conn->network_generation)) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    s_tls_read_count++;
    if (s_tls_read_count == 1) {
        logLine("tls: FIRST net_tls_read call len=%d\n", (int)len);
        logger_flush();
    }
    return mbedtls_ssl_read(&conn->ssl, buf, len);
}

int net_tls_write(NetTlsConnection *conn, const unsigned char *buf, size_t len)
{
    static int s_tls_write_count = 0;
    if (!conn) {
        return -1;
    }
    if (!link_generation_is_current(conn->network_generation)) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    s_tls_write_count++;
    if (s_tls_write_count == 1) {
        logLine("tls: FIRST net_tls_write call len=%d\n", (int)len);
        logger_flush();
    }
    return mbedtls_ssl_write(&conn->ssl, buf, len);
}

// Вспомогательные функции
static void log_mbedtls_error(const char *label, int err)
{
    char errbuf[128];
    mbedtls_strerror(err, errbuf, sizeof(errbuf));
    logLine("%s: -0x%04X (%s)\n", label, (unsigned int)(-err), errbuf);
}

static void format_ipv4(const struct in_addr *addr, char *out, size_t out_size)
{
    unsigned int ip;
    if (!addr || !out || out_size == 0) {
        return;
    }
    ip = ntohl(addr->s_addr);
    snprintf(out, out_size, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF,
             (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF,
             ip & 0xFF);
    out[out_size - 1] = '\0';
}

static void mbedtls_debug_cb(void *ctx, int level, const char *file, int line, const char *str)
{
    (void)ctx;
    (void)level;
    logLine("mbedtls: %s:%d %s", file, line, str);
}

static int g_send_count = 0;

static int psp_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    NetTlsConnection *conn = (NetTlsConnection *)ctx;  // net — первое поле
    int cnt = ++g_send_count;
    int tid = sceKernelGetThreadId();

    if (!link_generation_is_current(conn->network_generation)) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    if (cnt <= 2) {
        /* Без logger_flush: синхронная запись на MS в горячем пути
         * отправки дёргала скорость скачивания. */
        logLine("tls: send#%d fd=%d len=%d tid=%d\n", cnt, conn->net.fd, (int)len, tid);
    }

    int ret = sceNetInetSend(conn->net.fd, buf, (int)len, 0);

    if (ret > 0) {
        conn->send_deadline_us = 0;  // прогресс
        return ret;
    }
    if (ret == 0) {
        logLine("tls: send got 0 send#%d tid=%d\n", cnt, tid);
        logger_flush();
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    int e = sceNetInetGetErrno();
    if (e == EAGAIN || e == EWOULDBLOCK) {
        u64 now = sceKernelGetSystemTimeWide();
        if (tls_cancel_pending()) {
            logLine("tls: send cancelled send#%d tid=%d\n", cnt, tid);
            logger_flush();
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
        if (conn->send_deadline_us == 0) {
            conn->send_deadline_us = now + (u64)NET_TLS_IO_TIMEOUT_S * 1000000ULL;
        } else if (now >= conn->send_deadline_us) {
            logLine("tls: send TIMEOUT (%ds) send#%d tid=%d\n", NET_TLS_IO_TIMEOUT_S, cnt, tid);
            logger_flush();
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
        sceKernelDelayThread(1 * 1000);
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (e == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (conn->close_notify_active) {
        logLine("tls: close_notify not sent errno=%d send#%d tid=%d\n",
                e, cnt, tid);
    } else {
        logLine("tls: send FAILED errno=%d send#%d tid=%d\n", e, cnt, tid);
    }
    logger_flush();
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int psp_net_recv(void *ctx, unsigned char *buf, size_t len)
{
    NetTlsConnection *conn = (NetTlsConnection *)ctx;  // net — первое поле
    int tid = sceKernelGetThreadId();

    if (!link_generation_is_current(conn->network_generation)) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    int ret = sceNetInetRecv(conn->net.fd, buf, (int)len, 0);

    if (ret > 0) {
        conn->recv_deadline_us = 0;  // прогресс: сбрасываем окно тишины
        return ret;
    }
    if (ret == 0) {
        logLine("tls: recv got 0 = conn reset tid=%d\n", tid);
        logger_flush();
        return MBEDTLS_ERR_NET_CONN_RESET;
    }

    int e = sceNetInetGetErrno();
    if (e == EAGAIN || e == EWOULDBLOCK) {
        // Данных пока нет на неблокирующем сокете. Первый EAGAIN логируем —
        // это доказательство, что неблокирующий режим реально включился.
        static int s_eagain_logged = 0;
        if (!s_eagain_logged) {
            s_eagain_logged = 1;
            logLine("tls: recv EAGAIN (non-blocking active) tid=%d\n", tid);
            logger_flush();
        }
        u64 now = sceKernelGetSystemTimeWide();
        if (tls_cancel_pending()) {
            logLine("tls: recv cancelled tid=%d\n", tid);
            logger_flush();
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        if (conn->recv_deadline_us == 0) {
            conn->recv_deadline_us = now + (u64)NET_TLS_IO_TIMEOUT_S * 1000000ULL;
        } else if (now >= conn->recv_deadline_us) {
            logLine("tls: recv TIMEOUT (%ds silent) tid=%d\n", NET_TLS_IO_TIMEOUT_S, tid);
            logger_flush();
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        /* Ждём данные в poll (просыпаемся сразу при пакете), а не спим
         * вслепую 1 мс на каждый пустой recv: меньше задержек и вызовов. */
        {
            struct SceNetInetPollfd pfd;
            pfd.fd = conn->net.fd;
            pfd.events = SCE_NET_INET_POLLIN;
            pfd.revents = 0;
            if (sceNetInetPoll(&pfd, 1, 20) < 0) {
                sceKernelDelayThread(1 * 1000);
            }
        }
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (e == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    logLine("tls: recv FAILED errno=%d tid=%d len=%d\n", e, tid, (int)len);
    logger_flush();
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
