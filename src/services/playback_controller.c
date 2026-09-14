#include "services/playback_controller.h"

#include <pspkernel.h>
#include <string.h>

#include "core/logger.h"
#include "services/audio_cache.h"
#include "services/audio_player.h"
#include "services/cover_now_playing.h"
#include "services/playback_queue.h"
#include "services/token_loader.h"
#include "services/track_hydrator.h"
#include "services/net_stack.h"

/* The single global playback state instance. */
PlaybackState g_playback;

/* Token buffer — loaded on UI thread, cleared after handoff to audio_cache. */
static char s_token[256];
static int s_play_current_requested = 0;
static int s_pause_toggle_requested = 0;
static int s_stop_playback_requested = 0;
/* -1 previous, +1 next, 0 no navigation intent. Kept separate from the
 * queue cursor so a failed transition cannot move the visible queue. */
static int s_navigation_requested = 0;
static int s_prefetch_triggered = 0;
static int s_cover_prefetch_triggered = 0;
static int s_advance_triggered = 0;
/* Реакция на AUDIO_PLAYER_ERROR: не более N автопропусков подряд,
 * чтобы каскад ошибок не перебрал молча всю очередь до конца. */
#define PB_MAX_CONSECUTIVE_ERROR_SKIPS 3
static int s_error_skips = 0;
static int s_error_handled = 0;
static int s_network_retry_pending = 0;
static u64 s_network_retry_after_us = 0;
static u64 s_network_retry_backoff_us = 500000ULL;

/* Queue metadata resolution can come back PENDING (id known, entry not in the
 * store yet — deep shuffle jump or evicted record). The controller then asks
 * the hydrator and retries on a slow tick instead of polling the Memory Stick
 * every frame. */
#define PB_PENDING_RETRY_US 250000ULL
static u64 s_play_retry_after_us = 0;
static u64 s_prefetch_retry_after_us = 0;

static u64 pb_now_us(void)
{
    return (u64)sceKernelGetSystemTimeWide();
}

static void pb_request_track_hydration(const char *track_id)
{
    static char s_last_requested[40];
    static u64 s_last_requested_us = 0;
    u64 now = pb_now_us();
    char token[256];

    /* One request per id, re-issued at most every 3 s so a failed hydration
     * can never wedge playback permanently. */
    if (strcmp(s_last_requested, track_id) == 0 &&
        (now - s_last_requested_us) < 3000000ULL) {
        return;
    }
    token[0] = '\0';
    if (token_loader_read(token, sizeof(token)) != 0) {
        return;
    }
    track_hydrator_request_track(token, track_id, -1, 0);
    memset(token, 0, sizeof(token));
    strncpy(s_last_requested, track_id, sizeof(s_last_requested) - 1);
    s_last_requested[sizeof(s_last_requested) - 1] = '\0';
    s_last_requested_us = now;
    logLine("pb: hydration requested track_id='%s'\n", track_id);
}

static PlaybackIntentResult playback_controller_start_track(const TrackEntry *track)
{
    if (!track || track->id[0] == '\0') {
        logLine("pb: reject invalid track\n");
        return PLAYBACK_INTENT_REJECTED;
    }

    /* Early-exit if same track already in progress — avoids token disk read. */
    {
        AudioCacheStatus cur;
        audio_cache_get_status(&cur);
        if ((cur.state == AUDIO_CACHE_DOWNLOADING ||
             cur.state == AUDIO_CACHE_PROGRESSIVE_READY ||
             cur.state == AUDIO_CACHE_READY) &&
            strcmp(cur.track_id, track->id) == 0) {
            logLine("pb: same track already %s, skip\n",
                    cur.state == AUDIO_CACHE_READY ? "READY" :
                    (cur.state == AUDIO_CACHE_PROGRESSIVE_READY ? "PROGRESSIVE_READY" : "DOWNLOADING"));
            memcpy(&g_playback.current_track, track, sizeof(TrackEntry));
            if (audio_player_start_current() == 0) {
                return PLAYBACK_INTENT_ACCEPTED;
            }
            logLine("pb: reject same-track start_current failed track_id='%s'\n", track->id);
            return PLAYBACK_INTENT_REJECTED;
        }
    }

    s_token[0] = '\0';
    if (token_loader_read(s_token, sizeof(s_token)) != 0) {
        logLine("pb: token read failed\n");
        return PLAYBACK_INTENT_REJECTED;
    }

    audio_player_stop();
    memcpy(&g_playback.current_track, track, sizeof(TrackEntry));
    if (audio_cache_start(track, s_token) != 0) {
        /* Кэш отказал (source ещё удержан, воркер недоступен) — плеер не
         * стартуем, иначе он повиснет в OPENING на статусе чужого трека. */
        logLine("pb: reject cache_start failed track_id='%s'\n", track->id);
        memset(s_token, 0, sizeof(s_token));
        return PLAYBACK_INTENT_REJECTED;
    }
    if (audio_player_start_current() != 0) {
        logLine("pb: reject start_current failed track_id='%s'\n", track->id);
        memset(s_token, 0, sizeof(s_token));
        return PLAYBACK_INTENT_REJECTED;
    }
    memset(s_token, 0, sizeof(s_token));
    return PLAYBACK_INTENT_ACCEPTED;
}

void playback_controller_init(void)
{
    memset(&g_playback, 0, sizeof(g_playback));
    s_play_current_requested = 0;
    s_pause_toggle_requested = 0;
    s_stop_playback_requested = 0;
    s_navigation_requested = 0;
    s_prefetch_triggered = 0;
    s_cover_prefetch_triggered = 0;
    s_advance_triggered = 0;
    s_error_skips = 0;
    s_error_handled = 0;
    s_network_retry_pending = 0;
    s_network_retry_after_us = 0;
    s_network_retry_backoff_us = 500000ULL;
    playback_queue_init();
    audio_player_init();
    logLine("pb: controller init\n");
}

void playback_controller_shutdown(void)
{
    s_play_current_requested = 0;
    s_pause_toggle_requested = 0;
    s_stop_playback_requested = 0;
    s_navigation_requested = 0;
    s_prefetch_triggered = 0;
    s_cover_prefetch_triggered = 0;
    s_advance_triggered = 0;
    s_network_retry_pending = 0;
    audio_player_shutdown();
    playback_queue_clear();
    memset(&g_playback, 0, sizeof(g_playback));
    logLine("pb: controller shutdown\n");
}

int playback_controller_quiesce(void)
{
    s_play_current_requested = 0;
    s_pause_toggle_requested = 0;
    s_stop_playback_requested = 0;
    s_navigation_requested = 0;
    s_prefetch_triggered = 0;
    s_cover_prefetch_triggered = 0;
    s_advance_triggered = 0;
    s_network_retry_pending = 0;
    return audio_player_quiesce();
}

void playback_controller_request_play_current(void)
{
    s_play_current_requested = 1;
    s_navigation_requested = 0;
    logLine("pb: play_current requested\n");
}

static void playback_controller_request_navigation(int direction)
{
    s_navigation_requested = direction;
    s_play_current_requested = 0;
    s_play_retry_after_us = 0;
    s_network_retry_pending = 0;
    s_advance_triggered = 0;
    s_error_handled = 0;
    logLine("pb: %s requested\n", direction > 0 ? "next" : "previous");
}

void playback_controller_request_next(void)
{
    playback_controller_request_navigation(1);
}

void playback_controller_request_previous(void)
{
    playback_controller_request_navigation(-1);
}

void playback_controller_request_toggle_pause(void)
{
    s_pause_toggle_requested = 1;
    logLine("pb: play/pause requested\n");
}

void playback_controller_request_stop(void)
{
    s_stop_playback_requested = 1;
    s_pause_toggle_requested = 0;
    logLine("pb: stop requested\n");
}

static void playback_controller_service_navigation(void)
{
    TrackEntry target;
    int direction = s_navigation_requested;
    int rc;

    if (direction == 0 || pb_now_us() < s_play_retry_after_us) {
        return;
    }

    rc = direction > 0
        ? playback_queue_get_next(&target)
        : playback_queue_get_previous(&target);

    if (rc == PLAYBACK_QUEUE_EMPTY) {
        logLine("pb: %s rejected at queue boundary\n",
                direction > 0 ? "next" : "previous");
        s_navigation_requested = 0;
        return;
    }
    if (rc == PLAYBACK_QUEUE_PENDING) {
        pb_request_track_hydration(target.id);
        s_play_retry_after_us = pb_now_us() + PB_PENDING_RETRY_US;
        logLine("pb: %s waiting metadata track_id='%s'\n",
                direction > 0 ? "next" : "previous", target.id);
        return;
    }

    s_play_retry_after_us = 0;
    if (playback_controller_start_track(&target) != PLAYBACK_INTENT_ACCEPTED) {
        /* The cursor remains unchanged. Do not spin on a permanent failure
         * (missing token, cache refusal); another key press is a fresh intent. */
        s_navigation_requested = 0;
        logLine("pb: %s start rejected; queue unchanged track_id='%s'\n",
                direction > 0 ? "next" : "previous", target.id);
        return;
    }

    rc = direction > 0
        ? playback_queue_move_next()
        : playback_queue_move_previous();
    if (rc != 0) {
        logLine("pb: %s queue commit failed track_id='%s'\n",
                direction > 0 ? "next" : "previous", target.id);
        s_navigation_requested = 0;
        return;
    }

    s_navigation_requested = 0;
    s_prefetch_triggered = 0;
    s_cover_prefetch_triggered = 0;
    s_prefetch_retry_after_us = 0;
    s_advance_triggered = 0;
    s_error_skips = 0;
    s_network_retry_backoff_us = 500000ULL;
    logLine("pb: manual %s committed track_id='%s'\n",
            direction > 0 ? "next" : "previous", target.id);
}

void playback_controller_service(void)
{
    if (s_stop_playback_requested) {
        s_stop_playback_requested = 0;
        s_play_current_requested = 0;
        s_navigation_requested = 0;
        s_network_retry_pending = 0;
        s_advance_triggered = 0;
        audio_player_stop();
    }

    if (s_pause_toggle_requested) {
        AudioPlayerState state = audio_player_get_state();
        s_pause_toggle_requested = 0;
        if (state == AUDIO_PLAYER_PAUSED) {
            audio_player_resume();
        } else if (state == AUDIO_PLAYER_OPENING ||
                   state == AUDIO_PLAYER_PLAYING ||
                   state == AUDIO_PLAYER_BUFFERING) {
            audio_player_pause();
        } else if (g_playback.current_track.id[0] != '\0') {
            s_play_current_requested = 1;
            s_play_retry_after_us = 0;
        }
    }

    /* Manual navigation has priority over retries and automatic advance. */
    playback_controller_service_navigation();

    if (s_network_retry_pending &&
        pb_now_us() >= s_network_retry_after_us) {
        if (net_stack_is_ready()) {
            PlaybackIntentResult result =
                playback_controller_start_track(&g_playback.current_track);
            if (result == PLAYBACK_INTENT_ACCEPTED) {
                logLine("pb: network recovered, retry track_id='%s'\n",
                        g_playback.current_track.id);
                s_network_retry_pending = 0;
                s_error_handled = 0;
            } else {
                s_network_retry_after_us =
                    pb_now_us() + s_network_retry_backoff_us;
            }
        } else {
            s_network_retry_after_us = pb_now_us() + 500000ULL;
        }
    }

    if (s_play_current_requested &&
        pb_now_us() >= s_play_retry_after_us) {
        PlaybackIntentResult result;
        s_play_current_requested = 0;
        result = playback_controller_play_current();
        if (result != PLAYBACK_INTENT_ACCEPTED) {
            logLine("pb: deferred play_current rejected\n");
        }
    }

    if (!s_prefetch_triggered && net_stack_is_ready() &&
        pb_now_us() >= s_prefetch_retry_after_us) {
        AudioPlayerSnapshot snap;
        audio_player_get_snapshot(&snap);
        if (snap.state == AUDIO_PLAYER_PLAYING &&
            snap.duration_ms > 30000 &&
            snap.position_ms >= snap.duration_ms - 30000) {
            TrackEntry next;
            int rc = playback_queue_get_next(&next);
            if (rc == PLAYBACK_QUEUE_OK) {
                char token[256];
                token[0] = '\0';
                if (token_loader_read(token, sizeof(token)) == 0) {
                    audio_cache_prefetch_start(&next, token);
                    memset(token, 0, sizeof(token));
                    s_prefetch_triggered = 1;
                    s_cover_prefetch_triggered = 0;
                    logLine("pb: prefetch triggered track_id='%s'\n", next.id);
                }
            } else if (rc == PLAYBACK_QUEUE_PENDING) {
                pb_request_track_hydration(next.id);
                s_prefetch_retry_after_us =
                    pb_now_us() + PB_PENDING_RETRY_US;
            }
        }
    }

    if (s_prefetch_triggered && !s_cover_prefetch_triggered &&
        audio_cache_prefetch_get_state() == AUDIO_CACHE_READY) {
        TrackEntry next;
        if (playback_queue_get_next(&next) == PLAYBACK_QUEUE_OK) {
            if (next.album_id != 0 && next.cover_uri[0]) {
                cover_now_playing_prefetch(next.album_id, next.cover_uri);
                logLine("pb: cover prefetch triggered track_id='%s' album_id=%d\n",
                        next.id, next.album_id);
            }
            s_cover_prefetch_triggered = 1;
        }
    }

    if (!s_advance_triggered &&
        audio_player_get_state() == AUDIO_PLAYER_FINISHED &&
        g_playback.current_track.id[0] != '\0') {
        s_advance_triggered = 1;
        s_error_skips = 0;  /* трек доигран штатно — серия ошибок прервана */
        if (playback_queue_move_next() == 0) {
            TrackEntry next;
            int rc = playback_queue_get_current(&next);
            if (rc == PLAYBACK_QUEUE_PENDING) {
                /* Queue already moved; start via the deferred-play path once
                 * the metadata lands. play_current resets s_advance_triggered. */
                pb_request_track_hydration(next.id);
                s_play_retry_after_us =
                    pb_now_us() + PB_PENDING_RETRY_US;
                s_play_current_requested = 1;
                s_prefetch_triggered = 0;
                s_cover_prefetch_triggered = 0;
                logLine("pb: advance waiting metadata track_id='%s'\n", next.id);
            } else if (s_prefetch_triggered && audio_cache_prefetch_swap() == 0) {
                memcpy(&g_playback.current_track, &next, sizeof(next));
                audio_player_start_current();
                logLine("pb: advance from prefetch track_id='%s'\n", next.id);
                s_prefetch_triggered = 0;
                s_cover_prefetch_triggered = 0;
                s_advance_triggered = 0;
            } else {
                playback_controller_start_track(&next);
                logLine("pb: advance cold start track_id='%s'\n", next.id);
                s_prefetch_triggered = 0;
                s_cover_prefetch_triggered = 0;
                s_advance_triggered = 0;
            }
        } else {
            logLine("pb: queue end\n");
            s_prefetch_triggered = 0;
            s_cover_prefetch_triggered = 0;
        }
    }

    /* Ошибка плеера (кэш умер, watchdog, отказ бэкенда): один автопропуск
     * на ошибку, не более PB_MAX_CONSECUTIVE_ERROR_SKIPS подряд — дальше
     * останавливаемся в ERROR и ждём действий пользователя. */
    {
        AudioPlayerState st = audio_player_get_state();
        if (st == AUDIO_PLAYER_ERROR &&
            !s_error_handled &&
            g_playback.current_track.id[0] != '\0') {
            s_error_handled = 1;
            if (audio_cache_error_is_network() || !net_stack_is_ready()) {
                s_network_retry_pending = 1;
                s_network_retry_after_us =
                    pb_now_us() + s_network_retry_backoff_us;
                if (s_network_retry_backoff_us < 10000000ULL) {
                    s_network_retry_backoff_us *= 2;
                    if (s_network_retry_backoff_us > 10000000ULL)
                        s_network_retry_backoff_us = 10000000ULL;
                }
                logLine("pb: network error; hold queue and retry track_id='%s'\n",
                        g_playback.current_track.id);
            } else {
                s_error_skips++;
                if (s_error_skips <= PB_MAX_CONSECUTIVE_ERROR_SKIPS &&
                    playback_queue_move_next() == 0) {
                    TrackEntry next;
                    int rc = playback_queue_get_current(&next);
                    logLine("pb: error skip %d/%d -> track_id='%s'\n",
                            s_error_skips, PB_MAX_CONSECUTIVE_ERROR_SKIPS, next.id);
                    s_prefetch_triggered = 0;
                    s_cover_prefetch_triggered = 0;
                    if (rc == PLAYBACK_QUEUE_PENDING) {
                        pb_request_track_hydration(next.id);
                        s_play_retry_after_us =
                            pb_now_us() + PB_PENDING_RETRY_US;
                        s_play_current_requested = 1;
                    } else {
                        playback_controller_start_track(&next);
                    }
                } else {
                    logLine("pb: stopping after %d consecutive errors\n", s_error_skips);
                }
            }
        }
        if (st != AUDIO_PLAYER_ERROR) {
            s_error_handled = 0;
        }
        if (st == AUDIO_PLAYER_PLAYING) {
            s_error_skips = 0;
            s_network_retry_backoff_us = 500000ULL;
        }
    }
}

PlaybackIntentResult playback_controller_play_current(void)
{
    TrackEntry track;
    PlaybackQueueInfo info;

    s_prefetch_triggered = 0;
    s_cover_prefetch_triggered = 0;
    s_advance_triggered = 0;
    s_error_skips = 0;   /* ручной запуск начинает серию заново */
    s_error_handled = 0;
    s_network_retry_pending = 0;
    s_network_retry_backoff_us = 500000ULL;

    {
        int rc = playback_queue_get_current(&track);
        if (rc == PLAYBACK_QUEUE_EMPTY) {
            logLine("pb: play_current failed, queue empty\n");
            return PLAYBACK_INTENT_REJECTED;
        }
        if (rc == PLAYBACK_QUEUE_PENDING) {
            /* Id known, metadata not in the store yet: hydrate and retry from
             * service() on a slow tick. */
            pb_request_track_hydration(track.id);
            s_play_retry_after_us =
                pb_now_us() + PB_PENDING_RETRY_US;
            s_play_current_requested = 1;
            logLine("pb: play_current waiting metadata track_id='%s'\n", track.id);
            return PLAYBACK_INTENT_ACCEPTED;
        }
        s_play_retry_after_us = 0;
    }

    if (playback_queue_get_info(&info) == 0) {
        logLine("pb: play_current queue current=%d count=%d source=%d source_id=%d generation=%d track_id='%s'\n",
                info.current_index,
                info.count,
                (int)info.source,
                info.source_id,
                info.source_generation,
                track.id);
    } else {
        logLine("pb: play_current queue info unavailable track_id='%s'\n", track.id);
    }

    return playback_controller_start_track(&track);
}

PlaybackStatus playback_controller_get_status(void)
{
    if (audio_player_get_state() == AUDIO_PLAYER_ERROR) {
        return PB_ERROR;
    }
    switch (audio_cache_get_state()) {
        case AUDIO_CACHE_DOWNLOADING: return PB_BUFFERING;
        case AUDIO_CACHE_PROGRESSIVE_READY: return PB_READY;
        case AUDIO_CACHE_READY:       return PB_READY;
        case AUDIO_CACHE_ERROR:       return PB_ERROR;
        default:                      return PB_IDLE;
    }
}
