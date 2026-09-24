#include "services/playback_controller.h"

#include <pspkernel.h>
#include <string.h>

#include "core/logger.h"
#include "services/audio_cache.h"
#include "services/audio_player.h"
#include "services/cover_now_playing.h"
#include "services/last_play.h"
#include "services/playback_queue.h"
#include "services/playback_reporter.h"
#include "services/token_loader.h"
#include "services/track_hydrator.h"
#include "services/net_stack.h"
#include "services/wave.h"

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

/* WAVE-REPORT: playback telemetry context (spec sections 5-8). One stashed
 * context per started track; audio FIRST_PCM/terminal events become
 * reporter FIFO entries. Rotor feedback applies to FLOW only; /play-audio
 * is queued for every track with really-audible output. */
static PlaybackReportContext s_report_ctx;
static unsigned int s_report_seq = 0;

static int wave_report_find_flow(const char *track_id, WaveQueueItem *out)
{
    WaveQueueItem batch[64];
    int batch_count;
    int batch_index;

    if (!track_id || !track_id[0] || !out) {
        return -1;
    }
    batch_count = playback_queue_get_flow_items(batch, 64);
    for (batch_index = 0; batch_index < batch_count; ++batch_index) {
        if (strcmp(batch[batch_index].track_id, track_id) == 0) {
            *out = batch[batch_index];
            return 0;
        }
    }
    return -1;
}

static void wave_report_capture(const TrackEntry *track)
{
    PlaybackQueueInfo queue_info;
    PlaybackQueueSource source = PLAYBACK_QUEUE_SOURCE_NONE;
    WaveQueueItem flow;
    int have_flow = 0;
    unsigned int generation;

    if (!track || !track->id[0]) {
        return;
    }
    /* Same-track restart (pause/buffering/network retry): keep play_id. */
    if (strcmp(s_report_ctx.track_id, track->id) == 0 &&
        !s_report_ctx.terminal_reported) {
        return;
    }
    if (playback_queue_get_info(&queue_info) == 0) {
        source = queue_info.source;
    }
    if (source == PLAYBACK_QUEUE_SOURCE_FLOW) {
        generation = wave_current_generation();
        if (wave_report_find_flow(track->id, &flow) == 0) {
            have_flow = 1;
        }
    } else {
        s_report_seq++;
        if (s_report_seq == 0) {
            s_report_seq = 1;
        }
        generation = s_report_seq;
    }
    playback_report_context_start(&s_report_ctx,
                                  track->id,
                                  have_flow ? flow.album_id : track->album_id,
                                  have_flow ? flow.batch_id : "",
                                  track->duration_ms,
                                  generation, source);
    if (playback_reporter_new_play_id(s_report_ctx.play_id,
                                      sizeof(s_report_ctx.play_id)) != 0) {
        s_report_ctx.play_id[0] = '\0';
    }
    audio_player_set_report_generation(s_report_ctx.generation);
    logLine("pb: report ctx track='%s' flow=%d play='%s'\n",
            s_report_ctx.track_id, have_flow, s_report_ctx.play_id);
}

static void wave_report_poll_first_pcm(void)
{
    char pcm_id[40];
    unsigned int pcm_generation = 0;
    char token[256];
    char session_id[64];

    if (audio_player_take_first_pcm_event(pcm_id, sizeof(pcm_id),
                                          &pcm_generation) != 0) {
        return;
    }
    if (!s_report_ctx.track_id[0] ||
        strcmp(s_report_ctx.track_id, pcm_id) != 0 ||
        s_report_ctx.generation != pcm_generation ||
        s_report_ctx.first_pcm_reported ||
        !s_report_ctx.play_id[0]) {
        return;
    }
    s_report_ctx.first_pcm_reported = 1;
    token[0] = '\0';
    if (token_loader_read(token, sizeof(token)) != 0 || !token[0]) {
        logLine("pb: report start skipped, no token\n");
        return;
    }
    /* /play-audio for every audible track; trackStarted for FLOW only. */
    playback_reporter_enqueue_play_audio(&s_report_ctx, token, 0);
    if (s_report_ctx.active &&
        s_report_ctx.source == PLAYBACK_QUEUE_SOURCE_FLOW &&
        wave_get_session_id(session_id, sizeof(session_id)) == 0) {
        double len_s = (s_report_ctx.duration_ms > 0)
            ? ((double)s_report_ctx.duration_ms / 1000.0) : 0.0;
        playback_reporter_enqueue_feedback(PLAYBACK_REPORT_TRACK_STARTED,
                                           &s_report_ctx, token, session_id,
                                           0.0, len_s);
    }
    memset(token, 0, sizeof(token));
    logLine("pb: report start queued track='%s'\n", s_report_ctx.track_id);
    logger_flush();
}

/* Resolve really-audible ms for a stashed context: prefer the worker's
 * terminal snapshot on track+generation match, else the last value the
 * controller saw. *out_had_pcm gates event creation (spec 8.5: nothing
 * before the first PCM). */
static void wave_report_terminal_time(const PlaybackReportContext *old,
                                      int *out_audible_ms, int *out_had_pcm)
{
    char snap_id[40];
    unsigned int snap_generation = 0;
    AudioEndReason snap_reason = AUDIO_END_MANUAL_STOP;
    int snap_audible = 0;
    int snap_had_pcm = 0;

    if (out_audible_ms) {
        *out_audible_ms = old ? old->audible_ms : 0;
    }
    if (out_had_pcm) {
        *out_had_pcm = old ? old->first_pcm_reported : 0;
    }
    if (!old || !old->track_id[0]) {
        return;
    }
    if (audio_player_take_terminal_snapshot(snap_id, sizeof(snap_id),
                                            &snap_generation, &snap_reason,
                                            &snap_audible,
                                            &snap_had_pcm) != 0) {
        return;
    }
    (void)snap_reason; /* the controller branch owns the stop reason */
    if (strcmp(snap_id, old->track_id) == 0 &&
        snap_generation == old->generation) {
        if (out_audible_ms) {
            *out_audible_ms = snap_audible;
        }
        if (out_had_pcm) {
            *out_had_pcm = snap_had_pcm;
        }
    }
}

static void wave_report_note_old(const PlaybackReportContext *old, int finished)
{
    int audible_ms = 0;
    int had_pcm = 0;

    if (!old || !old->track_id[0] || !old->active ||
        old->source != PLAYBACK_QUEUE_SOURCE_FLOW) {
        return;
    }
    if (old->first_pcm_reported == 0 || old->terminal_reported) {
        return;
    }
    wave_report_terminal_time(old, &audible_ms, &had_pcm);
    if (!had_pcm) {
        return;
    }
    if (finished) {
        wave_note_finished(old->track_id, old->album_id,
                           audible_ms, old->duration_ms);
    } else {
        wave_note_skip(old->track_id, old->album_id,
                       audible_ms, old->duration_ms);
    }
}
/* SEEK: accumulated relative-seek request (ms) from UI hold handling. */
static int s_seek_delta_ms = 0;
/* One-shot absolute seek bound to the restored track id. It remains pending
 * through asynchronous metadata hydration and is cleared only after the
 * audio worker accepts it or another playback intent supersedes it. */
static int s_resume_seek_pending = 0;
static int s_resume_position_ms = 0;
static char s_resume_track_id[40];
static int s_restored_paused = 0;
static int s_restored_position_ms = 0;
static char s_restored_track_id[40];

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

static void pb_request_track_hydration(const TrackEntry *track)
{
    static char s_last_requested[40];
    static int s_last_requested_album = 0;
    static u64 s_last_requested_us = 0;
    u64 now = pb_now_us();
    char token[256];

    /* One request per id, re-issued at most every 3 s so a failed hydration
     * can never wedge playback permanently. */
    if (!track || !track->id[0]) {
        return;
    }
    if (strcmp(s_last_requested, track->id) == 0 &&
        s_last_requested_album == track->album_id &&
        (now - s_last_requested_us) < 3000000ULL) {
        return;
    }
    token[0] = '\0';
    if (token_loader_read(token, sizeof(token)) != 0) {
        return;
    }
    track_hydrator_request_track(token, track->id, track->album_id, -1, 0);
    memset(token, 0, sizeof(token));
    strncpy(s_last_requested, track->id, sizeof(s_last_requested) - 1);
    s_last_requested[sizeof(s_last_requested) - 1] = '\0';
    s_last_requested_album = track->album_id;
    s_last_requested_us = now;
    logLine("pb: hydration requested track_id='%s' album_id=%d\n",
            track->id, track->album_id);
}

static void pb_clear_restored_pause(void)
{
    s_restored_paused = 0;
    s_restored_position_ms = 0;
    s_restored_track_id[0] = '\0';
}

static PlaybackIntentResult playback_controller_start_track(const TrackEntry *track)
{
    if (!track || track->id[0] == '\0') {
        logLine("pb: reject invalid track\n");
        return PLAYBACK_INTENT_REJECTED;
    }
    if (!track->available) {
        logLine("pb: reject unavailable track_id='%s'\n", track->id);
        return PLAYBACK_INTENT_REJECTED;
    }
    if (s_resume_seek_pending &&
        strcmp(s_resume_track_id, track->id) != 0) {
        s_resume_seek_pending = 0;
        s_resume_position_ms = 0;
        s_resume_track_id[0] = '\0';
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
            /* WAVE-REPORT: keep (or restart, when terminal) the report ctx. */
            wave_report_capture(track);
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

    /* WAVE-REPORT: stash the report context before the worker stops/starts
     * (generation must be set before audio_player_start_current below). */
    wave_report_capture(track);
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
    /* WAVE-REPORT: fresh telemetry state. */
    memset(&s_report_ctx, 0, sizeof(s_report_ctx));
    s_report_seq = 0;
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
    /* SEEK: no queued seek across init. */
    s_seek_delta_ms = 0;
    s_resume_seek_pending = 0;
    s_resume_position_ms = 0;
    s_resume_track_id[0] = '\0';
    pb_clear_restored_pause();
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
    s_resume_seek_pending = 0;
    s_resume_position_ms = 0;
    s_resume_track_id[0] = '\0';
    pb_clear_restored_pause();
    audio_player_shutdown();
    playback_queue_clear();
    memset(&g_playback, 0, sizeof(g_playback));
    /* WAVE-REPORT: drop stashed telemetry with the controller. */
    memset(&s_report_ctx, 0, sizeof(s_report_ctx));
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
    /* SEEK: drop any queued seek on quiesce. */
    s_seek_delta_ms = 0;
    s_resume_seek_pending = 0;
    s_resume_position_ms = 0;
    s_resume_track_id[0] = '\0';
    pb_clear_restored_pause();
    return audio_player_quiesce();
}

void playback_controller_request_play_current(void)
{
    pb_clear_restored_pause();
    s_resume_seek_pending = 0;
    s_resume_position_ms = 0;
    s_resume_track_id[0] = '\0';
    s_play_current_requested = 1;
    s_navigation_requested = 0;
    logLine("pb: play_current requested\n");
}

void playback_controller_request_resume_current(int position_ms)
{
    TrackEntry track;

    memset(&track, 0, sizeof(track));
    if (position_ms < 0) position_ms = 0;
    if (playback_queue_get_current(&track) == PLAYBACK_QUEUE_EMPTY ||
        !track.id[0]) {
        return;
    }
    pb_clear_restored_pause();
    s_resume_seek_pending = position_ms > 0;
    s_resume_position_ms = position_ms;
    strncpy(s_resume_track_id, track.id, sizeof(s_resume_track_id) - 1);
    s_resume_track_id[sizeof(s_resume_track_id) - 1] = '\0';
    s_play_current_requested = 1;
    s_navigation_requested = 0;
    s_play_retry_after_us = 0;
    logLine("pb: resume requested track_id='%s' position_ms=%d\n",
            s_resume_track_id, s_resume_position_ms);
}

void playback_controller_restore_paused_current(int position_ms)
{
    TrackEntry track;
    int rc;

    memset(&track, 0, sizeof(track));
    rc = playback_queue_get_current(&track);
    if (rc == PLAYBACK_QUEUE_EMPTY || !track.id[0]) {
        pb_clear_restored_pause();
        return;
    }
    if (position_ms < 0) position_ms = 0;
    s_play_current_requested = 0;
    s_resume_seek_pending = 0;
    s_resume_position_ms = 0;
    s_resume_track_id[0] = '\0';
    s_restored_paused = 1;
    s_restored_position_ms = position_ms;
    strncpy(s_restored_track_id, track.id,
            sizeof(s_restored_track_id) - 1);
    s_restored_track_id[sizeof(s_restored_track_id) - 1] = '\0';
    if (rc == PLAYBACK_QUEUE_OK) {
        memcpy(&g_playback.current_track, &track, sizeof(track));
        if (track.duration_ms > 0 &&
            s_restored_position_ms > track.duration_ms) {
            s_restored_position_ms = track.duration_ms;
        }
    } else {
        pb_request_track_hydration(&track);
    }
    logLine("pb: restored paused track_id='%s' position_ms=%d metadata=%s\n",
            s_restored_track_id, s_restored_position_ms,
            rc == PLAYBACK_QUEUE_OK ? "ready" : "pending");
}

int playback_controller_prepare_current(TrackEntry *out)
{
    TrackEntry track;
    int rc;

    memset(&track, 0, sizeof(track));
    rc = playback_queue_get_current(&track);
    if (rc == PLAYBACK_QUEUE_EMPTY || !track.id[0]) {
        return -1;
    }
    if (rc == PLAYBACK_QUEUE_PENDING) {
        pb_request_track_hydration(&track);
        return 0;
    }
    memcpy(&g_playback.current_track, &track, sizeof(track));
    if (s_restored_paused &&
        strcmp(s_restored_track_id, track.id) == 0 &&
        track.duration_ms > 0 && s_restored_position_ms > track.duration_ms) {
        s_restored_position_ms = track.duration_ms;
    }
    if (out) memcpy(out, &track, sizeof(track));
    return 1;
}

int playback_controller_has_current(void)
{
    PlaybackQueueInfo info;
    return playback_queue_get_info(&info) == 0 && info.count > 0 &&
           info.current_index >= 0 && info.current_index < info.count;
}

int playback_controller_get_restored_pause(const char *track_id,
                                            int *position_ms)
{
    if (!s_restored_paused ||
        (track_id && strcmp(track_id, s_restored_track_id) != 0)) {
        return 0;
    }
    if (position_ms) *position_ms = s_restored_position_ms;
    return 1;
}

static void playback_controller_request_navigation(int direction)
{
    s_resume_seek_pending = 0;
    s_resume_position_ms = 0;
    s_resume_track_id[0] = '\0';
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
    pb_clear_restored_pause();
    s_stop_playback_requested = 1;
    s_pause_toggle_requested = 0;
    logLine("pb: stop requested\n");
}

/* SEEK: queue a relative seek; consumed by playback_controller_service(). */
void playback_controller_request_seek_relative(int delta_ms)
{
    s_seek_delta_ms += delta_ms;
}

static void playback_controller_service_navigation(void)
{
    TrackEntry target;
    /* WAVE-REPORT: outgoing context stashed before the cursor moves. */
    PlaybackReportContext nav_outgoing;
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
        pb_request_track_hydration(&target);
        s_play_retry_after_us = pb_now_us() + PB_PENDING_RETRY_US;
        logLine("pb: %s waiting metadata track_id='%s'\n",
                direction > 0 ? "next" : "previous", target.id);
        return;
    }

    s_play_retry_after_us = 0;
    /* WAVE-REPORT: stash the outgoing track; skip is queued after accept. */
    nav_outgoing = s_report_ctx;
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
    pb_clear_restored_pause();
    last_play_save();

    s_navigation_requested = 0;
    s_prefetch_triggered = 0;
    s_cover_prefetch_triggered = 0;
    s_prefetch_retry_after_us = 0;
    s_advance_triggered = 0;
    s_error_skips = 0;
    s_network_retry_backoff_us = 500000ULL;
    /* WAVE-REPORT: manual next/previous after first PCM → skip. */
    wave_report_note_old(&nav_outgoing, 0);
    logLine("pb: manual %s committed track_id='%s'\n",
            direction > 0 ? "next" : "previous", target.id);
}

void playback_controller_service(void)
{
    if (s_restored_paused &&
        (g_playback.current_track.id[0] == '\0' ||
         strcmp(g_playback.current_track.id, s_restored_track_id) != 0 ||
         g_playback.current_track.title[0] == '\0')) {
        (void)playback_controller_prepare_current(NULL);
    }

    if (s_stop_playback_requested) {
        s_stop_playback_requested = 0;
        s_play_current_requested = 0;
        s_navigation_requested = 0;
        s_network_retry_pending = 0;
        s_advance_triggered = 0;
        s_resume_seek_pending = 0;
        s_resume_position_ms = 0;
        s_resume_track_id[0] = '\0';
        audio_player_stop();
        /* WAVE-REPORT: manual stop after first PCM → skip (snapshot taken
         * after the worker died, so audible_ms is final). */
        wave_report_note_old(&s_report_ctx, 0);
        s_report_ctx.terminal_reported = 1;
    }

    /* WAVE-REPORT: first really-audible block → /play-audio (+trackStarted). */
    wave_report_poll_first_pcm();

    /* SEEK: forward one queued relative seek to the player engine. Dropped
     * when the engine is busy/idle; UI hold repeats re-issue it. */
    if (s_seek_delta_ms != 0) {
        AudioPlayerSnapshot seek_snap;
        int seek_delta = s_seek_delta_ms;
        s_seek_delta_ms = 0;
        if (audio_player_get_snapshot(&seek_snap)) {
            audio_player_seek_to_ms(seek_snap.position_ms + seek_delta);
        }
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
            last_play_save();
        } else if (g_playback.current_track.id[0] != '\0') {
            if (s_restored_paused &&
                strcmp(s_restored_track_id,
                       g_playback.current_track.id) == 0) {
                s_resume_seek_pending = s_restored_position_ms > 0;
                s_resume_position_ms = s_restored_position_ms;
                strncpy(s_resume_track_id, s_restored_track_id,
                        sizeof(s_resume_track_id) - 1);
                s_resume_track_id[sizeof(s_resume_track_id) - 1] = '\0';
            }
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
                if (!s_resume_seek_pending) last_play_save();
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
        if (result == PLAYBACK_INTENT_ACCEPTED) {
            pb_clear_restored_pause();
        }
        if (result != PLAYBACK_INTENT_ACCEPTED) {
            logLine("pb: deferred play_current rejected\n");
        }
    }

    if (s_resume_seek_pending) {
        AudioPlayerSnapshot resume_snapshot;
        if (audio_player_get_snapshot(&resume_snapshot) &&
            strcmp(resume_snapshot.track_id, s_resume_track_id) == 0 &&
            audio_player_seek_to_ms(s_resume_position_ms) == 0) {
            logLine("pb: resume seek accepted track_id='%s' position_ms=%d\n",
                    s_resume_track_id, s_resume_position_ms);
            s_resume_seek_pending = 0;
            s_resume_position_ms = 0;
            s_resume_track_id[0] = '\0';
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
                if (!next.available) {
                    s_prefetch_retry_after_us = pb_now_us() + PB_PENDING_RETRY_US;
                    logLine("pb: prefetch skipped unavailable track_id='%s'\n",
                            next.id);
                } else {
                    char token[256];
                    token[0] = '\0';
                    if (token_loader_read(token, sizeof(token)) == 0) {
                        audio_cache_prefetch_start(&next, token);
                        memset(token, 0, sizeof(token));
                        s_prefetch_triggered = 1;
                        s_cover_prefetch_triggered = 0;
                        logLine("pb: prefetch triggered track_id='%s'\n", next.id);
                    }
                }
            } else if (rc == PLAYBACK_QUEUE_PENDING) {
                pb_request_track_hydration(&next);
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
        /* WAVE-REPORT: natural EOF → trackFinished before the cursor moves. */
        wave_report_note_old(&s_report_ctx, 1);
        if (playback_queue_move_next() == 0) {
            TrackEntry next;
            int rc = playback_queue_get_current(&next);
            last_play_save();
            if (rc == PLAYBACK_QUEUE_PENDING) {
                /* Queue already moved; start via the deferred-play path once
                 * the metadata lands. play_current resets s_advance_triggered. */
                pb_request_track_hydration(&next);
                s_play_retry_after_us =
                    pb_now_us() + PB_PENDING_RETRY_US;
                s_play_current_requested = 1;
                s_prefetch_triggered = 0;
                s_cover_prefetch_triggered = 0;
                logLine("pb: advance waiting metadata track_id='%s'\n", next.id);
            } else if (s_prefetch_triggered && audio_cache_prefetch_swap() == 0) {
                memcpy(&g_playback.current_track, &next, sizeof(next));
                /* WAVE-REPORT: swapped-in track starts a fresh report ctx. */
                wave_report_capture(&next);
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
            /* WAVE-REPORT: natural EOF at queue end still closes the report. */
            wave_report_note_old(&s_report_ctx, 1);
            s_report_ctx.terminal_reported = 1;
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
                /* WAVE-REPORT: decoder error after first PCM → skip with the
                 * actual audible time (the worker is already dead, so the
                 * terminal snapshot is final). */
                wave_report_note_old(&s_report_ctx, 0);
                if (s_error_skips <= PB_MAX_CONSECUTIVE_ERROR_SKIPS &&
                    playback_queue_move_next() == 0) {
                    TrackEntry next;
                    int rc = playback_queue_get_current(&next);
                    last_play_save();
                    logLine("pb: error skip %d/%d -> track_id='%s'\n",
                            s_error_skips, PB_MAX_CONSECUTIVE_ERROR_SKIPS, next.id);
                    s_prefetch_triggered = 0;
                    s_cover_prefetch_triggered = 0;
                    if (rc == PLAYBACK_QUEUE_PENDING) {
                        pb_request_track_hydration(&next);
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
            pb_request_track_hydration(&track);
            s_play_retry_after_us =
                pb_now_us() + PB_PENDING_RETRY_US;
            s_play_current_requested = 1;
            logLine("pb: play_current waiting metadata track_id='%s'\n", track.id);
            if (!s_resume_seek_pending) last_play_save();
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

    {
        PlaybackIntentResult result = playback_controller_start_track(&track);
        if (result == PLAYBACK_INTENT_ACCEPTED && !s_resume_seek_pending) {
            last_play_save();
        }
        return result;
    }
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
