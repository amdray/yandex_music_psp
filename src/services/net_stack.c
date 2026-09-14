#include "services/net_stack.h"

#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psputility.h>
#include <psputility_netparam.h>
#include <pspwlan.h>
#include <pspthreadman.h>
#include <stdint.h>
#include <string.h>

#include "core/logger.h"
#include "services/dns.h"
#include "services/net_link_accumulator.h"

#define PSP_NET_PROFILE_MAX 9
/* Product policy, not a PSP API timing guarantee. */
#define APCTL_ATTEMPT_REPORT_US 15000000ULL
#define APCTL_RETRY_DELAY_US      500000ULL
#define SUPERVISOR_INTERVAL_US    250000U
#define APCTL_DIAG_CAPACITY             32U

typedef struct ApctlDiagEvent {
    volatile unsigned int ready;
    unsigned int sequence;
    unsigned long long timestamp_us;
    unsigned int attempt_id;
    SceUID thread_id;
    uintptr_t callback_arg;
    int profile;
    int old_state;
    int new_state;
    int event;
    int error;
} ApctlDiagEvent;

typedef struct SupervisorState {
    int pending;
    int stuck;
    int last_connect_error;
    int attempt_profile;
    int retry_profile;
    unsigned int start_disconnected_generation;
    unsigned int start_got_ip_generation;
    unsigned long long deadline_us;
    unsigned long long retry_at_us;
} SupervisorState;

static NetLinkAccumulator s_link;

static volatile int s_stack_started;
static volatile int s_supervisor_running;
static volatile int s_runtime_stuck;
static volatile unsigned int s_supervisor_phase;
static int s_last_profile;
static int s_handler_id = -1;
static SceUID s_event_sema = -1;
static SceUID s_supervisor_thread = -1;
static ApctlDiagEvent s_apctl_diag[APCTL_DIAG_CAPACITY];
static volatile unsigned int s_apctl_diag_write;
static unsigned int s_apctl_diag_read;
static volatile unsigned int s_connect_attempt_id;
static volatile int s_connect_attempt_profile;

static const NetTlsNetworkConfig s_default_config = {
    128 * 1024, 42, 4 * 1024, 42, 4 * 1024, 0x1800, 48, 0
};

static void supervisor_set_phase(NetSupervisorPhase phase)
{
    (void)__sync_lock_test_and_set(&s_supervisor_phase, (unsigned int)phase);
}

/* The APCTL callback runs on a PSP networking thread. It must not call the
 * ordinary logger, whose full-buffer path can synchronously write to the
 * Memory Stick. Publish a bounded RAM record and drain it from an app-owned
 * thread instead. */
static void apctl_diag_publish(int old_state, int new_state, int event, int error,
                               void *arg)
{
    unsigned int sequence = __sync_fetch_and_add(&s_apctl_diag_write, 1U);
    ApctlDiagEvent *slot = &s_apctl_diag[sequence % APCTL_DIAG_CAPACITY];

    slot->ready = 0U;
    __sync_synchronize();
    slot->sequence = sequence;
    slot->timestamp_us = sceKernelGetSystemTimeWide();
    slot->attempt_id = __sync_fetch_and_add(&s_connect_attempt_id, 0U);
    slot->thread_id = sceKernelGetThreadId();
    slot->callback_arg = (uintptr_t)arg;
    slot->profile = __sync_fetch_and_add(&s_connect_attempt_profile, 0);
    slot->old_state = old_state;
    slot->new_state = new_state;
    slot->event = event;
    slot->error = error;
    __sync_synchronize();
    slot->ready = 1U;
}

static const char *apctl_state_name(int state)
{
    switch (state) {
        case PSP_NET_APCTL_STATE_DISCONNECTED: return "DISCONNECTED";
        case PSP_NET_APCTL_STATE_SCANNING: return "SCANNING";
        case PSP_NET_APCTL_STATE_JOINING: return "JOINING";
        case PSP_NET_APCTL_STATE_GETTING_IP: return "GETTING_IP";
        case PSP_NET_APCTL_STATE_GOT_IP: return "GOT_IP";
        case PSP_NET_APCTL_STATE_EAP_AUTH: return "EAP_AUTH";
        case PSP_NET_APCTL_STATE_KEY_EXCHANGE: return "KEY_EXCHANGE";
        default: return "UNKNOWN";
    }
}

static const char *apctl_event_name(int event)
{
    switch (event) {
        case PSP_NET_APCTL_EVENT_CONNECT_REQUEST: return "CONNECT_REQUEST";
        case PSP_NET_APCTL_EVENT_SCAN_REQUEST: return "SCAN_REQUEST";
        case PSP_NET_APCTL_EVENT_SCAN_COMPLETE: return "SCAN_COMPLETE";
        case PSP_NET_APCTL_EVENT_ESTABLISHED: return "ESTABLISHED";
        case PSP_NET_APCTL_EVENT_GET_IP: return "GET_IP";
        case PSP_NET_APCTL_EVENT_DISCONNECT_REQUEST: return "DISCONNECT_REQUEST";
        case PSP_NET_APCTL_EVENT_ERROR: return "ERROR";
        case PSP_NET_APCTL_EVENT_INFO: return "INFO";
        case PSP_NET_APCTL_EVENT_EAP_AUTH: return "EAP_AUTH";
        case PSP_NET_APCTL_EVENT_KEY_EXCHANGE: return "KEY_EXCHANGE";
        case PSP_NET_APCTL_EVENT_RECONNECT: return "RECONNECT";
        default: return "UNKNOWN";
    }
}

static void log_apctl_info_string(int code, const char *name)
{
    union SceNetApctlInfo info;
    int rc;
    memset(&info, 0, sizeof(info));
    rc = sceNetApctlGetInfo(code, &info);
    ((char *)&info)[sizeof(info) - 1] = '\0';
    logLine("netmgr: apctl_info code=%d name=%s rc=0x%08X value='%s'\n",
            code, name, (unsigned int)rc, rc >= 0 ? (char *)&info : "");
}

static void log_apctl_info_uint(int code, const char *name)
{
    union SceNetApctlInfo info;
    int rc;
    memset(&info, 0, sizeof(info));
    rc = sceNetApctlGetInfo(code, &info);
    logLine("netmgr: apctl_info code=%d name=%s rc=0x%08X value=%u\n",
            code, name, (unsigned int)rc, rc >= 0 ? info.useProxy : 0U);
}

static void log_apctl_info_snapshot(void)
{
    union SceNetApctlInfo info;
    int rc;

    log_apctl_info_string(PSP_NET_APCTL_INFO_PROFILE_NAME, "profile_name");
    memset(&info, 0, sizeof(info));
    rc = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_BSSID, &info);
    logLine("netmgr: apctl_info code=%d name=bssid rc=0x%08X value=%02X:%02X:%02X:%02X:%02X:%02X\n",
            PSP_NET_APCTL_INFO_BSSID, (unsigned int)rc,
            info.bssid[0], info.bssid[1], info.bssid[2],
            info.bssid[3], info.bssid[4], info.bssid[5]);
    log_apctl_info_string(PSP_NET_APCTL_INFO_SSID, "ssid");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_SSID_LENGTH, "ssid_length");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_SECURITY_TYPE, "security_type");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_STRENGTH, "strength");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_CHANNEL, "channel");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_POWER_SAVE, "power_save");
    log_apctl_info_string(PSP_NET_APCTL_INFO_IP, "ip");
    log_apctl_info_string(PSP_NET_APCTL_INFO_SUBNETMASK, "subnet_mask");
    log_apctl_info_string(PSP_NET_APCTL_INFO_GATEWAY, "gateway");
    log_apctl_info_string(PSP_NET_APCTL_INFO_PRIMDNS, "primary_dns");
    log_apctl_info_string(PSP_NET_APCTL_INFO_SECDNS, "secondary_dns");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_USE_PROXY, "use_proxy");
    log_apctl_info_string(PSP_NET_APCTL_INFO_PROXY_URL, "proxy_url");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_PROXY_PORT, "proxy_port");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_8021_EAP_TYPE, "eap_type");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_START_BROWSER, "start_browser");
    log_apctl_info_uint(PSP_NET_APCTL_INFO_WIFISP, "wifisp");
}

static void log_profile_param_string(int profile, int param, const char *name,
                                     int redact)
{
    netData data;
    int rc;
    size_t length;
    memset(&data, 0, sizeof(data));
    rc = sceUtilityGetNetParam(profile, param, &data);
    data.asString[sizeof(data.asString) - 1] = '\0';
    length = strlen(data.asString);
    if (redact) {
        logLine("netmgr: profile_param profile=%d param=%d name=%s rc=0x%08X redacted=1 present=%d length=%u\n",
                profile, param, name, (unsigned int)rc,
                rc >= 0 && length > 0U, (unsigned int)(rc >= 0 ? length : 0U));
    } else {
        logLine("netmgr: profile_param profile=%d param=%d name=%s rc=0x%08X value='%s'\n",
                profile, param, name, (unsigned int)rc,
                rc >= 0 ? data.asString : "");
    }
}

static void log_profile_param_uint(int profile, int param, const char *name)
{
    netData data;
    int rc;
    memset(&data, 0, sizeof(data));
    rc = sceUtilityGetNetParam(profile, param, &data);
    logLine("netmgr: profile_param profile=%d param=%d name=%s rc=0x%08X value=%u\n",
            profile, param, name, (unsigned int)rc,
            rc >= 0 ? data.asUint : 0U);
}

static void log_profile_snapshot(int profile)
{
    log_profile_param_string(profile, PSP_NETPARAM_NAME, "name", 0);
    log_profile_param_string(profile, PSP_NETPARAM_SSID, "ssid", 0);
    log_profile_param_uint(profile, PSP_NETPARAM_SECURE, "secure");
    log_profile_param_string(profile, PSP_NETPARAM_WEPKEY, "wep_key", 1);
    log_profile_param_uint(profile, PSP_NETPARAM_IS_STATIC_IP, "is_static_ip");
    log_profile_param_string(profile, PSP_NETPARAM_IP, "ip", 0);
    log_profile_param_string(profile, PSP_NETPARAM_NETMASK, "netmask", 0);
    log_profile_param_string(profile, PSP_NETPARAM_ROUTE, "route", 0);
    log_profile_param_uint(profile, PSP_NETPARAM_MANUAL_DNS, "manual_dns");
    log_profile_param_string(profile, PSP_NETPARAM_PRIMARYDNS, "primary_dns", 0);
    log_profile_param_string(profile, PSP_NETPARAM_SECONDARYDNS, "secondary_dns", 0);
    log_profile_param_uint(profile, PSP_NETPARAM_USE_PROXY, "use_proxy");
    log_profile_param_string(profile, PSP_NETPARAM_PROXY_SERVER, "proxy_server", 0);
    log_profile_param_uint(profile, PSP_NETPARAM_PROXY_PORT, "proxy_port");
    log_profile_param_uint(profile, PSP_NETPARAM_UNKNOWN1, "unknown1");
    log_profile_param_uint(profile, PSP_NETPARAM_UNKNOWN2, "unknown2");
}

static void log_all_profile_snapshots(void)
{
    int profile;
    for (profile = 1; profile <= PSP_NET_PROFILE_MAX; ++profile) {
        int rc = sceUtilityCheckNetParam(profile);
        logLine("netmgr: profile_check profile=%d rc=0x%08X\n",
                profile, (unsigned int)rc);
        if (rc == 0) log_profile_snapshot(profile);
    }
    /* Keep profile enumeration I/O outside the timed connection attempts. */
    logger_flush();
}

static void apctl_diag_drain(void)
{
    unsigned int write = __sync_fetch_and_add(&s_apctl_diag_write, 0U);

    if (write - s_apctl_diag_read > APCTL_DIAG_CAPACITY) {
        unsigned int dropped = write - s_apctl_diag_read - APCTL_DIAG_CAPACITY;
        s_apctl_diag_read = write - APCTL_DIAG_CAPACITY;
        logLine("netmgr: apctl_diag dropped=%u\n", dropped);
    }

    while (s_apctl_diag_read != write) {
        ApctlDiagEvent *slot =
            &s_apctl_diag[s_apctl_diag_read % APCTL_DIAG_CAPACITY];
        unsigned int sequence;
        unsigned long long timestamp_us;
        unsigned int attempt_id;
        SceUID thread_id;
        uintptr_t callback_arg;
        int profile;
        int old_state;
        int new_state;
        int event;
        int error;

        __sync_synchronize();
        if (!slot->ready || slot->sequence != s_apctl_diag_read) break;
        sequence = slot->sequence;
        timestamp_us = slot->timestamp_us;
        attempt_id = slot->attempt_id;
        thread_id = slot->thread_id;
        callback_arg = slot->callback_arg;
        profile = slot->profile;
        old_state = slot->old_state;
        new_state = slot->new_state;
        event = slot->event;
        error = slot->error;
        __sync_synchronize();
        if (!slot->ready || slot->sequence != sequence) continue;

        {
            int direct_state = -1;
            int state_rc = sceNetApctlGetState(&direct_state);
            int wlan_switch = sceWlanGetSwitchState();
            logLine("netmgr: apctl_cb at=%llu us seq=%u tid=%d arg=0x%08X attempt=%u profile=%d old=%d(%s) new=%d(%s) event=%d(%s) error=0x%08X drain_state_rc=0x%08X drain_state=%d(%s) wlan=%d\n",
                    timestamp_us, sequence, (int)thread_id,
                    (unsigned int)callback_arg, attempt_id, profile,
                    old_state, apctl_state_name(old_state),
                    new_state, apctl_state_name(new_state),
                    event, apctl_event_name(event), (unsigned int)error,
                    (unsigned int)state_rc, direct_state,
                    apctl_state_name(direct_state), wlan_switch);
            if (new_state == PSP_NET_APCTL_STATE_GOT_IP)
                log_apctl_info_snapshot();
        }
        s_apctl_diag_read++;
    }
}

static unsigned int connect_attempt_begin(int profile, const char *phase,
                                          const NetStackSnapshot *before)
{
    unsigned int attempt_id =
        __sync_add_and_fetch(&s_connect_attempt_id, 1U);
    (void)__sync_lock_test_and_set(&s_connect_attempt_profile, profile);
    logLine("netmgr: connect_begin attempt=%u phase=%s profile=%d state=%d disconnected_gen=%u got_ip_gen=%u\n",
            attempt_id, phase, profile,
            before ? before->apctl_state : -1,
            before ? before->disconnected_generation : 0U,
            before ? before->got_ip_generation : 0U);
    return attempt_id;
}

static void apctl_handler(int old_state, int new_state, int event,
                          int error, void *arg)
{
    (void)arg;
    apctl_diag_publish(old_state, new_state, event, error, arg);
    (void)__sync_fetch_and_add(&s_link.inflight, 1U);
    net_link_accumulate(&s_link, old_state, new_state,
                        PSP_NET_APCTL_STATE_DISCONNECTED,
                        PSP_NET_APCTL_STATE_GOT_IP);
    __sync_synchronize();
    (void)__sync_fetch_and_add(&s_link.completion_epoch, 1U);
    (void)__sync_fetch_and_sub(&s_link.inflight, 1U);
    if (__sync_bool_compare_and_swap(&s_link.wake_pending, 0U, 1U) &&
        s_event_sema >= 0)
        (void)sceKernelSignalSema(s_event_sema, 1);
}

int net_stack_get_snapshot(NetStackSnapshot *out)
{
    unsigned int epoch_before;
    unsigned int epoch_after;
    unsigned int inflight_after;
    unsigned int disconnected_generation;
    unsigned int loss_generation;
    unsigned int got_ip_generation;
    unsigned int generation;
    int state;
    if (!out) return -1;
    for (;;) {
        epoch_before = s_link.completion_epoch;
        __sync_synchronize();
        if (s_link.inflight != 0U) {
            sceKernelDelayThread(100);
            continue;
        }
        state = s_link.current_state;
        disconnected_generation = s_link.disconnected_generation;
        loss_generation = s_link.loss_generation;
        got_ip_generation = s_link.got_ip_generation;
        generation = s_link.generation;
        if (s_link.publication_invalid != 0U) return -1;
        __sync_synchronize();
        epoch_after = s_link.completion_epoch;
        inflight_after = s_link.inflight;
        if (epoch_before != epoch_after || inflight_after != 0U) continue;

        if (!net_link_state_valid(state)) return -1;
        out->apctl_state = state;
        out->disconnected_generation = disconnected_generation;
        out->loss_generation = loss_generation;
        out->got_ip_generation = got_ip_generation;
        out->generation = generation;
        return 0;
    }
}

static int profile_exists(int profile)
{
    return profile > 0 && profile <= PSP_NET_PROFILE_MAX &&
           sceUtilityCheckNetParam(profile) == 0;
}

static int select_retry_profile(int preferred)
{
    int i;
    if (profile_exists(preferred)) return preferred;
    for (i = 1; i <= PSP_NET_PROFILE_MAX; ++i) {
        if (profile_exists(i)) return i;
    }
    return -1;
}

static int next_configured_profile(int after)
{
    int step;
    if (after < 1 || after > PSP_NET_PROFILE_MAX)
        return select_retry_profile(0);
    for (step = 1; step <= PSP_NET_PROFILE_MAX; ++step) {
        int profile = ((after - 1 + step) % PSP_NET_PROFILE_MAX) + 1;
        if (profile_exists(profile)) return profile;
    }
    return -1;
}

static int startup_try_profile(int profile)
{
    NetStackSnapshot before;
    unsigned int attempt_id;
    int rc;
    unsigned long long deadline;
    if (net_stack_get_snapshot(&before) < 0)
        return NET_STACK_ERR_FATAL_LIVE_RUNTIME;
    if (before.apctl_state != PSP_NET_APCTL_STATE_DISCONNECTED)
        return NET_STACK_ERR_FATAL_LIVE_RUNTIME;
    attempt_id = connect_attempt_begin(profile, "startup", &before);
    rc = sceNetApctlConnect(profile);
    logLine("netmgr: connect_return attempt=%u phase=startup profile=%d rc=0x%08X\n",
            attempt_id, profile, (unsigned int)rc);
    if (rc < 0) {
        logLine("netmgr: startup Connect profile=%d rc=0x%08X\n",
                profile, (unsigned int)rc);
        return -1;
    }
    deadline = sceKernelGetSystemTimeWide() + APCTL_ATTEMPT_REPORT_US;
    for (;;) {
        NetStackSnapshot now;
        SceUInt wait_us = 100000U;
        (void)__sync_lock_test_and_set(&s_link.wake_pending, 0U);
        if (net_stack_get_snapshot(&now) < 0)
            return NET_STACK_ERR_FATAL_LIVE_RUNTIME;
        apctl_diag_drain();
        rc = net_link_attempt_outcome(
            now.apctl_state,
            now.disconnected_generation, before.disconnected_generation,
            now.got_ip_generation, before.got_ip_generation,
            PSP_NET_APCTL_STATE_DISCONNECTED, PSP_NET_APCTL_STATE_GOT_IP);
        if (rc == NET_LINK_ATTEMPT_FAILED) {
            logLine("netmgr: startup offline profile=%d\n", profile);
            return -1;
        }
        if (rc == NET_LINK_ATTEMPT_SUCCESS) return 0;
        if ((unsigned long long)sceKernelGetSystemTimeWide() >= deadline) {
            logLine("netmgr: startup attempt stuck state=%d profile=%d\n",
                    now.apctl_state, profile);
            return NET_STACK_ERR_FATAL_LIVE_RUNTIME;
        }
        (void)sceKernelWaitSema(s_event_sema, 1, &wait_us);
    }
}

static int connect_initial_profile(int preferred)
{
    int i;
    int rc;
    if (profile_exists(preferred)) {
        rc = startup_try_profile(preferred);
        if (rc == 0) return preferred;
        if (rc == NET_STACK_ERR_FATAL_LIVE_RUNTIME) return rc;
    }
    for (i = 1; i <= PSP_NET_PROFILE_MAX; ++i) {
        if (i == preferred || !profile_exists(i)) continue;
        rc = startup_try_profile(i);
        if (rc == 0) return i;
        if (rc == NET_STACK_ERR_FATAL_LIVE_RUNTIME) return rc;
    }
    return -1;
}

static int supervisor_main(SceSize args, void *argp)
{
    SupervisorState attempt;
    (void)args;
    (void)argp;
    memset(&attempt, 0, sizeof(attempt));
    attempt.retry_profile = s_last_profile;
    while (s_supervisor_running) {
        NetStackSnapshot link;
        unsigned long long now;
        SceUInt wait_us = SUPERVISOR_INTERVAL_US;
        (void)sceKernelWaitSema(s_event_sema, 1, &wait_us);
        if (!s_supervisor_running) break;
        (void)__sync_lock_test_and_set(&s_link.wake_pending, 0U);
        if (net_stack_get_snapshot(&link) < 0) {
            s_runtime_stuck = 1;
            supervisor_set_phase(NET_SUPERVISOR_STUCK);
            continue;
        }
        apctl_diag_drain();
        now = sceKernelGetSystemTimeWide();

        if (attempt.pending) {
            int outcome = net_link_attempt_outcome(
                link.apctl_state,
                link.disconnected_generation,
                attempt.start_disconnected_generation,
                link.got_ip_generation, attempt.start_got_ip_generation,
                PSP_NET_APCTL_STATE_DISCONNECTED,
                PSP_NET_APCTL_STATE_GOT_IP);
            if (outcome == NET_LINK_ATTEMPT_FAILED) {
                attempt.pending = 0;
                attempt.stuck = 0;
                attempt.retry_profile =
                    next_configured_profile(attempt.attempt_profile);
                attempt.retry_at_us = now + APCTL_RETRY_DELAY_US;
                s_runtime_stuck = 0;
                supervisor_set_phase(NET_SUPERVISOR_BACKOFF);
                logLine("netmgr: retryable offline profile=%d next=%d\n",
                        attempt.attempt_profile, attempt.retry_profile);
            } else if (outcome == NET_LINK_ATTEMPT_SUCCESS) {
                attempt.pending = 0;
                attempt.stuck = 0;
                s_last_profile = attempt.attempt_profile;
                attempt.retry_profile = attempt.attempt_profile;
                attempt.retry_at_us = 0;
                s_runtime_stuck = 0;
                supervisor_set_phase(NET_SUPERVISOR_IDLE);
            } else if (now >= attempt.deadline_us && !attempt.stuck) {
                attempt.stuck = 1;
                s_runtime_stuck = 1;
                supervisor_set_phase(NET_SUPERVISOR_STUCK);
                logLine("netmgr: attempt stuck state=%d\n", link.apctl_state);
            }
        }
        if (link.apctl_state != PSP_NET_APCTL_STATE_DISCONNECTED ||
            !sceWlanGetSwitchState() || attempt.pending ||
            now < attempt.retry_at_us)
            continue;

        if (!profile_exists(attempt.retry_profile))
            attempt.retry_profile = select_retry_profile(0);
        if (attempt.retry_profile < 0) {
            attempt.retry_at_us = now + APCTL_RETRY_DELAY_US;
            supervisor_set_phase(NET_SUPERVISOR_BACKOFF);
            continue;
        }

        attempt.pending = 1;
        attempt.attempt_profile = attempt.retry_profile;
        supervisor_set_phase(NET_SUPERVISOR_CONNECTING);
        attempt.stuck = 0;
        attempt.deadline_us = now + APCTL_ATTEMPT_REPORT_US;
        attempt.start_disconnected_generation = link.disconnected_generation;
        attempt.start_got_ip_generation = link.got_ip_generation;
        {
            unsigned int attempt_id =
                connect_attempt_begin(attempt.attempt_profile, "supervisor", &link);
            attempt.last_connect_error =
                sceNetApctlConnect(attempt.attempt_profile);
            logLine("netmgr: connect_return attempt=%u phase=supervisor profile=%d rc=0x%08X\n",
                    attempt_id, attempt.attempt_profile,
                    (unsigned int)attempt.last_connect_error);
        }
        if (attempt.last_connect_error < 0) {
            attempt.pending = 0;
            attempt.retry_profile =
                next_configured_profile(attempt.attempt_profile);
            attempt.retry_at_us = sceKernelGetSystemTimeWide() +
                                  APCTL_RETRY_DELAY_US;
            supervisor_set_phase(NET_SUPERVISOR_BACKOFF);
            logLine("netmgr: runtime Connect profile=%d rc=0x%08X next=%d\n",
                    attempt.attempt_profile,
                    (unsigned int)attempt.last_connect_error,
                    attempt.retry_profile);
        }
    }
    return 0;
}

int net_stack_init(const NetTlsNetworkConfig *config)
{
    const NetTlsNetworkConfig *cfg = config ? config : &s_default_config;
    int rc;
    int profile;
    int retry_profile;
    if (s_stack_started) return 0;
    memset((void *)&s_link, 0, sizeof(s_link));
    memset(s_apctl_diag, 0, sizeof(s_apctl_diag));
    s_apctl_diag_write = 0U;
    s_apctl_diag_read = 0U;
    s_connect_attempt_id = 0U;
    s_connect_attempt_profile = 0;
    s_link.current_state = PSP_NET_APCTL_STATE_DISCONNECTED;
    s_last_profile = 0;
    s_runtime_stuck = 0;
    supervisor_set_phase(NET_SUPERVISOR_IDLE);

    if (!sceWlanGetSwitchState()) return -1;
    if ((rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON)) < 0) return rc;
    if ((rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET)) < 0) goto fail_common;
    log_all_profile_snapshots();
    retry_profile = select_retry_profile(cfg->wifi_profile_id);
    if (retry_profile < 0) {
        logLine("netmgr: no configured Wi-Fi profile\n");
        rc = -1;
        goto fail_inet_module;
    }
    s_last_profile = retry_profile;
    if ((rc = sceNetInit(cfg->pool_size, cfg->callout_prio, cfg->callout_stack,
                         cfg->netintr_prio, cfg->netintr_stack)) < 0)
        goto fail_inet_module;
    if ((rc = sceNetInetInit()) < 0) goto fail_net;
    if ((rc = sceNetApctlInit(cfg->apctl_stack_size, cfg->apctl_priority)) < 0)
        goto fail_inet;
    s_event_sema = sceKernelCreateSema("net_events", 0, 0, 1, NULL);
    if (s_event_sema < 0) { rc = s_event_sema; goto fail_apctl; }
    s_handler_id = sceNetApctlAddHandler(apctl_handler, NULL);
    if (s_handler_id < 0) { rc = s_handler_id; goto fail_sema; }

    s_stack_started = 1;
    if (dns_init() < 0) return NET_STACK_ERR_FATAL_LIVE_RUNTIME;
    profile = connect_initial_profile(cfg->wifi_profile_id);
    if (profile == NET_STACK_ERR_FATAL_LIVE_RUNTIME)
        return NET_STACK_ERR_FATAL_LIVE_RUNTIME;
    if (profile > 0) s_last_profile = profile;
    s_supervisor_running = 1;
    s_supervisor_thread = sceKernelCreateThread("net_supervisor", supervisor_main,
                                                0x19, 8 * 1024,
                                                PSP_THREAD_ATTR_USER, NULL);
    if (s_supervisor_thread < 0) return NET_STACK_ERR_FATAL_LIVE_RUNTIME;
    rc = sceKernelStartThread(s_supervisor_thread, 0, NULL);
    if (rc < 0) return NET_STACK_ERR_FATAL_LIVE_RUNTIME;
    if (profile > 0) {
        logLine("netmgr: online profile=%d generation=%u\n", profile,
                net_stack_get_generation());
    } else {
        logLine("netmgr: startup offline; supervisor retry profile=%d\n",
                s_last_profile);
    }
    return 0;

fail_sema:
    sceKernelDeleteSema(s_event_sema);
    s_event_sema = -1;
fail_apctl:
    sceNetApctlTerm();
fail_inet:
    sceNetInetTerm();
fail_net:
    sceNetTerm();
fail_inet_module:
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
fail_common:
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    return rc;
}

int net_stack_stop_supervisor(void)
{
    if (!s_stack_started) return 0;
    s_supervisor_running = 0;
    if (s_event_sema >= 0) (void)sceKernelSignalSema(s_event_sema, 1);
    if (s_supervisor_thread >= 0) {
        SceUInt timeout = 5000000U;
        if (sceKernelWaitThreadEnd(s_supervisor_thread, &timeout) < 0) return -1;
    }
    return 0;
}

int net_stack_is_ready(void)
{
    NetStackSnapshot link;
    if (net_stack_get_snapshot(&link) < 0) return 0;
    return link.apctl_state == PSP_NET_APCTL_STATE_GOT_IP;
}

int net_stack_is_stuck(void) { return s_runtime_stuck != 0; }

NetSupervisorPhase net_stack_get_supervisor_phase(void)
{
    return (NetSupervisorPhase)__sync_fetch_and_add(&s_supervisor_phase, 0U);
}

unsigned int net_stack_get_generation(void)
{
    NetStackSnapshot link;
    if (net_stack_get_snapshot(&link) < 0) return 0;
    return link.generation;
}
