#include "services/dns.h"

#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspthreadman.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#include "core/logger.h"
#include "services/dns_wire.h"
#include "services/net_poll.h"
#include "services/net_stack.h"
#include "services/systemctrl_rng.h"

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long)0xFFFFFFFF)
#endif
#ifndef SO_NONBLOCK
#define SO_NONBLOCK 0x1009
#endif

#define DNS_PORT 53
#define DNS_CACHE_SIZE 16
#define DNS_TOTAL_TIMEOUT_US 6000000ULL
#define DNS_SERVER_SLICE_US 2000000ULL
#define DNS_POLL_SLICE_US 100000U
#define DNS_UDP_RETRY_US 700000ULL
#define DNS_UDP_ATTEMPTS 3
#define DNS_MAX_CNAME_QUERIES 8
#define DNS_TTL_CLAMP_S 3600U

typedef struct DnsCacheEntry {
    int valid;
    char host[DNS_WIRE_MAX_NAME];
    struct in_addr addr;
    unsigned int generation;
    unsigned long long expires_us;
    unsigned int serial;
} DnsCacheEntry;

static DnsCacheEntry s_cache[DNS_CACHE_SIZE];
static SceLwMutexWorkarea s_cache_mutex;
static int s_cache_mutex_ready;
static unsigned int s_cache_serial;

static int ascii_fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int host_equal(const char *a, const char *b)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    size_t i;
    while (alen > 0 && a[alen - 1] == '.') --alen;
    while (blen > 0 && b[blen - 1] == '.') --blen;
    if (alen != blen) return 0;
    for (i = 0; i < alen; ++i)
        if (ascii_fold((unsigned char)a[i]) !=
            ascii_fold((unsigned char)b[i])) return 0;
    return 1;
}

static int request_cancelled(DnsCancelFn cancel, void *cancel_ctx)
{
    return cancel && cancel(cancel_ctx);
}

static int generation_current(unsigned int generation)
{
    NetStackSnapshot link;
    return net_stack_get_snapshot(&link) == 0 &&
           link.apctl_state == PSP_NET_APCTL_STATE_GOT_IP &&
           link.generation == generation;
}

static int cache_read(const char *host, unsigned int generation,
                      struct in_addr *addr)
{
    unsigned long long now = sceKernelGetSystemTimeWide();
    int hit = 0;
    int i;
    if (!s_cache_mutex_ready) return 0;
    sceKernelLockLwMutex(&s_cache_mutex, 1, NULL);
    for (i = 0; i < DNS_CACHE_SIZE; ++i) {
        if (!s_cache[i].valid) continue;
        if (s_cache[i].generation != generation ||
            now >= s_cache[i].expires_us) {
            s_cache[i].valid = 0;
            continue;
        }
        if (host_equal(s_cache[i].host, host)) {
            *addr = s_cache[i].addr;
            s_cache[i].serial = ++s_cache_serial;
            hit = 1;
            break;
        }
    }
    sceKernelUnlockLwMutex(&s_cache_mutex, 1);
    return hit;
}

static void cache_write(const char *host, const struct in_addr *addr,
                        unsigned int generation, unsigned int ttl)
{
    int slot = -1;
    int oldest_slot = -1;
    unsigned int oldest = 0xFFFFFFFFU;
    int i;
    size_t len;
    if (!s_cache_mutex_ready || ttl == 0) return;
    len = strlen(host);
    if (len > 0 && host[len - 1] == '.') --len;
    if (len == 0 || len >= DNS_WIRE_MAX_NAME) return;
    if (ttl > DNS_TTL_CLAMP_S) ttl = DNS_TTL_CLAMP_S;
    sceKernelLockLwMutex(&s_cache_mutex, 1, NULL);
    for (i = 0; i < DNS_CACHE_SIZE; ++i) {
        if (s_cache[i].valid && s_cache[i].generation == generation &&
            host_equal(s_cache[i].host, host)) {
            slot = i;
            break;
        }
        if (!s_cache[i].valid && slot < 0) slot = i;
        else if (s_cache[i].valid && s_cache[i].serial < oldest) {
            oldest = s_cache[i].serial;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot;
    if (slot >= 0) {
        memcpy(s_cache[slot].host, host, len);
        s_cache[slot].host[len] = '\0';
        s_cache[slot].addr = *addr;
        s_cache[slot].generation = generation;
        s_cache[slot].expires_us = sceKernelGetSystemTimeWide() +
                                   (unsigned long long)ttl * 1000000ULL;
        s_cache[slot].serial = ++s_cache_serial;
        s_cache[slot].valid = 1;
    }
    sceKernelUnlockLwMutex(&s_cache_mutex, 1);
}

static int wait_fd(int fd, int write_ready, unsigned long long deadline,
                   unsigned int generation, DnsCancelFn cancel,
                   void *cancel_ctx)
{
    for (;;) {
        struct SceNetInetPollfd poll_fd;
        unsigned long long now = sceKernelGetSystemTimeWide();
        unsigned long long remaining;
        int timeout_ms;
        int rc;
        if (request_cancelled(cancel, cancel_ctx)) return DNS_ERR_CANCELLED;
        if (!generation_current(generation)) return DNS_ERR_OFFLINE;
        if (now >= deadline) return DNS_ERR_TIMEOUT;
        remaining = deadline - now;
        if (remaining > DNS_POLL_SLICE_US) remaining = DNS_POLL_SLICE_US;
        timeout_ms = (int)((remaining + 999ULL) / 1000ULL);
        memset(&poll_fd, 0, sizeof(poll_fd));
        poll_fd.fd = fd;
        poll_fd.events = write_ready ? SCE_NET_INET_POLLOUT
                                     : SCE_NET_INET_POLLIN;
        rc = sceNetInetPoll(&poll_fd, 1, timeout_ms);
        if (rc > 0) {
            if (poll_fd.revents & poll_fd.events) return 0;
            if (poll_fd.revents & (SCE_NET_INET_POLLERR |
                                   SCE_NET_INET_POLLHUP |
                                   SCE_NET_INET_POLLNVAL))
                return DNS_ERR_LOOKUP;
            return DNS_ERR_LOOKUP;
        }
        if (rc < 0) return DNS_ERR_LOOKUP;
    }
}

static int make_socket(int type)
{
    int fd = sceNetInetSocket(AF_INET, type, 0);
    int nb = 1;
    if (fd < 0) return -1;
    if (sceNetInetSetsockopt(fd, SOL_SOCKET, SO_NONBLOCK,
                             &nb, sizeof(nb)) < 0) {
        sceNetInetClose(fd);
        return -1;
    }
    return fd;
}

static int server_sockaddr(const struct in_addr *server,
                           struct sockaddr_in *sa)
{
    if (!server || !sa || server->s_addr == 0 ||
        server->s_addr == INADDR_NONE) return -1;
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(DNS_PORT);
    sa->sin_addr = *server;
    return 0;
}

static int udp_exchange(const struct in_addr *server,
                        const unsigned char *query, size_t query_size,
                        uint16_t id, const char *host,
                        unsigned char *response, size_t *response_size,
                        unsigned long long deadline,
                        unsigned int generation, DnsCancelFn cancel,
                        void *cancel_ctx, DnsWireResult *result)
{
    struct sockaddr_in peer;
    int fd;
    int rc = DNS_ERR_LOOKUP;
    if (server_sockaddr(server, &peer) < 0) return DNS_ERR_LOOKUP;
    fd = make_socket(SOCK_DGRAM);
    if (fd < 0) return DNS_ERR_LOOKUP;
    {
        int attempt;
        for (attempt = 0; attempt < DNS_UDP_ATTEMPTS; ++attempt) {
            unsigned long long attempt_deadline;
            for (;;) {
                int sent;
                rc = wait_fd(fd, 1, deadline, generation, cancel, cancel_ctx);
                if (rc != 0) goto done;
                sent = (int)sceNetInetSendto(fd, query, query_size, 0,
                                            (struct sockaddr *)&peer,
                                            sizeof(peer));
                if (sent == (int)query_size) break;
                if (sent >= 0) { rc = DNS_ERR_LOOKUP; goto done; }
                {
                    int e = sceNetInetGetErrno();
                    if (e != EAGAIN && e != EWOULDBLOCK) {
                        rc = DNS_ERR_LOOKUP;
                        goto done;
                    }
                }
            }
            attempt_deadline = sceKernelGetSystemTimeWide() + DNS_UDP_RETRY_US;
            if (attempt_deadline > deadline) attempt_deadline = deadline;
            for (;;) {
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                int got;
                rc = wait_fd(fd, 0, attempt_deadline, generation,
                             cancel, cancel_ctx);
                if (rc == DNS_ERR_TIMEOUT) break;
                if (rc != 0) goto done;
                memset(&from, 0, sizeof(from));
                got = (int)sceNetInetRecvfrom(fd, response,
                                             DNS_WIRE_MAX_PACKET, 0,
                                             (struct sockaddr *)&from,
                                             &from_len);
                if (got < 0) {
                    int e = sceNetInetGetErrno();
                    if (e == EAGAIN || e == EWOULDBLOCK) continue;
                    rc = DNS_ERR_LOOKUP;
                    goto done;
                }
                if (from_len < sizeof(from) || from.sin_family != AF_INET ||
                    from.sin_port != htons(DNS_PORT) ||
                    from.sin_addr.s_addr != server->s_addr)
                    continue;
                *response_size = (size_t)got;
                rc = dns_wire_parse_response(response, *response_size,
                                             id, host, result);
                if (rc == DNS_WIRE_INVALID) continue;
                goto done;
            }
            if ((unsigned long long)sceKernelGetSystemTimeWide() >= deadline)
                break;
        }
        rc = DNS_ERR_TIMEOUT;
    }
done:
    sceNetInetClose(fd);
    return rc;
}

static int tcp_send_all(int fd, const unsigned char *buf, size_t size,
                        unsigned long long deadline, unsigned int generation,
                        DnsCancelFn cancel, void *cancel_ctx)
{
    size_t done = 0;
    while (done < size) {
        int rc = wait_fd(fd, 1, deadline, generation, cancel, cancel_ctx);
        int sent;
        if (rc != 0) return rc;
        sent = (int)sceNetInetSend(fd, buf + done, size - done, 0);
        if (sent > 0) done += (size_t)sent;
        else if (sent == 0) return DNS_ERR_LOOKUP;
        else {
            int e = sceNetInetGetErrno();
            if (e != EAGAIN && e != EWOULDBLOCK) return DNS_ERR_LOOKUP;
        }
    }
    return 0;
}

static int tcp_recv_all(int fd, unsigned char *buf, size_t size,
                        unsigned long long deadline, unsigned int generation,
                        DnsCancelFn cancel, void *cancel_ctx)
{
    size_t done = 0;
    while (done < size) {
        int rc = wait_fd(fd, 0, deadline, generation, cancel, cancel_ctx);
        int got;
        if (rc != 0) return rc;
        got = (int)sceNetInetRecv(fd, buf + done, size - done, 0);
        if (got > 0) done += (size_t)got;
        else if (got == 0) return DNS_ERR_LOOKUP;
        else {
            int e = sceNetInetGetErrno();
            if (e != EAGAIN && e != EWOULDBLOCK) return DNS_ERR_LOOKUP;
        }
    }
    return 0;
}

static int tcp_exchange(const struct in_addr *server,
                        const unsigned char *query, size_t query_size,
                        uint16_t id, const char *host,
                        unsigned char *response,
                        unsigned long long deadline,
                        unsigned int generation, DnsCancelFn cancel,
                        void *cancel_ctx, DnsWireResult *result)
{
    struct sockaddr_in peer;
    unsigned char length[2];
    int fd;
    int rc;
    int connect_rc;
    if (server_sockaddr(server, &peer) < 0 || query_size > 65535) return DNS_ERR_LOOKUP;
    fd = make_socket(SOCK_STREAM);
    if (fd < 0) return DNS_ERR_LOOKUP;
    connect_rc = sceNetInetConnect(fd, (struct sockaddr *)&peer, sizeof(peer));
    if (connect_rc < 0) {
        int e = sceNetInetGetErrno();
        if (e != EINPROGRESS && e != EAGAIN && e != EWOULDBLOCK) {
            rc = DNS_ERR_LOOKUP;
            goto done;
        }
        rc = wait_fd(fd, 1, deadline, generation, cancel, cancel_ctx);
        if (rc != 0) goto done;
        {
            int socket_error = 0;
            socklen_t error_len = sizeof(socket_error);
            if (sceNetInetGetsockopt(fd, SOL_SOCKET, SO_ERROR,
                                     &socket_error, &error_len) < 0 ||
                socket_error != 0) {
                rc = DNS_ERR_LOOKUP;
                goto done;
            }
        }
    }
    length[0] = (unsigned char)(query_size >> 8);
    length[1] = (unsigned char)query_size;
    rc = tcp_send_all(fd, length, 2, deadline, generation, cancel, cancel_ctx);
    if (rc == 0) rc = tcp_send_all(fd, query, query_size, deadline,
                                   generation, cancel, cancel_ctx);
    if (rc == 0) rc = tcp_recv_all(fd, length, 2, deadline, generation,
                                   cancel, cancel_ctx);
    if (rc == 0) {
        size_t response_size = ((size_t)length[0] << 8) | length[1];
        if (response_size < 12 || response_size > DNS_WIRE_MAX_PACKET)
            rc = DNS_ERR_LOOKUP;
        else
            rc = tcp_recv_all(fd, response, response_size, deadline,
                              generation, cancel, cancel_ctx);
        if (rc == 0)
            rc = dns_wire_parse_response(response, response_size,
                                         id, host, result);
    }
done:
    sceNetInetClose(fd);
    return rc;
}

static int get_dns_servers(unsigned int generation,
                           struct in_addr servers[2], int *out_count)
{
    NetStackSnapshot before;
    NetStackSnapshot after;
    union SceNetApctlInfo info;
    char text[16];
    int count = 0;
    int i;
    const int codes[2] = { PSP_NET_APCTL_INFO_PRIMDNS,
                           PSP_NET_APCTL_INFO_SECDNS };
    if (!out_count || net_stack_get_snapshot(&before) < 0 ||
        before.apctl_state != PSP_NET_APCTL_STATE_GOT_IP ||
        before.generation != generation) return DNS_ERR_OFFLINE;
    for (i = 0; i < 2; ++i) {
        memset(&info, 0, sizeof(info));
        if (sceNetApctlGetInfo(codes[i], &info) < 0) continue;
        memcpy(text, codes[i] == PSP_NET_APCTL_INFO_PRIMDNS
                         ? info.primaryDns : info.secondaryDns,
               sizeof(text));
        text[sizeof(text) - 1] = '\0';
        if (inet_aton(text, &servers[count]) == 0 ||
            servers[count].s_addr == 0 || servers[count].s_addr == INADDR_NONE)
            continue;
        if (count == 1 && servers[0].s_addr == servers[1].s_addr) continue;
        ++count;
    }
    if (net_stack_get_snapshot(&after) < 0 ||
        after.apctl_state != PSP_NET_APCTL_STATE_GOT_IP ||
        after.generation != generation) return DNS_ERR_OFFLINE;
    if (count == 0) return DNS_ERR_LOOKUP;
    *out_count = count;
    return 0;
}

int dns_init(void)
{
    int rc;
    memset(s_cache, 0, sizeof(s_cache));
    s_cache_serial = 0;
    rc = sceKernelCreateLwMutex(&s_cache_mutex, "dns_cache", 0, 0, NULL);
    if (rc < 0) return rc;
    s_cache_mutex_ready = 1;
    return 0;
}

int dns_shutdown(void)
{
    /* DNS state has process lifetime, matching APCTL/Inet. The application
     * exits immediately after consumer quiescence, so no shared object is
     * destroyed while a request might still be unwinding. */
    return 0;
}

int dns_resolve(const char *host, struct in_addr *out_addr,
                int *out_cacheable, DnsCancelFn cancel, void *cancel_ctx)
{
    struct in_addr servers[2];
    unsigned char query[DNS_WIRE_MAX_NAME + 32];
    unsigned char response[DNS_WIRE_MAX_PACKET];
    char current[DNS_WIRE_MAX_NAME];
    char cname_seen[DNS_MAX_CNAME_QUERIES][DNS_WIRE_MAX_NAME];
    unsigned int generation;
    unsigned int ttl_min = 0xFFFFFFFFU;
    unsigned long long total_deadline;
    int server_count;
    int cname_depth;
    NetStackSnapshot link;
    if (!host || !out_addr) return DNS_ERR_INVALID;
    if (out_cacheable) *out_cacheable = 0;
    if (inet_aton(host, out_addr) != 0) return 0;
    if (strlen(host) >= sizeof(current)) return DNS_ERR_INVALID;
    if (net_stack_get_snapshot(&link) < 0 ||
        link.apctl_state != PSP_NET_APCTL_STATE_GOT_IP) return DNS_ERR_OFFLINE;
    generation = link.generation;
    if (cache_read(host, generation, out_addr)) {
        if (out_cacheable) *out_cacheable = 1;
        logLine("dns: hit %s\n", host);
        return 0;
    }
    {
        int server_rc = get_dns_servers(generation, servers, &server_count);
        if (server_rc != 0) return server_rc;
    }
    strcpy(current, host);
    total_deadline = sceKernelGetSystemTimeWide() + DNS_TOTAL_TIMEOUT_US;
    logLine("dns: lookup %s servers=%d\n", host, server_count);
    for (cname_depth = 0; cname_depth < DNS_MAX_CNAME_QUERIES; ++cname_depth) {
        DnsWireResult parsed;
        uint16_t id;
        size_t query_size;
        int server_index;
        int rc = DNS_ERR_LOOKUP;
        int seen_index;
        for (seen_index = 0; seen_index < cname_depth; ++seen_index)
            if (host_equal(cname_seen[seen_index], current))
                return DNS_ERR_LOOKUP;
        strcpy(cname_seen[cname_depth], current);
        if (systemctrl_rng(NULL, (unsigned char *)&id, sizeof(id)) != 0)
            return DNS_ERR_LOOKUP;
        if (dns_wire_build_query(current, id, query, sizeof(query),
                                 &query_size) < 0) return DNS_ERR_INVALID;
        for (server_index = 0; server_index < server_count; ++server_index) {
            unsigned long long now = sceKernelGetSystemTimeWide();
            unsigned long long server_deadline;
            size_t response_size = 0;
            if (now >= total_deadline) return DNS_ERR_TIMEOUT;
            server_deadline = now + DNS_SERVER_SLICE_US;
            if (server_deadline > total_deadline) server_deadline = total_deadline;
            memset(&parsed, 0, sizeof(parsed));
            rc = udp_exchange(&servers[server_index], query, query_size,
                              id, current, response, &response_size,
                              server_deadline, generation, cancel, cancel_ctx,
                              &parsed);
            if (rc == DNS_WIRE_TRUNCATED)
                rc = tcp_exchange(&servers[server_index], query, query_size,
                                  id, current, response, total_deadline,
                                  generation, cancel, cancel_ctx, &parsed);
            if (rc == DNS_WIRE_OK || rc == DNS_WIRE_CNAME_ONLY) break;
            if (rc == DNS_WIRE_NXDOMAIN) return DNS_ERR_LOOKUP;
            if (rc == DNS_ERR_CANCELLED || rc == DNS_ERR_OFFLINE) return rc;
        }
        if (rc == DNS_WIRE_OK) {
            if (parsed.ttl < ttl_min) ttl_min = parsed.ttl;
            memcpy(&out_addr->s_addr, &parsed.ipv4_be, 4);
            if (out_addr->s_addr == 0 || out_addr->s_addr == INADDR_NONE)
                return DNS_ERR_LOOKUP;
            if (!generation_current(generation)) return DNS_ERR_OFFLINE;
            cache_write(host, out_addr, generation, ttl_min);
            if (out_cacheable) *out_cacheable = ttl_min != 0;
            logLine("dns: store %s ttl=%u\n", host, ttl_min);
            return 0;
        }
        if (rc != DNS_WIRE_CNAME_ONLY || !parsed.canonical_name[0])
            return rc == DNS_ERR_TIMEOUT ? rc : DNS_ERR_LOOKUP;
        if (parsed.ttl < ttl_min) ttl_min = parsed.ttl;
        strcpy(current, parsed.canonical_name);
    }
    return DNS_ERR_LOOKUP;
}

void dns_invalidate(const char *host, const struct in_addr *addr)
{
    int i;
    if (!host || !addr || !s_cache_mutex_ready) return;
    sceKernelLockLwMutex(&s_cache_mutex, 1, NULL);
    for (i = 0; i < DNS_CACHE_SIZE; ++i) {
        if (s_cache[i].valid && host_equal(s_cache[i].host, host) &&
            s_cache[i].addr.s_addr == addr->s_addr)
            s_cache[i].valid = 0;
    }
    sceKernelUnlockLwMutex(&s_cache_mutex, 1);
}
