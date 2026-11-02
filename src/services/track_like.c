#include "services/track_like.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/net_tls.h"
#include "services/token_loader.h"

/* The worker exclusively owns s_library while s_tid >= 0. The UI reads
 * published state only after STOPPED and never reads a changing collection. */
typedef struct {
    char token[256]; /* Same token contract as token_loader. */
    char track_id[TRACK_ID_SIZE];
    int uid;
    unsigned int generation;
    int mutate;
    YmReactionAction action;
    int result;
} LikeJob;

static SceUID s_tid = -1;
static LikeJob s_job;
static YmReactions s_library;
static int s_uid;
static unsigned int s_generation;
static char s_track_id[TRACK_ID_SIZE];
static YmReaction s_visible = YM_REACTION_UNKNOWN;
static int s_sync_pending;
static int s_sync_error;
static int s_action_error;
static int s_stopping;
static volatile int s_cancel;

static int like_worker(SceSize args, void *argp)
{
    YmApiContext ctx;
    (void)args;
    (void)argp;
    ctx.oauth_token = s_job.token;
    ctx.timeout_ms = 0;
    net_tls_cancel_bind(&s_cancel);
    if (s_job.mutate)
        s_job.result = ym_api_reaction_set(&ctx, s_job.uid, &s_library,
                                           s_job.track_id, s_job.action);
    else
        s_job.result = ym_api_reactions_sync(&ctx, &s_library);
    net_tls_cancel_unbind();
    memset(s_job.token, 0, sizeof(s_job.token));
    return 0;
}

static void refresh_visible(void)
{
    s_visible = ym_reactions_get(&s_library, s_track_id);
}

static void poll_worker(void)
{
    SceKernelThreadRunStatus status;
    if (s_tid < 0) return;
    status.size = sizeof(status);
    if (sceKernelReferThreadRunStatus(s_tid, &status) != 0 ||
        status.status != PSP_THREAD_STOPPED) return;
    /* Do not reuse the job storage unless the previous thread is gone. */
    if (sceKernelDeleteThread(s_tid) < 0) return;
    s_tid = -1;
    if (s_job.generation != s_generation) {
        ym_reactions_free(&s_library);
    } else if (s_job.mutate) {
        if (strcmp(s_job.track_id, s_track_id) == 0)
            s_action_error = s_job.result;
        /* Also reconcile an ambiguous failure: the server may have committed
         * a mutation before the connection/response failed. No mutation retry. */
        s_sync_pending = 1;
    } else {
        s_sync_error = s_job.result;
    }
    memset(&s_job, 0, sizeof(s_job));
    refresh_visible();
}

static int start_job(int mutate)
{
    memset(&s_job, 0, sizeof(s_job));
    if (token_loader_read(s_job.token, sizeof(s_job.token)) != 0)
        return YM_ERR_AUTH;
    s_job.uid = s_uid;
    s_job.generation = s_generation;
    s_job.mutate = mutate;
    strcpy(s_job.track_id, s_track_id);
    s_job.action = s_visible == YM_REACTION_LIKE
        ? YM_REACTION_REMOVE_LIKE : YM_REACTION_ADD_LIKE;
    s_job.result = YM_ERR_HTTP;
    s_cancel = 0;
    /* В ответах data/research/collection-sync-20261029-101300 максимум
     * 7 уровней вложенности: до 8 кадров parse_value по 120 байт
     * в проверенной сборке. Эти 960 байт учитывают только рекурсивные
     * кадры парсера, а не полную цепочку вызовов потока. */
    s_tid = sceKernelCreateThread("like_worker", like_worker,
                                  0x18, 32 * 1024, 0, NULL);
    if (s_tid < 0) goto fail;
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        goto fail;
    }
    return YM_OK;
fail:
    memset(s_job.token, 0, sizeof(s_job.token));
    return YM_ERR_HTTP;
}

void track_like_update(int uid, const char *track_id)
{
    if (s_stopping) return;
    if (!track_id) track_id = "";
    if (uid != s_uid) {
        s_cancel = 1;
        s_generation++;
        s_uid = uid;
        s_visible = YM_REACTION_UNKNOWN;
        s_sync_error = s_action_error = 0;
        s_sync_pending = uid > 0;
        if (s_tid < 0) ym_reactions_free(&s_library);
    }
    if (strcmp(track_id, s_track_id) != 0) {
        s_action_error = 0;
        if (strlen(track_id) >= sizeof(s_track_id)) {
            s_track_id[0] = '\0';
            s_visible = YM_REACTION_UNKNOWN;
            s_sync_error = YM_ERR_PARSE;
            return;
        }
        strcpy(s_track_id, track_id);
        s_visible = YM_REACTION_UNKNOWN;
        if (s_tid < 0) refresh_visible();
        s_sync_pending = uid > 0;
    }
    poll_worker();
    if (s_tid < 0 && s_uid > 0 && s_sync_pending) {
        s_sync_pending = 0;
        s_sync_error = start_job(0);
    }
}

YmReaction track_like_state(const char *track_id)
{
    return track_id && strcmp(track_id, s_track_id) == 0
        ? s_visible : YM_REACTION_UNKNOWN;
}

int track_like_is_on(const char *track_id)
{
    return track_like_state(track_id) == YM_REACTION_LIKE;
}

int track_like_error(const char *track_id)
{
    if (!track_id || strcmp(track_id, s_track_id) != 0) return 0;
    return s_action_error ? s_action_error : s_sync_error;
}

void track_like_request_toggle(int uid, const char *track_id)
{
    track_like_update(uid, track_id);
    if (s_stopping || uid <= 0 || !track_id || !track_id[0] || s_tid >= 0) return;
    s_action_error = 0;
    if (s_visible == YM_REACTION_UNKNOWN) {
        s_sync_pending = 1;
        return; /* Unknown is not equivalent to unliked. Retry the read first. */
    }
    s_action_error = start_job(1);
}

int track_like_quiesce(void)
{
    s_stopping = 1;
    s_cancel = 1;
    if (s_tid >= 0) {
        /* Same bounded join as net_client_playlist_load_quiesce(). */
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_tid, &timeout_us) < 0) {
            logLine("reactions: worker still active; resources retained\n");
            return -1;
        }
    }
    return 0;
}
