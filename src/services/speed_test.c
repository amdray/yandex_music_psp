/* Тест скорости сети для PSP: реальный CDN-файл приложения, ~10 секунд,
 * байты не пишутся на диск. Итог — в app.log (speedtest:) и в строке
 * настроек «Тест скорости». */
#include "services/speed_test.h"

#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/locale.h"
#include "services/net_http.h"
#include "services/net_stack.h"
#include "services/playback_controller.h"
#include "services/token_loader.h"
#include "services/ym_api.h"

#define SPEED_TEST_DURATION_MS 10000
#define SPEED_TEST_TICKET_PRIO 0x18
#define SPEED_TEST_STACK       0x10000

typedef enum {
    ST_IDLE = 0,
    ST_RUNNING,
    ST_DONE,
    ST_NO_TRACK,
    ST_FAILED
} SpeedTestState;

static volatile SpeedTestState s_state = ST_IDLE;
static char s_result_text[32];

typedef struct {
    u64 start_us;
    u64 window_us;
    int window_bytes;
    int total_bytes;
    int peak_bps;
} SpeedTestCtx;

static int speed_test_chunk(const char *data, int size, void *user)
{
    SpeedTestCtx *ctx = (SpeedTestCtx *)user;
    u64 now;
    (void)data;
    if (!ctx || size <= 0) return -1;
    ctx->total_bytes += size;
    ctx->window_bytes += size;
    now = sceKernelGetSystemTimeWide();
    if (now - ctx->window_us >= 1000000ULL) {
        int bps = (int)((int64_t)ctx->window_bytes * 1000000LL /
                        (int64_t)(now - ctx->window_us));
        if (bps > ctx->peak_bps) ctx->peak_bps = bps;
        ctx->window_us = now;
        ctx->window_bytes = 0;
        logLine("speedtest: t=%d s total=%d %d KB/s\n",
                (int)((now - ctx->start_us) / 1000000ULL),
                ctx->total_bytes, bps / 1024);
    }
    if (now - ctx->start_us >= (u64)SPEED_TEST_DURATION_MS * 1000ULL) {
        return NET_HTTP_STREAM_CANCELLED;
    }
    return 0;
}

/* Track id для переговоров URL: текущий трек иначе первый файл кэша. */
static int speed_test_pick_track(char *out, size_t out_size)
{
    char abs_dir[256];
    SceUID dfd;
    SceIoDirent ent;

    if (g_playback.current_track.id[0] != '\0') {
        snprintf(out, out_size, "%s", g_playback.current_track.id);
        return 0;
    }
    if (fs_make_abs_path("cache_music", abs_dir, sizeof(abs_dir)) != 0) {
        return -1;
    }
    memset(&ent, 0, sizeof(ent));
    dfd = sceIoDopen(abs_dir);
    if (dfd < 0) return -1;
    while (sceIoDread(dfd, &ent) > 0) {
        char *dot;
        if (ent.d_name[0] == '\0' || ent.d_name[0] == '.' ||
            FIO_S_ISDIR(ent.d_stat.st_mode)) {
            continue;
        }
        dot = strchr(ent.d_name, '.');
        if (dot) *dot = '\0';
        snprintf(out, out_size, "%.*s", (int)out_size - 1, ent.d_name);
        sceIoDclose(dfd);
        return 0;
    }
    sceIoDclose(dfd);
    return -1;
}

static int speed_test_worker(SceSize args, void *argp)
{
    char token[256];
    char track_id[40];
    char url[512];
    YmApiContext ctx;
    YmDownloadVariant variant;
    SpeedTestCtx m;
    YmStatus ym;
    int rc;
    u64 elapsed_us;
    int avg_kbps = 0;
    int peak_kbps = 0;

    (void)args;
    (void)argp;

    token[0] = '\0';
    track_id[0] = '\0';
    memset(&variant, 0, sizeof(variant));

    if (!net_stack_is_ready()) {
        logLine("speedtest: network not ready\n");
        s_state = ST_FAILED;
        goto out;
    }
    if (token_loader_read(token, sizeof(token)) != 0 || token[0] == '\0') {
        logLine("speedtest: no token\n");
        s_state = ST_FAILED;
        goto out;
    }
    if (speed_test_pick_track(track_id, sizeof(track_id)) != 0) {
        logLine("speedtest: no track id (need current or cached track)\n");
        s_state = ST_NO_TRACK;
        goto out;
    }

    ctx.oauth_token = token;
    ctx.timeout_ms = 0;
    ym = ym_api_download_get_aac_raw(&ctx, track_id, &variant);
    if (ym != YM_OK) {
        logLine("speedtest: url fetch failed track_id='%s' status=%d\n",
                track_id, ym);
        s_state = ST_FAILED;
        goto out;
    }
    snprintf(url, sizeof(url), "%s", variant.url);

    logLine("speedtest: start track_id='%s' bitrate=%d duration=%d ms\n",
            track_id, variant.bitrate_kbps, SPEED_TEST_DURATION_MS);
    logger_flush();

    memset(&m, 0, sizeof(m));
    m.start_us = sceKernelGetSystemTimeWide();
    m.window_us = m.start_us;
    rc = http_stream(&(HttpRequest){ .method = HTTP_GET,
                                     .url = url,
                                     .token = token,
                                     .identity_encoding = 1 },
                     &(HttpSink){ .on_chunk = speed_test_chunk,
                                  .user = &m });
    elapsed_us = sceKernelGetSystemTimeWide() - m.start_us;
    memset(token, 0, sizeof(token));

    if (rc != 0 && rc != NET_HTTP_STREAM_CANCELLED) {
        logLine("speedtest: stream failed rc=%d bytes=%d\n",
                rc, m.total_bytes);
        s_state = ST_FAILED;
        goto out;
    }
    if (elapsed_us < 1000ULL || m.total_bytes <= 0) {
        logLine("speedtest: no data rc=%d\n", rc);
        s_state = ST_FAILED;
        goto out;
    }

    avg_kbps = (int)((int64_t)m.total_bytes * 1000000LL /
                     (int64_t)elapsed_us / 1024LL);
    peak_kbps = m.peak_bps / 1024;
    snprintf(s_result_text, sizeof(s_result_text), "%d/%d KB/s",
             avg_kbps, peak_kbps);
    logLine("speedtest: done avg=%d KB/s peak=%d KB/s bytes=%d ms=%d rc=%d\n",
            avg_kbps, peak_kbps, m.total_bytes,
            (int)(elapsed_us / 1000ULL), rc);
    s_state = ST_DONE;

out:
    memset(token, 0, sizeof(token));
    logger_flush();
    sceKernelExitDeleteThread(0);
    return 0;
}

void speed_test_request(void)
{
    SceUID tid;

    if (s_state == ST_RUNNING) return;
    s_state = ST_RUNNING;
    tid = sceKernelCreateThread("speedtest", speed_test_worker,
                                SPEED_TEST_TICKET_PRIO, SPEED_TEST_STACK,
                                0, NULL);
    if (tid < 0) {
        logLine("speedtest: thread create failed 0x%08X\n", (unsigned)tid);
        s_state = ST_FAILED;
        return;
    }
    if (sceKernelStartThread(tid, 0, NULL) < 0) {
        logLine("speedtest: thread start failed\n");
        sceKernelDeleteThread(tid);
        s_state = ST_FAILED;
        return;
    }
    logLine("speedtest: requested\n");
    logger_flush();
}

void speed_test_status(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    switch (s_state) {
    case ST_RUNNING:
        snprintf(out, out_size, "%s", locale_get(LOCALE_SPEED_TEST_RUNNING));
        break;
    case ST_DONE:
        snprintf(out, out_size, "%s",
                 s_result_text[0] ? s_result_text
                                  : locale_get(LOCALE_SPEED_TEST_RUNNING));
        break;
    case ST_NO_TRACK:
        snprintf(out, out_size, "%s", locale_get(LOCALE_SPEED_TEST_NO_TRACK));
        break;
    case ST_FAILED:
        snprintf(out, out_size, "%s", locale_get(LOCALE_SPEED_TEST_FAILED));
        break;
    default:
        snprintf(out, out_size, "%s", locale_get(LOCALE_SPEED_TEST_RUN));
        break;
    }
}
