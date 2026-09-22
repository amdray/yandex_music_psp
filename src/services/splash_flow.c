#include "services/splash_flow.h"

// #define DISABLE_NETWORK_INIT 1

#include <pspthreadman.h>
#include <string.h>

#include "services/locale.h"
#include "services/token_loader.h"
#include "services/net_client.h"
#include "services/net_stack.h"
#include "services/net_tls.h"
#include "core/logger.h"

static int auth_worker_thread(SceSize args __attribute__((unused)), void *argp)
{
    AuthThreadArgs *params = (AuthThreadArgs *)argp;
    if (!params || !params->token || !params->user_info || !params->result ||
        !params->cancel) {
        logLine("splash: auth thread invalid params\n");
        return -1;
    }

    logLine("splash: auth thread started\n");
    net_tls_cancel_bind(params->cancel);
    int ret = net_client_fetch_user_info(params->token, params->user_info);
    net_tls_cancel_unbind();
    // Measure real stack peak on the auth path (TLS handshake is the heavy part):
    // free = never-touched tail, so peak_used = actual stack size - free.
    {
        SceUID me = sceKernelGetThreadId();
        int stack_free = sceKernelGetThreadStackFreeSize(me);
        SceKernelThreadInfo info;
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(me, &info) == 0) {
            logLine("splash: auth_worker stack free=%d peak_used=%d of %d\n",
                    stack_free, info.stackSize - stack_free, info.stackSize);
        } else {
            logLine("splash: auth_worker stack free=%d\n", stack_free);
        }
    }
    // Write result (volatile ensures UI thread sees the update)
    *params->result = ret;
    logLine("splash: auth thread finished ret=%d\n", ret);
    sceKernelExitThread(0);
    return 0;
}

static void cleanup_auth_thread(SplashFlow *flow)
{
    if (flow->auth_thread_id >= 0) {
        SceKernelThreadRunStatus ts;
        ts.size = sizeof(SceKernelThreadRunStatus);
        if (sceKernelReferThreadRunStatus(flow->auth_thread_id, &ts) == 0) {
            if (ts.status == PSP_THREAD_STOPPED) {
                sceKernelDeleteThread(flow->auth_thread_id);
                flow->auth_thread_id = -1;
            }
        }
    }
}

static const char *splash_state_name(SplashState state)
{
    static const char *const names[] = {
        "storage", "cache", "index", "locale", "token", "net_start",
        "net_wait", "wlan_off", "net_error", "auth_start", "auth_wait",
        "auth_error", "ready"
    };
    return (state >= SPLASH_STATE_STORAGE && state <= SPLASH_STATE_READY)
        ? names[state] : "invalid";
}

static const char *splash_state_status(SplashState state)
{
    switch (state) {
    case SPLASH_STATE_STORAGE: return locale_get(LOCALE_SPLASH_STORAGE);
    case SPLASH_STATE_CACHE: return locale_get(LOCALE_SPLASH_CACHE);
    case SPLASH_STATE_INDEX: return locale_get(LOCALE_SPLASH_INDEX);
    case SPLASH_STATE_LOCALE: return locale_get(LOCALE_SPLASH_LOCALE);
    case SPLASH_STATE_TOKEN: return locale_get(LOCALE_SPLASH_TOKEN);
    case SPLASH_STATE_NET_START:
    case SPLASH_STATE_NET_WAIT: return locale_get(LOCALE_SPLASH_NET);
    case SPLASH_STATE_WLAN_OFF: return locale_get(LOCALE_SPLASH_WLAN_OFF);
    case SPLASH_STATE_NET_ERROR: return locale_get(LOCALE_SPLASH_NET_ERROR);
    case SPLASH_STATE_AUTH_START:
    case SPLASH_STATE_AUTH_WAIT: return locale_get(LOCALE_SPLASH_AUTH);
    case SPLASH_STATE_AUTH_ERROR: return locale_get(LOCALE_SPLASH_AUTH_ERROR);
    case SPLASH_STATE_READY: return NULL;
    }
    return NULL;
}

static void splash_set_state(SplashFlow *flow, SplashState next)
{
    SplashState previous;
    if (!flow || flow->state == next) return;
    previous = flow->state;
    flow->state = next;
    flow->status = splash_state_status(next);
    flow->ready = (next == SPLASH_STATE_READY);
    logLine("splash: state %s -> %s\n",
            splash_state_name(previous), splash_state_name(next));
}

void splash_flow_init(SplashFlow *flow)
{
    if (!flow) return;
    memset(flow, 0, sizeof(*flow));
    flow->state = SPLASH_STATE_STORAGE;
    flow->status = splash_state_status(flow->state);
    flow->auth_thread_id = -1;
    flow->auth_result = -2;
    logLine("splash: state -> %s\n", splash_state_name(flow->state));
}

int splash_flow_has_error(const SplashFlow *flow)
{
    if (!flow) return 0;
    return flow->state == SPLASH_STATE_WLAN_OFF ||
           flow->state == SPLASH_STATE_NET_ERROR ||
           flow->state == SPLASH_STATE_AUTH_ERROR;
}

static void apply_retry_reset(SplashFlow *flow)
{
    if (flow->state == SPLASH_STATE_AUTH_ERROR) {
        flow->auth_result = -2;
        flow->auth_cancel = 0;
        splash_set_state(flow, SPLASH_STATE_AUTH_START);
    } else {
        flow->wlan_check_at_us = 0;
        splash_set_state(flow, SPLASH_STATE_NET_START);
    }
}

void splash_flow_retry(SplashFlow *flow)
{
    if (!flow) {
        return;
    }

    if (flow->auth_thread_id >= 0) {
        SceKernelThreadRunStatus status;
        status.size = sizeof(SceKernelThreadRunStatus);
        if (sceKernelReferThreadRunStatus(flow->auth_thread_id, &status) == 0 &&
            status.status == PSP_THREAD_STOPPED) {
            sceKernelDeleteThread(flow->auth_thread_id);
            flow->auth_thread_id = -1;
        } else {
            // Never terminate: the worker is bounded by the TLS I/O deadline
            // and will report STOPPED on its own. Defer the reset to
            // splash_flow_tick, which retries this check every frame.
            flow->retry_pending = 1;
            logLine("splash: retry deferred, auth thread not stopped yet\n");
            return;
        }
    }

    apply_retry_reset(flow);
}

int splash_flow_quiesce(SplashFlow *flow)
{
    if (!flow || flow->auth_thread_id < 0) return 0;
    flow->auth_cancel = 1;
    {
        SceUInt timeout_us = 40000000U;
        if (sceKernelWaitThreadEnd(flow->auth_thread_id, &timeout_us) < 0) {
            logLine("splash: auth worker still active; resources retained\n");
            return -1;
        }
    }
    return 0;
}

void splash_flow_tick(SplashFlow *flow, AppState *app)
{
    if (!flow) {
        return;
    }

    if (flow->retry_pending) {
        SceKernelThreadRunStatus status;
        status.size = sizeof(SceKernelThreadRunStatus);
        int stopped = (flow->auth_thread_id < 0) ||
                      (sceKernelReferThreadRunStatus(flow->auth_thread_id, &status) == 0 &&
                       status.status == PSP_THREAD_STOPPED);
        if (!stopped) {
            return;  // keep waiting, check again next tick
        }
        if (flow->auth_thread_id >= 0) {
            sceKernelDeleteThread(flow->auth_thread_id);
            flow->auth_thread_id = -1;
        }
        flow->retry_pending = 0;
        apply_retry_reset(flow);
    }

    if (flow->ready) return;

    switch (flow->state) {
    case SPLASH_STATE_STORAGE:
        splash_set_state(flow, SPLASH_STATE_CACHE);
        break;
    case SPLASH_STATE_CACHE:
        splash_set_state(flow, SPLASH_STATE_INDEX);
        break;
    case SPLASH_STATE_INDEX:
        splash_set_state(flow, SPLASH_STATE_LOCALE);
        break;
    case SPLASH_STATE_LOCALE:
        locale_init("ru");
        splash_set_state(flow, SPLASH_STATE_TOKEN);
        break;
    case SPLASH_STATE_TOKEN:
        if (token_loader_read(flow->token, sizeof(flow->token)) != 0) {
            logLine("splash: no token -> guest session\n");
            flow->token[0] = '\0';
        }
        splash_set_state(flow, SPLASH_STATE_NET_START);
        break;
    case SPLASH_STATE_NET_START:
#ifdef DISABLE_NETWORK_INIT
        splash_set_state(flow, SPLASH_STATE_AUTH_START);
#else
        {
            int rc = net_client_init(0);
            if (rc == NET_STACK_ERR_WLAN_OFF) {
                flow->wlan_check_at_us = sceKernelGetSystemTimeWide() + 250000ULL;
                splash_set_state(flow, SPLASH_STATE_WLAN_OFF);
            } else if (rc < 0) {
                logLine("splash: network start failed rc=%d\n", rc);
                splash_set_state(flow, SPLASH_STATE_NET_ERROR);
            } else {
                splash_set_state(flow, SPLASH_STATE_NET_WAIT);
            }
        }
#endif
        break;
    case SPLASH_STATE_NET_WAIT:
        net_client_poll();
        if (net_client_has_error()) {
            splash_set_state(flow, SPLASH_STATE_NET_ERROR);
        } else if (net_client_is_ready()) {
            splash_set_state(flow, SPLASH_STATE_AUTH_START);
        }
        break;
    case SPLASH_STATE_WLAN_OFF:
        {
            u64 now = sceKernelGetSystemTimeWide();
            if (now >= flow->wlan_check_at_us) {
                flow->wlan_check_at_us = now + 250000ULL;
                if (net_stack_wlan_switch_is_on())
                    splash_set_state(flow, SPLASH_STATE_NET_START);
            }
        }
        break;
    case SPLASH_STATE_NET_ERROR:
    case SPLASH_STATE_AUTH_ERROR:
        break;
    case SPLASH_STATE_AUTH_START:
        if (!flow->token[0]) {
            logLine("splash: empty token -> menu as guest\n");
            splash_set_state(flow, SPLASH_STATE_READY);
            break;
        }
        flow->auth_result = -2;
        flow->auth_cancel = 0;
        flow->auth_thread_args.token = flow->token;
        flow->auth_thread_args.user_info = &flow->auth_user_info;
        flow->auth_thread_args.result = &flow->auth_result;
        flow->auth_thread_args.cancel = &flow->auth_cancel;
        flow->auth_thread_id = sceKernelCreateThread("auth_worker", auth_worker_thread,
                                                     0x18, 64 * 1024, 0, NULL);
        if (flow->auth_thread_id < 0) {
            logLine("splash: failed to create auth thread 0x%08X\n",
                    flow->auth_thread_id);
            flow->auth_result = -1;
            splash_set_state(flow, SPLASH_STATE_AUTH_ERROR);
            break;
        }
        {
            int rc = sceKernelStartThread(flow->auth_thread_id,
                                          sizeof(AuthThreadArgs),
                                          &flow->auth_thread_args);
            if (rc < 0) {
                logLine("splash: failed to start auth thread 0x%08X\n", rc);
                sceKernelDeleteThread(flow->auth_thread_id);
                flow->auth_thread_id = -1;
                flow->auth_result = -1;
                splash_set_state(flow, SPLASH_STATE_AUTH_ERROR);
            } else {
                splash_set_state(flow, SPLASH_STATE_AUTH_WAIT);
            }
        }
        break;
    case SPLASH_STATE_AUTH_WAIT:
        if (flow->auth_result == 0) {
            memcpy(&app->currentUser, &flow->auth_user_info, sizeof(UserInfo));
            cleanup_auth_thread(flow);
            splash_set_state(flow, SPLASH_STATE_READY);
        } else if (flow->auth_result == -1) {
            cleanup_auth_thread(flow);
            splash_set_state(flow, SPLASH_STATE_AUTH_ERROR);
        }
        break;
    case SPLASH_STATE_READY:
        break;
    }
}
