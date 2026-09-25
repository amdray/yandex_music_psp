#include "services/time_sync.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psprtc.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "core/logger.h"
#include "services/dns.h"
#include "services/net_poll.h"
#include "services/net_stack.h"
#include "services/systemctrl_rng.h"

#ifndef SO_NONBLOCK
#define SO_NONBLOCK 0x1009
#endif

#define NTP_PORT 123
#define NTP_PACKET_SIZE 48
#define NTP_ERA_SECONDS 4294967296ULL
#define NTP_REQUEST_TIMEOUT_US 3000000ULL
#define NTP_POLL_SLICE_US 100000ULL
#define RTC_SET_CURRENT_TICK_NID_150 0x9763C138U
#define RTC_SET_CURRENT_TICK_NID_660 0x929620CEU

struct KernelCallArg {
    u32 arg1;
    u32 arg2;
    u32 arg3;
    u32 arg4;
    u32 arg5;
    u32 arg6;
    u32 arg7;
    u32 arg8;
    u32 arg9;
    u32 arg10;
    u32 arg11;
    u32 arg12;
    u32 ret1;
    u32 ret2;
};

extern u32 sctrlHENFindFunction(const char *module_name,
                                const char *library_name,
                                u32 nid);
extern int kuKernelCall(void *function, struct KernelCallArg *args);

static const char *const s_ntp_hosts[] = {
    "0.ru.pool.ntp.org",
    "1.ru.pool.ntp.org"
};

static int cancelled(volatile unsigned int *cancel)
{
    return cancel && __sync_fetch_and_add(cancel, 0U) != 0U;
}

static int dns_cancel(void *ctx)
{
    return cancelled((volatile unsigned int *)ctx);
}

static int generation_current(unsigned int generation)
{
    NetStackSnapshot link;
    return net_stack_get_snapshot(&link) == 0 &&
           link.apctl_state == PSP_NET_APCTL_STATE_GOT_IP &&
           link.generation == generation;
}

static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static long long tick_delta_to_us(u64 magnitude, u64 resolution, int negative)
{
    u64 whole_seconds;
    u64 remainder;
    u64 microseconds;
    if (resolution == 0) return 0;
    whole_seconds = magnitude / resolution;
    remainder = magnitude % resolution;
    if (whole_seconds > (u64)LLONG_MAX / 1000000ULL)
        return negative ? LLONG_MIN : LLONG_MAX;
    microseconds = whole_seconds * 1000000ULL;
    microseconds += (remainder * 1000000ULL) / resolution;
    if (microseconds > (u64)LLONG_MAX)
        return negative ? LLONG_MIN : LLONG_MAX;
    return negative ? -(long long)microseconds : (long long)microseconds;
}

static int wait_socket(int fd, short events, unsigned long long deadline,
                       unsigned int generation,
                       volatile unsigned int *cancel)
{
    for (;;) {
        struct SceNetInetPollfd poll_fd;
        unsigned long long now;
        unsigned long long remaining;
        int timeout_ms;
        int rc;

        if (cancelled(cancel)) return TIME_SYNC_ERR_CANCELLED;
        if (!generation_current(generation)) return TIME_SYNC_ERR_OFFLINE;
        now = sceKernelGetSystemTimeWide();
        if (now >= deadline) return TIME_SYNC_ERR_NETWORK;
        remaining = deadline - now;
        if (remaining > NTP_POLL_SLICE_US) remaining = NTP_POLL_SLICE_US;
        timeout_ms = (int)((remaining + 999ULL) / 1000ULL);

        memset(&poll_fd, 0, sizeof(poll_fd));
        poll_fd.fd = fd;
        poll_fd.events = events;
        rc = sceNetInetPoll(&poll_fd, 1, timeout_ms);
        if (rc > 0) {
            if (poll_fd.revents & events) return 0;
            if (poll_fd.revents & (SCE_NET_INET_POLLERR |
                                   SCE_NET_INET_POLLHUP |
                                   SCE_NET_INET_POLLNVAL))
                return TIME_SYNC_ERR_NETWORK;
        } else if (rc < 0) {
            return TIME_SYNC_ERR_NETWORK;
        }
    }
}

static int ntp_exchange(const struct in_addr *address,
                        unsigned int generation,
                        volatile unsigned int *cancel,
                        uint32_t *seconds, uint32_t *fraction,
                        unsigned long long *half_rtt_us)
{
    unsigned char request[NTP_PACKET_SIZE];
    unsigned char response[NTP_PACKET_SIZE];
    unsigned char token[8];
    struct sockaddr_in peer;
    unsigned long long deadline;
    unsigned long long sent_at;
    int fd = -1;
    int nonblock = 1;
    int rc = TIME_SYNC_ERR_NETWORK;
    int error;

    if (!address || !seconds || !fraction || !half_rtt_us) {
        logLine("time_sync: ntp invalid arguments\n");
        return TIME_SYNC_ERR_PROTOCOL;
    }
    error = systemctrl_rng(NULL, token, sizeof(token));
    if (error != 0) {
        logLine("time_sync: ntp rng failed rc=%d\n", error);
        return TIME_SYNC_ERR_PROTOCOL;
    }

    memset(request, 0, sizeof(request));
    request[0] = (unsigned char)((4U << 3) | 3U); /* NTPv4 client. */
    memcpy(request + 40, token, sizeof(token));
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(NTP_PORT);
    peer.sin_addr = *address;

    fd = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        logLine("time_sync: ntp socket failed rc=%d errno=%d\n",
                fd, sceNetInetGetErrno());
        return TIME_SYNC_ERR_NETWORK;
    }
    if (sceNetInetSetsockopt(fd, SOL_SOCKET, SO_NONBLOCK,
                             &nonblock, sizeof(nonblock)) < 0) {
        logLine("time_sync: ntp nonblock failed errno=%d\n",
                sceNetInetGetErrno());
        goto done;
    }

    deadline = sceKernelGetSystemTimeWide() + NTP_REQUEST_TIMEOUT_US;
    rc = wait_socket(fd, SCE_NET_INET_POLLOUT, deadline, generation, cancel);
    if (rc != 0) {
        logLine("time_sync: ntp send wait failed status=%d\n", rc);
        goto done;
    }
    for (;;) {
        int sent = (int)sceNetInetSendto(fd, request, sizeof(request), 0,
                                        (struct sockaddr *)&peer,
                                        sizeof(peer));
        if (sent == (int)sizeof(request)) break;
        if (sent >= 0) {
            logLine("time_sync: ntp short send bytes=%d\n", sent);
            rc = TIME_SYNC_ERR_NETWORK;
            goto done;
        }
        {
            error = sceNetInetGetErrno();
            if (error != EAGAIN && error != EWOULDBLOCK) {
                logLine("time_sync: ntp send failed errno=%d\n", error);
                rc = TIME_SYNC_ERR_NETWORK;
                goto done;
            }
        }
        rc = wait_socket(fd, SCE_NET_INET_POLLOUT, deadline, generation,
                         cancel);
        if (rc != 0) {
            logLine("time_sync: ntp resend wait failed status=%d\n", rc);
            goto done;
        }
    }
    sent_at = sceKernelGetSystemTimeWide();

    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int received;
        unsigned int version;
        unsigned int mode;
        unsigned int leap;

        rc = wait_socket(fd, SCE_NET_INET_POLLIN, deadline, generation,
                         cancel);
        if (rc != 0) {
            logLine("time_sync: ntp response wait failed status=%d\n", rc);
            goto done;
        }
        memset(&from, 0, sizeof(from));
        received = (int)sceNetInetRecvfrom(fd, response, sizeof(response), 0,
                                          (struct sockaddr *)&from,
                                          &from_len);
        if (received < 0) {
            error = sceNetInetGetErrno();
            if (error == EAGAIN || error == EWOULDBLOCK) continue;
            logLine("time_sync: ntp receive failed errno=%d\n", error);
            rc = TIME_SYNC_ERR_NETWORK;
            goto done;
        }
        if (received < NTP_PACKET_SIZE || from_len < sizeof(from) ||
            from.sin_family != AF_INET || from.sin_port != htons(NTP_PORT) ||
            from.sin_addr.s_addr != address->s_addr)
            continue;

        leap = response[0] >> 6;
        version = (response[0] >> 3) & 7U;
        mode = response[0] & 7U;
        if (leap == 3U || (version != 3U && version != 4U) || mode != 4U ||
            response[1] == 0U || response[1] > 15U ||
            memcmp(response + 24, token, sizeof(token)) != 0) {
            logLine("time_sync: ntp invalid response li=%u version=%u mode=%u stratum=%u origin_match=%d\n",
                    leap, version, mode, (unsigned int)response[1],
                    memcmp(response + 24, token, sizeof(token)) == 0);
            rc = TIME_SYNC_ERR_PROTOCOL;
            goto done;
        }
        *seconds = load_be32(response + 40);
        *fraction = load_be32(response + 44);
        if (*seconds == 0U && *fraction == 0U) {
            logLine("time_sync: ntp zero transmit timestamp\n");
            rc = TIME_SYNC_ERR_PROTOCOL;
            goto done;
        }
        *half_rtt_us = (sceKernelGetSystemTimeWide() - sent_at) / 2ULL;
        logLine("time_sync: ntp accepted version=%u stratum=%u rtt_us=%llu\n",
                version, (unsigned int)response[1], *half_rtt_us * 2ULL);
        rc = 0;
        goto done;
    }

done:
    if (fd >= 0) sceNetInetClose(fd);
    return rc;
}

static int unfold_ntp_tick(uint32_t seconds, uint32_t fraction,
                           unsigned long long half_rtt_us,
                           u64 *target_tick, u64 *current_tick)
{
    ScePspDateTime epoch_date;
    ScePspDateTime candidate_date;
    u64 epoch_tick;
    u64 now_tick;
    u64 current_ntp_seconds = 0;
    u64 resolution;
    u64 base_era = 0;
    u64 best_tick = 0;
    u64 best_distance = ~(u64)0;
    int found = 0;
    int delta;

    if (!target_tick || !current_tick) return TIME_SYNC_ERR_RTC;
    memset(&epoch_date, 0, sizeof(epoch_date));
    epoch_date.year = 1900;
    epoch_date.month = 1;
    epoch_date.day = 1;
    resolution = sceRtcGetTickResolution();
    if (resolution == 0 || sceRtcGetTick(&epoch_date, &epoch_tick) < 0 ||
        sceRtcGetCurrentTick(&now_tick) < 0)
        return TIME_SYNC_ERR_RTC;

    if (now_tick >= epoch_tick)
        current_ntp_seconds = (now_tick - epoch_tick) / resolution;
    base_era = current_ntp_seconds / NTP_ERA_SECONDS;

    for (delta = -1; delta <= 1; ++delta) {
        u64 era;
        u64 total_seconds;
        u64 candidate_tick;
        u64 distance;
        if (delta < 0 && base_era == 0) continue;
        era = (u64)((long long)base_era + delta);
        total_seconds = era * NTP_ERA_SECONDS + (u64)seconds;
        if (total_seconds > (~(u64)0 - epoch_tick) / resolution) continue;
        candidate_tick = epoch_tick + total_seconds * resolution;
        candidate_tick += ((u64)fraction * resolution) >> 32;
        candidate_tick += (half_rtt_us * resolution) / 1000000ULL;
        if (sceRtcSetTick(&candidate_date, &candidate_tick) < 0 ||
            sceRtcCheckValid(&candidate_date) < 0 || candidate_date.year < 2004)
            continue;
        distance = candidate_tick >= now_tick ? candidate_tick - now_tick
                                              : now_tick - candidate_tick;
        if (!found || distance < best_distance) {
            found = 1;
            best_distance = distance;
            best_tick = candidate_tick;
        }
    }
    if (!found) return TIME_SYNC_ERR_PROTOCOL;
    *target_tick = best_tick;
    *current_tick = now_tick;
    return 0;
}

static u32 find_rtc_set_current_tick(u32 *resolved_nid)
{
    static const u32 nids[] = {
        RTC_SET_CURRENT_TICK_NID_150,
        RTC_SET_CURRENT_TICK_NID_660
    };
    unsigned int i;
    for (i = 0; i < sizeof(nids) / sizeof(nids[0]); ++i) {
        u32 address = sctrlHENFindFunction("sceRTC_Service", "sceRtc_driver",
                                           nids[i]);
        if (address != 0) {
            if (resolved_nid) *resolved_nid = nids[i];
            return address;
        }
    }
    return 0;
}

static int set_system_tick(u64 *tick)
{
    struct KernelCallArg args;
    u32 address;
    u32 nid = 0;
    int rc;
    if (!tick) return TIME_SYNC_ERR_RTC;
    address = find_rtc_set_current_tick(&nid);
    if (address == 0) {
        logLine("time_sync: rtc setter export not found\n");
        return TIME_SYNC_ERR_ARK;
    }
    logLine("time_sync: rtc setter resolved nid=0x%08X\n", nid);
    memset(&args, 0, sizeof(args));
    args.arg1 = (u32)(uintptr_t)tick;
    rc = kuKernelCall((void *)address, &args);
    if (rc < 0) {
        logLine("time_sync: kuKernelCall failed rc=%d\n", rc);
        return rc;
    }
    logLine("time_sync: rtc setter returned rc=%d\n", (int)args.ret1);
    return (int)args.ret1;
}

int time_sync_run(volatile unsigned int *cancel, TimeSyncResult *result)
{
    NetStackSnapshot link;
    TimeSyncResult local;
    unsigned int host_index;
    int last_error = TIME_SYNC_ERR_NETWORK;

    if (!result) return TIME_SYNC_ERR_PROTOCOL;
    memset(&local, 0, sizeof(local));
    local.status = TIME_SYNC_ERR_NETWORK;
    local.rtc_set_result = 0;

    if (net_stack_get_snapshot(&link) < 0 ||
        link.apctl_state != PSP_NET_APCTL_STATE_GOT_IP) {
        logLine("time_sync: skipped because network is offline\n");
        local.status = TIME_SYNC_ERR_OFFLINE;
        *result = local;
        return local.status;
    }

    for (host_index = 0;
         host_index < sizeof(s_ntp_hosts) / sizeof(s_ntp_hosts[0]);
         ++host_index) {
        struct in_addr address;
        uint32_t seconds;
        uint32_t fraction;
        unsigned long long half_rtt_us;
        u64 target_tick __attribute__((aligned(8)));
        u64 current_tick;
        u64 resolution;
        u64 offset_ticks;
        int offset_negative;
        int cacheable;
        int rc;
        uint32_t ip;

        if (cancelled(cancel)) {
            logLine("time_sync: cancelled before server attempt\n");
            last_error = TIME_SYNC_ERR_CANCELLED;
            break;
        }
        logLine("time_sync: resolving host=%s\n", s_ntp_hosts[host_index]);
        rc = dns_resolve(s_ntp_hosts[host_index], &address, &cacheable,
                         dns_cancel, (void *)cancel);
        if (rc != 0) {
            last_error = (rc == DNS_ERR_CANCELLED) ? TIME_SYNC_ERR_CANCELLED :
                         (rc == DNS_ERR_OFFLINE) ? TIME_SYNC_ERR_OFFLINE :
                                                  TIME_SYNC_ERR_NETWORK;
            logLine("time_sync: dns failed host=%s rc=%d status=%d\n",
                    s_ntp_hosts[host_index], rc, last_error);
            if (last_error == TIME_SYNC_ERR_CANCELLED ||
                last_error == TIME_SYNC_ERR_OFFLINE)
                break;
            continue;
        }
        ip = ntohl(address.s_addr);
        logLine("time_sync: server host=%s ip=%u.%u.%u.%u cacheable=%d\n",
                s_ntp_hosts[host_index], (ip >> 24) & 0xffU,
                (ip >> 16) & 0xffU, (ip >> 8) & 0xffU, ip & 0xffU,
                cacheable);
        rc = ntp_exchange(&address, link.generation, cancel, &seconds,
                          &fraction, &half_rtt_us);
        if (rc != 0) {
            logLine("time_sync: ntp attempt failed host=%s status=%d\n",
                    s_ntp_hosts[host_index], rc);
            last_error = rc;
            if (rc == TIME_SYNC_ERR_CANCELLED || rc == TIME_SYNC_ERR_OFFLINE)
                break;
            continue;
        }
        rc = unfold_ntp_tick(seconds, fraction, half_rtt_us,
                             &target_tick, &current_tick);
        if (rc != 0) {
            logLine("time_sync: timestamp conversion failed host=%s status=%d\n",
                    s_ntp_hosts[host_index], rc);
            last_error = rc;
            continue;
        }
        resolution = sceRtcGetTickResolution();
        offset_negative = target_tick < current_tick;
        offset_ticks = offset_negative ? current_tick - target_tick
                                       : target_tick - current_tick;
        local.offset_us = tick_delta_to_us(offset_ticks, resolution,
                                           offset_negative);
        {
            ScePspDateTime target_date;
            if (sceRtcSetTick(&target_date, &target_tick) >= 0) {
                logLine("time_sync: candidate utc=%04u-%02u-%02uT%02u:%02u:%02u.%06u offset_us=%lld\n",
                        (unsigned int)target_date.year,
                        (unsigned int)target_date.month,
                        (unsigned int)target_date.day,
                        (unsigned int)target_date.hour,
                        (unsigned int)target_date.minute,
                        (unsigned int)target_date.second,
                        (unsigned int)target_date.microsecond,
                        local.offset_us);
            } else {
                logLine("time_sync: candidate offset_us=%lld\n",
                        local.offset_us);
            }
        }
        if (offset_ticks <= resolution * 60ULL) {
            logLine("time_sync: rtc unchanged threshold_s=60\n");
            local.status = TIME_SYNC_UNCHANGED;
            *result = local;
            return local.status;
        }
        local.rtc_set_result = set_system_tick(&target_tick);
        if (local.rtc_set_result < 0) {
            logLine("time_sync: rtc update failed rc=%d\n",
                    local.rtc_set_result);
            local.status = TIME_SYNC_ERR_ARK;
            *result = local;
            return local.status;
        }
        {
            u64 verify_tick;
            u64 verify_distance;
            if (sceRtcGetCurrentTick(&verify_tick) < 0) {
                logLine("time_sync: rtc verification read failed\n");
                local.status = TIME_SYNC_ERR_RTC;
                *result = local;
                return local.status;
            }
            verify_distance = verify_tick >= target_tick
                            ? verify_tick - target_tick
                            : target_tick - verify_tick;
            if (verify_distance > resolution * 2ULL) {
                logLine("time_sync: rtc verification failed distance_us=%lld\n",
                        tick_delta_to_us(verify_distance, resolution, 0));
                local.status = TIME_SYNC_ERR_RTC;
                *result = local;
                return local.status;
            }
        }
        logLine("time_sync: rtc updated and verified\n");
        local.status = TIME_SYNC_UPDATED;
        *result = local;
        return local.status;
    }

    local.status = last_error;
    logLine("time_sync: all attempts finished status=%d\n", last_error);
    *result = local;
    return local.status;
}
