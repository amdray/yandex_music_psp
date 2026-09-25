#include "services/track_like.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/token_loader.h"
#include "services/ym_api_like.h"

typedef struct LikeJob {
    char token[256];
    char track_id[40];
    int uid;
    int target;
    int result;
} LikeJob;

static SceUID s_tid = -1;
static LikeJob s_job;
static char s_marked_track[40];
static int s_marked_on;

static int like_worker(SceSize args, void *argp)
{
    YmApiContext ctx;

    (void)args;
    (void)argp;
    ctx.oauth_token = s_job.token;
    ctx.timeout_ms = 0;
    logLine("like: post id='%s' like=%d\n", s_job.track_id, s_job.target);
    s_job.result = ym_api_track_like(&ctx, s_job.uid, s_job.track_id,
                                     s_job.target);
    if (s_job.result == 0) {
        logLine("like: ok id='%s' like=%d\n", s_job.track_id, s_job.target);
    } else {
        logLine("like: failed id='%s' like=%d\n", s_job.track_id, s_job.target);
    }
    logger_flush();
    memset(s_job.token, 0, sizeof(s_job.token));
    return 0;
}

void track_like_poll(void)
{
    SceKernelThreadRunStatus status;

    if (s_tid < 0) {
        return;
    }
    status.size = sizeof(status);
    if (sceKernelReferThreadRunStatus(s_tid, &status) != 0 ||
        status.status != PSP_THREAD_STOPPED) {
        return;
    }
    if (s_job.result == 0) {
        snprintf(s_marked_track, sizeof(s_marked_track), "%s", s_job.track_id);
        s_marked_on = s_job.target;
    }
    sceKernelDeleteThread(s_tid);
    s_tid = -1;
    memset(&s_job, 0, sizeof(s_job));
}

int track_like_is_on(const char *track_id)
{
    return track_id && track_id[0] && s_marked_on &&
           strcmp(s_marked_track, track_id) == 0;
}

void track_like_request_toggle(int uid, const char *track_id)
{
    if (!track_id || !track_id[0] || uid <= 0) {
        return;
    }
    track_like_poll();
    if (s_tid >= 0) {
        logLine("like: busy, ignored\n");
        return;
    }
    if (strlen(track_id) >= sizeof(s_job.track_id)) {
        logLine("like: track id too long\n");
        return;
    }
    if (token_loader_read(s_job.token, sizeof(s_job.token)) != 0) {
        logLine("like: no token\n");
        memset(&s_job, 0, sizeof(s_job));
        return;
    }
    s_job.uid = uid;
    snprintf(s_job.track_id, sizeof(s_job.track_id), "%s", track_id);
    s_job.target = track_like_is_on(track_id) ? 0 : 1;
    s_job.result = -1;
    s_tid = sceKernelCreateThread("like_worker", like_worker,
                                  0x18, 32 * 1024, 0, NULL);
    if (s_tid < 0) {
        logLine("like: create thread failed 0x%08X\n", s_tid);
        memset(&s_job, 0, sizeof(s_job));
        return;
    }
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        logLine("like: start thread failed\n");
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        memset(&s_job, 0, sizeof(s_job));
    }
}
