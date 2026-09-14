#include "services/splash_flow.h"

// #define DISABLE_NETWORK_INIT 1

#include <pspthreadman.h>
#include <string.h>

#include "services/locale.h"
#include "services/token_loader.h"
#include "services/net_client.h"
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

void splash_flow_init(SplashFlow *flow)
{
    if (!flow) {
        return;
    }
    flow->status = NULL;  // Will be set in first tick
    flow->ready = 0;
    flow->stage = 0;
    flow->token[0] = '\0';
    flow->net_started = 0;
    flow->auth_attempted = 0;
    flow->auth_thread_id = -1;
    flow->auth_result = -2;  // -2 = pending (not started / in progress)
    flow->auth_cancel = 0;
    flow->error_stage = 0;
    flow->retry_pending = 0;
    memset(&flow->auth_user_info, 0, sizeof(UserInfo));
}

int splash_flow_has_error(const SplashFlow *flow)
{
    if (!flow) {
        return 0;
    }
    return (flow->error_stage != 0);
}

// Applies the stage reset for the pending error_stage. Only safe to call once
// auth_thread_id is confirmed gone (see splash_flow_retry / splash_flow_tick).
static void apply_retry_reset(SplashFlow *flow)
{
    switch (flow->error_stage) {
    case 4:
        // Retry from token stage
        flow->stage = 4;
        flow->auth_attempted = 0;
        break;
    case 5:
        /* Retry follows only a fully unwound pre-handler init failure. A
         * post-handler failure requires top-level process exit instead. */
        flow->stage = 5;
        flow->net_started = 0;
        flow->auth_attempted = 0;
        break;
    case 6:
        // Retry from auth stage
        flow->stage = 6;
        flow->auth_attempted = 0;
        flow->auth_result = -2;
        break;
    }
    flow->error_stage = 0;
    flow->ready = 0;
    logLine("splash: retry from stage %d\n", flow->stage);
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

    if (flow->ready) {
        return;
    }

    switch (flow->stage) {
    case 0:
        flow->status = locale_get(LOCALE_SPLASH_STORAGE);
        logLine("splash: status -> %s\n", flow->status);
        flow->stage++;
        break;
    case 1:
        flow->status = locale_get(LOCALE_SPLASH_CACHE);
        logLine("splash: status -> %s\n", flow->status);
        flow->stage++;
        break;
    case 2:
        flow->status = locale_get(LOCALE_SPLASH_INDEX);
        logLine("splash: status -> %s\n", flow->status);
        flow->stage++;
        break;
    case 3:
        flow->status = locale_get(LOCALE_SPLASH_LOCALE);
        logLine("splash: status -> %s\n", flow->status);
        locale_init("ru");
        flow->stage++;
        break;
    case 4:
        flow->status = locale_get(LOCALE_SPLASH_TOKEN);
        logLine("splash: status -> %s\n", flow->status);
        if (token_loader_read(flow->token, sizeof(flow->token)) == 0) {
            flow->stage++;
        } else {
            /* Без токена не стоим: дальше сеть, пропуск auth и меню —
             * войти можно пунктом «Профиль» (ya_auth). */
            logLine("splash: no token -> menu, login via Profile\n");
            flow->token[0] = '\0';
            flow->stage++;
        }
        break;
    case 5:
        flow->status = locale_get(LOCALE_SPLASH_NET);
        if (!flow->net_started) {
            /* Первый тик: только обновляем статус, рендерим кадр, init на следующем тике */
            flow->net_started = 1;
            logLine("splash: status -> %s\n", flow->status);
            break;
        }
#ifdef DISABLE_NETWORK_INIT
        logLine("splash: network disabled for debugging\n");
        flow->stage++;
#else
        if (flow->net_started == 1) {
            flow->net_started = 2;
            u64 ni_start = sceKernelGetSystemTimeWide();
            logLine("splash: net_client_init begin (sync Wi-Fi bring-up)\n");
            logger_flush();
            int ni_rc = net_client_init(0);
            logLine("splash: net_client_init done rc=%d took=%llu ms\n",
                    ni_rc, (sceKernelGetSystemTimeWide() - ni_start) / 1000);
            logger_flush();
            if (ni_rc < 0) {
                flow->status = locale_get(LOCALE_SPLASH_NET_ERROR);
                logLine("splash: status -> %s\n", flow->status);
                flow->error_stage = 5;
                break;
            }
        }
        net_client_poll();
        {
            /* Main-thread tick only, so a plain static counter is safe here. */
            static unsigned int s_net_wait_ticks = 0;
            s_net_wait_ticks++;
            if (s_net_wait_ticks == 1 || s_net_wait_ticks % 60 == 0) {
                logLine("splash: net wait tick=%u ready=%d err=%d\n",
                        s_net_wait_ticks, net_client_is_ready(), net_client_has_error());
                logger_flush();
            }
        }
        if (net_client_has_error()) {
            flow->status = locale_get(LOCALE_SPLASH_NET_ERROR);
            logLine("splash: status -> %s\n", flow->status);
            flow->error_stage = 5;
        } else if (net_client_is_ready()) {
            logLine("splash: net ready -> auth stage\n");
            flow->stage++;
        }
#endif
        break;
    case 6:
        if (!flow->token[0]) {
            /* Токена нет (пришли из меню без входа): auth нечего проверять —
             * сразу в меню, профиль пуст. Вход — пункт «Профиль». */
            logLine("splash: empty token -> menu as guest\n");
            flow->status = NULL;
            flow->ready = 1;
            break;
        }
        if (!flow->auth_attempted) {
            logLine("splash: starting auth (stage 6)\n");
            flow->auth_attempted = 1;
            flow->status = locale_get(LOCALE_SPLASH_AUTH);
            logLine("splash: status -> %s\n", flow->status);
            flow->auth_result = -2;  // in progress
            
            // Prepare thread arguments (stored in flow to outlive this block)
            flow->auth_thread_args.token = flow->token;
            flow->auth_thread_args.user_info = &flow->auth_user_info;
            flow->auth_thread_args.result = &flow->auth_result;
            flow->auth_thread_args.cancel = &flow->auth_cancel;
            flow->auth_cancel = 0;
            
            // Create worker thread
            flow->auth_thread_id = sceKernelCreateThread("auth_worker", auth_worker_thread,
                                                         0x18,  // Priority (higher than UI at 0x20)
                                                         64 * 1024,  // 64KB stack
                                                         0, NULL);
            if (flow->auth_thread_id < 0) {
                logLine("splash: failed to create auth thread 0x%08X\n", flow->auth_thread_id);
                flow->status = locale_get(LOCALE_SPLASH_AUTH_ERROR);
                logLine("splash: status -> %s\n", flow->status);
                flow->auth_result = -1;
                flow->error_stage = 6;
            } else {
                logLine("splash: auth thread created id=%d\n", flow->auth_thread_id);
                // Start thread
                int start_ret = sceKernelStartThread(flow->auth_thread_id, sizeof(AuthThreadArgs), &flow->auth_thread_args);
                if (start_ret < 0) {
                    logLine("splash: failed to start auth thread 0x%08X\n", start_ret);
                    sceKernelDeleteThread(flow->auth_thread_id);
                    flow->auth_thread_id = -1;
                    flow->status = locale_get(LOCALE_SPLASH_AUTH_ERROR);
                    logLine("splash: status -> %s\n", flow->status);
                    flow->auth_result = -1;
                    flow->error_stage = 6;
                } else {
                    logLine("splash: auth thread started successfully\n");
                }
            }
        } else {
            // Check thread result (auth_result is updated by worker thread)
            // Field is already volatile, so read is safe
            int result = flow->auth_result;
            if (result == 0) {
                // Success - account status received and parsed successfully
                // Copy user info to app state
                memcpy(&app->currentUser, &flow->auth_user_info, sizeof(UserInfo));
                flow->status = NULL;  // No status needed when ready
                flow->ready = 1;  // Splash ends after successful account parsing
                logLine("splash: ready (account parsed successfully)\n");
                cleanup_auth_thread(flow);
            } else if (result == -1) {
                // Error
                flow->status = locale_get(LOCALE_SPLASH_AUTH_ERROR);
                logLine("splash: status -> %s\n", flow->status);
                logLine("splash: auth failed\n");
                flow->error_stage = 6;
                cleanup_auth_thread(flow);
            }
            // else: still in progress (auth_result == -2), keep showing "auth" status
        }
        break;
    default:
        break;
    }
}
