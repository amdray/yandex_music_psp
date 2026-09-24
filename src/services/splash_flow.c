#include "services/splash_flow.h"

// #define DISABLE_NETWORK_INIT 1

#include <pspthreadman.h>
#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "services/locale.h"
#include "services/image_loader.h"
#include "services/token_loader.h"
#include "services/net_client.h"
#include "services/net_http.h"
#include "services/net_stack.h"
#include "services/net_tls.h"
#include "services/time_sync.h"
#include "services/ym_api_genres.h"
#include "ui/ui_palette.h"
#include "core/logger.h"

#define SPLASH_IMAGE_URL "https://music.yandex.ru/web-app-manifest-192x192.png"
#define SPLASH_IMAGE_CACHE_PATH "data/cache/splash.png"
#define SPLASH_IMAGE_STAGE_PATH "data/cache/splash.download"
#define SPLASH_IMAGE_WIDTH 192
#define SPLASH_IMAGE_HEIGHT 192
#define SPLASH_IMAGE_MAX_BYTES (64 * 1024)

typedef struct SplashImageDownload {
    unsigned char *data;
    int size;
    volatile int *cancel;
} SplashImageDownload;

static int splash_image_chunk(const char *data, int size, void *user)
{
    SplashImageDownload *download = (SplashImageDownload *)user;

    if (!download || !data || size < 0) return -1;
    if (__sync_fetch_and_add(download->cancel, 0)) {
        return NET_HTTP_STREAM_CANCELLED;
    }
    if (size > SPLASH_IMAGE_MAX_BYTES - download->size) {
        logLine("splash: image exceeds %d-byte limit\n", SPLASH_IMAGE_MAX_BYTES);
        return -1;
    }
    memcpy(download->data + download->size, data, (size_t)size);
    download->size += size;
    return 0;
}

static int splash_image_progress(int written, int content_length, void *user)
{
    SplashImageDownload *download = (SplashImageDownload *)user;

    (void)written;
    if (!download) return -1;
    if (__sync_fetch_and_add(download->cancel, 0)) {
        return NET_HTTP_STREAM_CANCELLED;
    }
    if (content_length > SPLASH_IMAGE_MAX_BYTES) {
        logLine("splash: image content length too large=%d\n", content_length);
        return -1;
    }
    return 0;
}

static int splash_image_worker_thread(SceSize args __attribute__((unused)),
                                      void *argp)
{
    SplashImageThreadArgs *params = (SplashImageThreadArgs *)argp;
    SplashImageDownload download;
    int result = -1;

    if (!params || !params->cancel || !params->published || !params->result ||
        !params->data || !params->size) {
        return -1;
    }
    memset(&download, 0, sizeof(download));
    download.cancel = params->cancel;
    download.data = (unsigned char *)malloc(SPLASH_IMAGE_MAX_BYTES);
    if (!download.data) {
        logLine("splash: image download buffer allocation failed\n");
        goto publish;
    }

    logLine("splash: image download started\n");
    net_tls_cancel_bind(params->cancel);
    result = http_stream(
        &(HttpRequest){
            .method = HTTP_GET,
            .url = SPLASH_IMAGE_URL,
            .accept = "image/png",
            .identity_encoding = 1,
        },
        &(HttpSink){
            .on_chunk = splash_image_chunk,
            .on_progress = splash_image_progress,
            .user = &download,
        });
    net_tls_cancel_unbind();

    if (result != 0 || download.size <= 0 ||
        __sync_fetch_and_add(params->cancel, 0)) {
        logLine("splash: image download failed rc=%d size=%d\n",
                result, download.size);
        result = -1;
        goto publish;
    }
    *params->data = download.data;
    *params->size = download.size;
    download.data = NULL;
    result = 0;
    logLine("splash: image downloaded bytes=%d\n", *params->size);

publish:
    free(download.data);
    *params->result = result;
    __sync_synchronize();
    __sync_lock_test_and_set(params->published, 1U);
    return 0;
}

static int time_sync_worker_thread(SceSize args __attribute__((unused)),
                                   void *argp)
{
    TimeSyncThreadArgs *params = (TimeSyncThreadArgs *)argp;
    TimeSyncResult result;
    if (!params || !params->cancel || !params->published || !params->result)
        return -1;

    memset(&result, 0, sizeof(result));
    result.status = time_sync_run(params->cancel, &result);
    *params->result = result;
    __sync_synchronize();
    __sync_lock_test_and_set(params->published, 1U);
    return 0;
}

static void splash_image_composite_background(void *pixels, int width,
                                              int height, int stride_bytes)
{
    const unsigned int bg = UI_COLOR_BACKGROUND;
    const unsigned int bg_r = bg & 0xFFU;
    const unsigned int bg_g = (bg >> 8) & 0xFFU;
    const unsigned int bg_b = (bg >> 16) & 0xFFU;
    unsigned char *row = (unsigned char *)pixels;
    int y;

    for (y = 0; y < height; y++, row += stride_bytes) {
        int x;
        for (x = 0; x < width; x++) {
            unsigned char *p = row + x * 4;
            unsigned int alpha = p[3];
            unsigned int inverse = 255U - alpha;
            p[0] = (unsigned char)((p[0] * alpha + bg_r * inverse + 127U) / 255U);
            p[1] = (unsigned char)((p[1] * alpha + bg_g * inverse + 127U) / 255U);
            p[2] = (unsigned char)((p[2] * alpha + bg_b * inverse + 127U) / 255U);
            p[3] = 0xFF;
        }
    }
}

static int splash_image_decode(const char *path, void **out_pixels,
                               int *out_stride_bytes)
{
    void *pixels = NULL;
    int width = 0;
    int height = 0;
    int stride_bytes = 0;

    if (image_load_rgba8888(path, &pixels, &width, &height,
                            &stride_bytes) != 0) {
        return -1;
    }
    if (width != SPLASH_IMAGE_WIDTH || height != SPLASH_IMAGE_HEIGHT ||
        stride_bytes < SPLASH_IMAGE_WIDTH * 4) {
        logLine("splash: rejected image dimensions=%dx%d stride=%d\n",
                width, height, stride_bytes);
        image_free_rgba8888(pixels);
        return -1;
    }
    splash_image_composite_background(pixels, width, height, stride_bytes);
    *out_pixels = pixels;
    *out_stride_bytes = stride_bytes;
    return 0;
}

static int splash_image_load_cache(SplashFlow *flow)
{
    SceIoStat stat;
    void *pixels = NULL;
    int stride_bytes = 0;

    if (fs_getstat(SPLASH_IMAGE_CACHE_PATH, &stat) != 0) {
        logLine("splash: image cache miss\n");
        return -1;
    }
    if (splash_image_decode(SPLASH_IMAGE_CACHE_PATH, &pixels,
                            &stride_bytes) != 0) {
        logLine("splash: invalid image cache removed\n");
        fs_remove(SPLASH_IMAGE_CACHE_PATH);
        return -1;
    }
    flow->image_pixels = pixels;
    flow->image_w = SPLASH_IMAGE_WIDTH;
    flow->image_h = SPLASH_IMAGE_HEIGHT;
    flow->image_stride_bytes = stride_bytes;
    logLine("splash: image cache loaded %dx%d\n",
            SPLASH_IMAGE_WIDTH, SPLASH_IMAGE_HEIGHT);
    return 0;
}

static void splash_image_finish_download(SplashFlow *flow)
{
    void *pixels = NULL;
    int stride_bytes = 0;
    int stage_written = 0;

    if (!flow->image_download_data || flow->image_download_size <= 0) {
        logLine("splash: image worker published no data\n");
        return;
    }
    if (fs_ensure_dir(SPLASH_IMAGE_STAGE_PATH) == 0 &&
        fs_write_atomic(SPLASH_IMAGE_STAGE_PATH, flow->image_download_data,
                        (size_t)flow->image_download_size) >= 0) {
        stage_written = 1;
    }
    if (!stage_written ||
        splash_image_decode(SPLASH_IMAGE_STAGE_PATH, &pixels,
                            &stride_bytes) != 0) {
        logLine("splash: downloaded image validation failed\n");
        fs_remove(SPLASH_IMAGE_STAGE_PATH);
        return;
    }

    if (fs_write_atomic(SPLASH_IMAGE_CACHE_PATH, flow->image_download_data,
                        (size_t)flow->image_download_size) < 0) {
        logLine("splash: image cache write failed\n");
    } else {
        logLine("splash: image cache updated bytes=%d\n",
                flow->image_download_size);
    }
    fs_remove(SPLASH_IMAGE_STAGE_PATH);
    image_free_rgba8888(flow->image_pixels);
    flow->image_pixels = pixels;
    flow->image_w = SPLASH_IMAGE_WIDTH;
    flow->image_h = SPLASH_IMAGE_HEIGHT;
    flow->image_stride_bytes = stride_bytes;
}

static void splash_image_discard_download(SplashFlow *flow)
{
    free(flow->image_download_data);
    flow->image_download_data = NULL;
    flow->image_download_size = 0;
}

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
    if (ret == 0) {
        const char *language = locale_get_current_lang() == LOCALE_LANG_RU
                                   ? "ru" : "en";
        if (ym_api_genres_load(params->token, language) != 0) {
            logLine("splash: genre catalog unavailable\n");
        }
    }
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
    /* Publish the completed user data and genre catalog before the UI can
     * observe the terminal result. */
    __sync_synchronize();
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
        "net_wait", "time_sync_start", "time_sync_wait", "image_start",
        "image_wait", "wlan_off", "net_error", "auth_start", "auth_wait",
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
    case SPLASH_STATE_NET_WAIT:
    case SPLASH_STATE_TIME_SYNC_START:
    case SPLASH_STATE_TIME_SYNC_WAIT:
    case SPLASH_STATE_IMAGE_START:
    case SPLASH_STATE_IMAGE_WAIT: return locale_get(LOCALE_SPLASH_NET);
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
    flow->time_sync_thread_id = -1;
    flow->image_thread_id = -1;
    flow->image_result = -2;
    flow->auth_thread_id = -1;
    flow->auth_result = -2;
    fs_remove(SPLASH_IMAGE_STAGE_PATH);
    splash_image_load_cache(flow);
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
    int quiesced = 1;
    if (!flow) return 0;
    if (flow->time_sync_thread_id >= 0) {
        SceUInt timeout_us = 10000000U;
        __sync_lock_test_and_set(&flow->time_sync_cancel, 1U);
        if (sceKernelWaitThreadEnd(flow->time_sync_thread_id, &timeout_us) < 0) {
            logLine("splash: time sync worker still active; resources retained\n");
            quiesced = 0;
        }
    }
    if (flow->image_thread_id >= 0) {
        SceUInt timeout_us = 40000000U;
        __sync_lock_test_and_set(&flow->image_cancel, 1);
        if (sceKernelWaitThreadEnd(flow->image_thread_id, &timeout_us) < 0) {
            logLine("splash: image worker still active; resources retained\n");
            quiesced = 0;
        } else {
            sceKernelDeleteThread(flow->image_thread_id);
            flow->image_thread_id = -1;
            splash_image_discard_download(flow);
        }
    }
    if (flow->auth_thread_id >= 0) {
        SceUInt timeout_us = 40000000U;
        __sync_lock_test_and_set(&flow->auth_cancel, 1);
        if (sceKernelWaitThreadEnd(flow->auth_thread_id, &timeout_us) < 0) {
            logLine("splash: auth worker still active; resources retained\n");
            quiesced = 0;
        }
    }
    return quiesced ? 0 : -1;
}

void splash_flow_release_image(SplashFlow *flow)
{
    if (!flow) return;
    image_free_rgba8888(flow->image_pixels);
    flow->image_pixels = NULL;
    flow->image_w = 0;
    flow->image_h = 0;
    flow->image_stride_bytes = 0;
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
            splash_set_state(flow, SPLASH_STATE_TIME_SYNC_START);
        }
        break;
    case SPLASH_STATE_TIME_SYNC_START:
        memset(&flow->time_sync_result, 0, sizeof(flow->time_sync_result));
        flow->time_sync_result.status = TIME_SYNC_ERR_NETWORK;
        flow->time_sync_cancel = 0U;
        flow->time_sync_published = 0U;
        flow->time_sync_thread_args.cancel = &flow->time_sync_cancel;
        flow->time_sync_thread_args.published = &flow->time_sync_published;
        flow->time_sync_thread_args.result = &flow->time_sync_result;
        flow->time_sync_thread_id = sceKernelCreateThread(
            "time_sync_worker", time_sync_worker_thread, 0x18, 32 * 1024,
            PSP_THREAD_ATTR_USER, NULL);
        if (flow->time_sync_thread_id < 0) {
            logLine("splash: failed to create time sync thread 0x%08X\n",
                    flow->time_sync_thread_id);
            flow->time_sync_thread_id = -1;
            splash_set_state(flow, SPLASH_STATE_IMAGE_START);
            break;
        }
        {
            int rc = sceKernelStartThread(flow->time_sync_thread_id,
                                          sizeof(TimeSyncThreadArgs),
                                          &flow->time_sync_thread_args);
            if (rc < 0) {
                logLine("splash: failed to start time sync thread 0x%08X\n",
                        rc);
                sceKernelDeleteThread(flow->time_sync_thread_id);
                flow->time_sync_thread_id = -1;
                splash_set_state(flow, SPLASH_STATE_IMAGE_START);
            } else {
                splash_set_state(flow, SPLASH_STATE_TIME_SYNC_WAIT);
            }
        }
        break;
    case SPLASH_STATE_TIME_SYNC_WAIT:
        if (flow->time_sync_thread_id >= 0) {
            SceKernelThreadRunStatus status;
            status.size = sizeof(status);
            if (sceKernelReferThreadRunStatus(flow->time_sync_thread_id,
                                              &status) == 0 &&
                status.status == PSP_THREAD_STOPPED) {
                unsigned int published = __sync_fetch_and_add(
                    &flow->time_sync_published, 0U);
                sceKernelDeleteThread(flow->time_sync_thread_id);
                flow->time_sync_thread_id = -1;
                if (!published) {
                    logLine("time_sync: worker stopped without result\n");
                    splash_set_state(flow, SPLASH_STATE_IMAGE_START);
                } else {
                    logLine("time_sync: status=%d offset_us=%lld set_rc=%d\n",
                            flow->time_sync_result.status,
                            flow->time_sync_result.offset_us,
                            flow->time_sync_result.rtc_set_result);
                    if (flow->time_sync_result.status == TIME_SYNC_ERR_OFFLINE)
                        splash_set_state(flow, SPLASH_STATE_NET_WAIT);
                    else
                        splash_set_state(flow, SPLASH_STATE_IMAGE_START);
                }
            }
        } else {
            splash_set_state(flow, SPLASH_STATE_IMAGE_START);
        }
        break;
    case SPLASH_STATE_IMAGE_START:
        flow->image_cancel = 0;
        flow->image_published = 0U;
        flow->image_result = -2;
        splash_image_discard_download(flow);
        flow->image_thread_args.cancel = &flow->image_cancel;
        flow->image_thread_args.published = &flow->image_published;
        flow->image_thread_args.result = &flow->image_result;
        flow->image_thread_args.data = &flow->image_download_data;
        flow->image_thread_args.size = &flow->image_download_size;
        flow->image_thread_id = sceKernelCreateThread(
            "splash_image_worker", splash_image_worker_thread, 0x18,
            64 * 1024, PSP_THREAD_ATTR_USER, NULL);
        if (flow->image_thread_id < 0) {
            logLine("splash: failed to create image thread 0x%08X\n",
                    flow->image_thread_id);
            flow->image_thread_id = -1;
            splash_set_state(flow, SPLASH_STATE_AUTH_START);
            break;
        }
        {
            int rc = sceKernelStartThread(flow->image_thread_id,
                                          sizeof(SplashImageThreadArgs),
                                          &flow->image_thread_args);
            if (rc < 0) {
                logLine("splash: failed to start image thread 0x%08X\n", rc);
                sceKernelDeleteThread(flow->image_thread_id);
                flow->image_thread_id = -1;
                splash_set_state(flow, SPLASH_STATE_AUTH_START);
            } else {
                splash_set_state(flow, SPLASH_STATE_IMAGE_WAIT);
            }
        }
        break;
    case SPLASH_STATE_IMAGE_WAIT:
        if (flow->image_thread_id >= 0) {
            SceKernelThreadRunStatus status;
            status.size = sizeof(status);
            if (sceKernelReferThreadRunStatus(flow->image_thread_id,
                                              &status) == 0 &&
                status.status == PSP_THREAD_STOPPED) {
                unsigned int published = __sync_fetch_and_add(
                    &flow->image_published, 0U);
                sceKernelDeleteThread(flow->image_thread_id);
                flow->image_thread_id = -1;
                if (published && flow->image_result == 0) {
                    splash_image_finish_download(flow);
                } else {
                    logLine("splash: image unavailable result=%d published=%u\n",
                            flow->image_result, published);
                }
                splash_image_discard_download(flow);
                splash_set_state(flow, SPLASH_STATE_AUTH_START);
            }
        } else {
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
            __sync_synchronize();
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
