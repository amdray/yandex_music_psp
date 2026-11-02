#include "services/playback_controller.h"

#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/audio_cache.h"
#include "services/audio_player.h"
#include "services/cover_now_playing.h"
#include "services/download_settings.h"
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
static int s_play_prepared = 0;
static int s_pause_toggle_requested = 0;
static int s_stop_playback_requested = 0;
/* -1 previous, +1 next, 0 no navigation intent. Kept separate from the
 * queue cursor so a failed transition cannot move the visible queue. */
static int s_navigation_requested = 0;
static int s_prefetch_triggered = 0;
static TrackRef s_prefetch_target;
static unsigned int s_observed_queue_revision;
static int s_observed_queue_position = -1;
static DownloadSettings s_download_settings;
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
static u64 s_report_checkpoint_us;

static int wave_report_capture(const TrackEntry *track, int order_pos)
{
    PlaybackQueueInfo queue_info;
    PlaybackQueueSource source = PLAYBACK_QUEUE_SOURCE_NONE;
    WaveQueueItem flow;
    int have_flow = 0;
    unsigned int generation;
    char session_id[64] = "";

    if (!track || !track->id[0]) {
        return -1;
    }
    if (playback_queue_get_info(&queue_info) == 0) {
        source = queue_info.source;
    }
    if (source == PLAYBACK_QUEUE_SOURCE_FLOW) {
        generation = wave_current_generation();
        wave_get_session_id(session_id, sizeof(session_id));
        if (playback_queue_get_flow_at(order_pos < 0 ? queue_info.order_pos : order_pos, &flow) == 0) {
            have_flow = 1;
        }
    } else {
        generation = s_report_ctx.generation;
    }
    if (source != PLAYBACK_QUEUE_SOURCE_FLOW) {
        s_report_seq++;
        if (s_report_seq == 0) {
            s_report_seq = 1;
        }
        generation = s_report_seq;
    }
    if (playback_report_context_start(&s_report_ctx,
                                  track->id,
                                  have_flow ? flow.album_id : track->album_id,
                                  have_flow ? flow.batch_id : "",
                                  track->duration_ms,
                                  generation, source, s_token) < 0) return -1;
    snprintf(s_report_ctx.radio_session_id,
             sizeof(s_report_ctx.radio_session_id), "%s", session_id);
    if (playback_reporter_new_play_id(s_report_ctx.play_id,
                                      sizeof(s_report_ctx.play_id)) != 0) {
        s_report_ctx.play_id[0] = '\0';
    }
    audio_player_set_report_generation(s_report_ctx.generation);
    logLine("pb: report ctx track='%s' flow=%d play='%s'\n",
            s_report_ctx.track_id, have_flow, s_report_ctx.play_id);
    return 0;
}

static void wave_report_poll_first_pcm(void)
{
    char pcm_id[40];
    unsigned int pcm_generation = 0;
    if (audio_player_take_first_pcm_event(pcm_id, sizeof(pcm_id), &pcm_generation) == 0 &&
        strcmp(s_report_ctx.track_id, pcm_id) == 0 && s_report_ctx.generation == pcm_generation &&
        !s_report_ctx.had_first_pcm) {
        s_report_ctx.had_first_pcm = 1;
        s_report_checkpoint_us = sceKernelGetSystemTimeWide();
    }
    if (!s_report_ctx.had_first_pcm || s_report_ctx.start_reported ||
        s_report_ctx.terminal_reported || !s_report_ctx.play_id[0]) return;
    if (playback_reporter_enqueue_play_audio(&s_report_ctx, 0, 0, 0) < 0) return;
    s_report_ctx.start_reported = 1;
    if (s_report_ctx.active && s_report_ctx.source == PLAYBACK_QUEUE_SOURCE_FLOW &&
        s_report_ctx.radio_session_id[0] &&
        (s_report_ctx.generation != wave_current_generation() || wave_is_active())) {
        wave_report_started(s_report_ctx.generation, s_report_ctx.token);
        playback_reporter_enqueue_feedback(PLAYBACK_REPORT_TRACK_STARTED,
                                           &s_report_ctx, s_report_ctx.token,
                                           s_report_ctx.radio_session_id,
                                           0, s_report_ctx.duration_ms);
    }
    logLine("pb: report start track='%s' uid=%d queued=1\n", s_report_ctx.track_id, s_report_ctx.uid);
}
/* Resolve really-audible ms for a stashed context: prefer the worker's
 * terminal snapshot on track+generation match, else the last value the
 * controller saw. *out_had_pcm gates event creation (spec 8.5: nothing
 * before the first PCM). */
static void wave_report_terminal_time(const PlaybackReportContext *old,
                                      int *out_audible_ms, int *out_had_pcm, int *out_position_ms)
{
    char snap_id[40];
    unsigned int snap_generation = 0;
    AudioEndReason snap_reason = AUDIO_END_MANUAL_STOP;
    int snap_audible = 0;
    int snap_had_pcm = 0;
    int snap_position_ms = 0;

    if (out_position_ms) *out_position_ms = old ? old->position_ms : 0;

    if (out_audible_ms) {
        *out_audible_ms = old ? old->audible_ms : 0;
    }
    if (out_had_pcm) {
        *out_had_pcm = old ? old->had_first_pcm : 0;
    }
    if (!old || !old->track_id[0]) {
        return;
    }
    if (audio_player_take_terminal_snapshot(snap_id, sizeof(snap_id),
                                            &snap_generation, &snap_reason,
                                            &snap_audible,
                                            &snap_had_pcm, &snap_position_ms) != 0) {
        return;
    }
    (void)snap_reason; /* the controller branch owns the stop reason */
    if (strcmp(snap_id, old->track_id) == 0 &&
        snap_generation == old->generation) {
        if (out_position_ms && snap_had_pcm) *out_position_ms = snap_position_ms;
        if (out_audible_ms) {
            *out_audible_ms = old->audible_ms + snap_audible;
        }
        if (out_had_pcm) {
            *out_had_pcm = old->had_first_pcm || snap_had_pcm;
        }
    }
}

static int wave_report_note_old(PlaybackReportContext *old, int finished)
{
    int audible_ms = 0, had_pcm = 0, position_ms = 0;
    if (!old || !old->track_id[0] || !old->had_first_pcm || old->terminal_reported) return 0;
    wave_report_terminal_time(old, &audible_ms, &had_pcm, &position_ms);
    if (!had_pcm) return 0;
    old->audible_ms = audible_ms;
    old->position_ms = position_ms;
    if (!old->terminal_pending) old->terminal_finished = finished;
    old->terminal_pending = 1;
    if (!old->start_reported ||
        playback_reporter_enqueue_play_audio(old, audible_ms, position_ms, 0) < 0) return -1;
    old->terminal_reported = 1;
    old->terminal_pending = 0;
    logLine("pb: report end track='%s' uid=%d played_ms=%d position_ms=%d queued=1\n",
            old->track_id, old->uid, audible_ms, position_ms);
    if (old->active && old->source == PLAYBACK_QUEUE_SOURCE_FLOW &&
        old->radio_session_id[0] && old->batch_id[0] &&
        (old->generation != wave_current_generation() || wave_is_active()))
        playback_reporter_enqueue_feedback(
            old->terminal_finished ? PLAYBACK_REPORT_TRACK_FINISHED : PLAYBACK_REPORT_SKIP,
            old, old->token, old->radio_session_id, audible_ms, old->duration_ms);
    return 0;
}

static void playback_report_checkpoint(void)
{
    AudioPlayerSnapshot snap;
    u64 now;
    if (!s_report_ctx.start_reported || s_report_ctx.terminal_pending ||
        s_report_ctx.terminal_reported) return;
    now = sceKernelGetSystemTimeWide();
    if (now - s_report_checkpoint_us < 10000000ULL) return;
    if (!audio_player_get_snapshot(&snap) || snap.state != AUDIO_PLAYER_PLAYING ||
        strcmp(snap.track_id, s_report_ctx.track_id) != 0) return;
    if (playback_reporter_enqueue_play_audio(&s_report_ctx,
            s_report_ctx.audible_ms + snap.total_played_ms, snap.position_ms, 1) == 0)
        s_report_checkpoint_us = now;
}/* SEEK: accumulated relative-seek request (ms) from UI hold handling. */
static int s_seek_delta_ms = 0;
/* One-shot absolute seek bound to a track id. It remains pending
 * through asynchronous metadata hydration and is cleared only after the
 * audio worker accepts it or another playback intent supersedes it. */
static int s_absolute_seek_pending = 0;
static int s_absolute_position_ms = 0;
static char s_absolute_track_id[40];
static int s_restored_paused = 0;
static int s_restored_position_ms = 0;
static char s_restored_track_id[40];

/* Pending metadata is checked in the update loop; hydration and disk reads
 * remain in the worker. Transport retries have their own deadline. */
static u64 s_metadata_retry_after_us = 0;
static u64 s_metadata_retry_backoff_us = 500000ULL;
#define PB_PREVIOUS_START_MS 3000

static u64 pb_now_us(void)
{
    return (u64)sceKernelGetSystemTimeWide();
}

static int pb_request_track_hydration(const TrackEntry *track)
{
    if (s_metadata_retry_after_us &&
        (!net_stack_is_ready() || pb_now_us() < s_metadata_retry_after_us)) return 1;
    s_metadata_retry_after_us = 0;
    TrackRef refs[TRACK_HYDRATOR_JOB_MAX];
    TrackRef window[TRACK_HYDRATOR_JOB_MAX];
    PlaybackQueueInfo info;
    char token[256];
    int count = 0;
    if (playback_queue_get_info(&info) < 0 || info.count <= 0) return -1;
    if (track && track->id[0]) {
        memset(&refs[count], 0, sizeof(refs[count]));
        snprintf(refs[count].id, sizeof(refs[count].id), "%s", track->id);
        refs[count++].album_id = track->album_id;
    }
    int n = playback_queue_get_window(window, TRACK_HYDRATOR_JOB_MAX);
    for (int i = 0; i < n && count < TRACK_HYDRATOR_JOB_MAX; ++i) {
        int j;
        for (j = 0; j < count; ++j)
            if (refs[j].album_id == window[i].album_id && strcmp(refs[j].id, window[i].id) == 0) break;
        if (j == count) refs[count++] = window[i];
    }
    if (!count || token_loader_read(token, sizeof(token)) != 0) return -1;
    int rc = track_hydrator_request(TRACK_HYDRATOR_PLAYBACK, token, refs, NULL, count, info.revision);
    memset(token, 0, sizeof(token));
    if (rc == TRACK_HYDRATOR_TRANSPORT_ERROR) {
        track_hydrator_cancel(TRACK_HYDRATOR_PLAYBACK);
        s_metadata_retry_after_us = pb_now_us() + s_metadata_retry_backoff_us;
        logLine("pb: metadata transport retry delay_us=%llu revision=%u\n",
            (unsigned long long)s_metadata_retry_backoff_us, info.revision);
        if (s_metadata_retry_backoff_us < 10000000ULL) {
            s_metadata_retry_backoff_us *= 2;
            if (s_metadata_retry_backoff_us > 10000000ULL)
                s_metadata_retry_backoff_us = 10000000ULL;
        }
        return 1;
    }
    if (rc == 0) s_metadata_retry_backoff_us = 500000ULL;
    return rc;
}
static void pb_clear_restored_pause(void)
{
    s_restored_paused = 0;
    s_restored_position_ms = 0;
    s_restored_track_id[0] = '\0';
}

static PlaybackIntentResult playback_controller_start_track(const TrackEntry *track,
                                                             int order_pos, int recovery)
{
    if (!track || track->id[0] == '\0') {
        logLine("pb: reject invalid track\n");
        return PLAYBACK_INTENT_REJECTED;
    }
    if (!track->available) {
        logLine("pb: reject unavailable track_id='%s'\n", track->id);
        return PLAYBACK_INTENT_REJECTED;
    }

    /* Подсказка для ac_trim_music: файл следующего трека очереди должен
     * пережить чистку — trim идёт до поста prefetch-задания, когда о
     * будущем prefetch в кэше ещё никто не знает. */
    {
        TrackEntry nxt;
        if (playback_queue_get_adjacent(1, &nxt, NULL) == PLAYBACK_QUEUE_OK &&
            nxt.id[0] != '\0') {
            audio_cache_hint_next(nxt.id);
        } else {
            audio_cache_hint_next(NULL);
        }
    }
    if (s_absolute_seek_pending &&
        strcmp(s_absolute_track_id, track->id) != 0) {
        s_absolute_seek_pending = 0;
        s_absolute_position_ms = 0;
        s_absolute_track_id[0] = '\0';
    }

    s_token[0] = '\0';
    if (token_loader_read(s_token, sizeof(s_token)) != 0) {
        logLine("pb: token read failed\n");
        return PLAYBACK_INTENT_REJECTED;
    }

    audio_player_stop();
    AudioPlayerState stopped_state = audio_player_get_state();
    if (stopped_state != AUDIO_PLAYER_STOPPED && stopped_state != AUDIO_PLAYER_IDLE) {
        logLine("pb: reject start, previous player did not stop track_id='%s'\n", track->id);
        memset(s_token, 0, sizeof(s_token));
        return PLAYBACK_INTENT_REJECTED;
    }
    wave_report_poll_first_pcm();
    if (recovery) {
        int played_ms, had_pcm, position_ms = 0;
        wave_report_terminal_time(&s_report_ctx, &played_ms, &had_pcm, &position_ms);
        s_report_ctx.audible_ms = played_ms;
        s_report_ctx.position_ms = position_ms;
    } else {
        if (wave_report_note_old(&s_report_ctx, 0) < 0 ||
            wave_report_capture(track, order_pos) < 0) {
            memset(s_token, 0, sizeof(s_token));
            return PLAYBACK_INTENT_REJECTED;
        }
    }
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

static void pb_sync_download_settings(void)
{
    DownloadSettings settings = download_settings_get();
    if (download_settings_required_quality(settings) !=
        download_settings_required_quality(s_download_settings)) {
        s_prefetch_triggered = 0;
        s_cover_prefetch_triggered = 0;
    }
    s_download_settings = settings;
}

void playback_controller_init(void)
{
    s_metadata_retry_after_us = 0;
    s_metadata_retry_backoff_us = 500000ULL;
    s_download_settings = download_settings_get();
    s_play_prepared = 0;
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
    s_absolute_seek_pending = 0;
    s_absolute_position_ms = 0;
    s_absolute_track_id[0] = '\0';
    pb_clear_restored_pause();
    playback_queue_init();
    audio_player_init();
    logLine("pb: controller init\n");
}

void playback_controller_shutdown(void)
{
    s_metadata_retry_after_us = 0;
    s_metadata_retry_backoff_us = 500000ULL;
    s_play_prepared = 0;
    s_play_current_requested = 0;
    s_pause_toggle_requested = 0;
    s_stop_playback_requested = 0;
    s_navigation_requested = 0;
    s_prefetch_triggered = 0;
    s_cover_prefetch_triggered = 0;
    s_advance_triggered = 0;
    s_network_retry_pending = 0;
    s_absolute_seek_pending = 0;
    s_absolute_position_ms = 0;
    s_absolute_track_id[0] = '\0';
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
    s_metadata_retry_after_us = 0;
    s_metadata_retry_backoff_us = 500000ULL;
    s_play_prepared = 0;
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
    s_absolute_seek_pending = 0;
    s_absolute_position_ms = 0;
    s_absolute_track_id[0] = '\0';
    pb_clear_restored_pause();
    if (audio_player_quiesce() < 0) return -1;
    wave_report_poll_first_pcm();
    return wave_report_note_old(&s_report_ctx, 0);
}

void playback_controller_request_play_current(void)
{
    track_hydrator_cancel(TRACK_HYDRATOR_PLAYBACK);
    s_metadata_retry_after_us = 0;
    s_metadata_retry_backoff_us = 500000ULL;
    s_play_prepared = 0;
    pb_clear_restored_pause();
    s_absolute_seek_pending = 0;
    s_absolute_position_ms = 0;
    s_absolute_track_id[0] = '\0';
    s_play_current_requested = 1;
    s_navigation_requested = 0;
    logLine("pb: play_current requested\n");
}

void playback_controller_request_play_prepared(const TrackEntry *track)
{
    if (!track || !track->id[0]) return;
    playback_controller_request_play_current();
    g_playback.current_track = *track;
    s_play_prepared = 1;
    pb_request_track_hydration(NULL);
    track_hydrator_cancel(TRACK_HYDRATOR_PREPARE);
}

void playback_controller_request_resume_current(int position_ms)
{
    s_play_prepared = 0;
    TrackEntry track;

    memset(&track, 0, sizeof(track));
    if (position_ms < 0) position_ms = 0;
    if (playback_queue_get_current(&track) == PLAYBACK_QUEUE_EMPTY ||
        !track.id[0]) {
        return;
    }
    track_hydrator_cancel(TRACK_HYDRATOR_PLAYBACK);
    s_metadata_retry_after_us = 0;
    s_metadata_retry_backoff_us = 500000ULL;
    pb_clear_restored_pause();
    s_absolute_seek_pending = position_ms > 0;
    s_absolute_position_ms = position_ms;
    strncpy(s_absolute_track_id, track.id, sizeof(s_absolute_track_id) - 1);
    s_absolute_track_id[sizeof(s_absolute_track_id) - 1] = '\0';
    s_play_current_requested = 1;
    s_navigation_requested = 0;
    logLine("pb: resume requested track_id='%s' position_ms=%d\n",
            s_absolute_track_id, s_absolute_position_ms);
}

void playback_controller_restore_paused_current(int position_ms)
{
    s_play_prepared = 0;
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
    s_absolute_seek_pending = 0;
    s_absolute_position_ms = 0;
    s_absolute_track_id[0] = '\0';
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
        if (pb_request_track_hydration(&track) < 0) return -1;
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

static void pb_reset_track_request(void)
{
    track_hydrator_cancel(TRACK_HYDRATOR_PLAYBACK);
    s_metadata_retry_after_us = 0;
    s_metadata_retry_backoff_us = 500000ULL;
    s_play_prepared = 0;
    s_absolute_seek_pending = 0;
    s_absolute_position_ms = 0;
    s_absolute_track_id[0] = '\0';
    s_navigation_requested = 0;
    s_play_current_requested = 0;
    s_network_retry_pending = 0;
    s_advance_triggered = 0;
    s_error_handled = 0;
}

static void playback_controller_request_navigation(int direction)
{
    pb_reset_track_request();
    s_error_skips = 0;
    s_navigation_requested = direction;
    logLine("pb: %s requested\n", direction > 0 ? "next" : "previous");
}

void playback_controller_request_next(void)
{
    playback_controller_request_navigation(1);
}

void playback_controller_request_previous(void)
{
    AudioPlayerSnapshot snapshot;
    if (s_navigation_requested == 0 &&
        audio_player_get_snapshot(&snapshot) &&
        (snapshot.state == AUDIO_PLAYER_PLAYING ||
         snapshot.state == AUDIO_PLAYER_BUFFERING ||
         snapshot.state == AUDIO_PLAYER_OPENING) &&
        strcmp(snapshot.track_id, g_playback.current_track.id) == 0) {
        int position_ms = snapshot.seek_pending
            ? snapshot.seek_target_ms : snapshot.position_ms;
        if (s_absolute_seek_pending &&
            strcmp(s_absolute_track_id, snapshot.track_id) == 0) {
            position_ms = s_absolute_position_ms;
        }
        if (position_ms >= PB_PREVIOUS_START_MS) {
            pb_reset_track_request();
            s_seek_delta_ms = 0;
            s_absolute_seek_pending = 1;
            s_absolute_position_ms = 0;
            snprintf(s_absolute_track_id, sizeof(s_absolute_track_id), "%s",
                     snapshot.track_id);
            logLine("pb: restart requested track_id='%s'\n", snapshot.track_id);
            return;
        }
    }
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
    int direction = s_navigation_requested;
    int rc;

    if (direction == 0) {
        return;
    }

    int target_pos = -1;
    rc = playback_queue_get_adjacent(direction, &target, &target_pos);

    if (rc == PLAYBACK_QUEUE_EMPTY) {
        logLine("pb: %s rejected at queue boundary\n",
                direction > 0 ? "next" : "previous");
        s_navigation_requested = 0;
        return;
    }
    if (rc == PLAYBACK_QUEUE_PENDING) {
        if (pb_request_track_hydration(&target) < 0) {
            s_navigation_requested = 0;
            return;
        }
        return;
    }
    /* WAVE-REPORT: stash the outgoing track; skip is queued after accept. */
    s_metadata_retry_after_us = 0;
    s_metadata_retry_backoff_us = 500000ULL;
    if (playback_controller_start_track(&target, target_pos, 0) != PLAYBACK_INTENT_ACCEPTED) {
        /* The cursor remains unchanged. Do not spin on a permanent failure
         * (missing token, cache refusal); another key press is a fresh intent. */
        s_navigation_requested = 0;
        logLine("pb: %s start rejected; queue unchanged track_id='%s'\n",
                direction > 0 ? "next" : "previous", target.id);
        return;
    }

    rc = playback_queue_select_order_pos(target_pos);
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
    s_advance_triggered = 0;
    s_network_retry_backoff_us = 500000ULL;
    logLine("pb: manual %s committed track_id='%s'\n",
            direction > 0 ? "next" : "previous", target.id);
}

void playback_controller_service(void)
{
    PlaybackQueueInfo queue_info;
    playback_queue_get_info(&queue_info);
    if (queue_info.revision != s_observed_queue_revision ||
        queue_info.order_pos != s_observed_queue_position) {
        if (audio_cache_prefetch_cancel() >= 0) {
            s_observed_queue_revision = queue_info.revision;
            s_observed_queue_position = queue_info.order_pos;
            s_prefetch_triggered = 0;
            s_cover_prefetch_triggered = 0;
            memset(&s_prefetch_target, 0, sizeof(s_prefetch_target));
            track_hydrator_cancel(TRACK_HYDRATOR_PLAYBACK);
            s_metadata_retry_after_us = 0;
            s_metadata_retry_backoff_us = 500000ULL;
            if (queue_info.count > 0) pb_request_track_hydration(NULL);
            TrackEntry next;
            int rc = playback_queue_get_adjacent(1, &next, NULL);
            audio_cache_hint_next(rc != PLAYBACK_QUEUE_EMPTY ? next.id : NULL);
        }
    }
    pb_sync_download_settings();
    if (s_restored_paused &&
        (g_playback.current_track.id[0] == '\0' ||
         strcmp(g_playback.current_track.id, s_restored_track_id) != 0 ||
         g_playback.current_track.title[0] == '\0')) {
        (void)playback_controller_prepare_current(NULL);
    }

    if (s_stop_playback_requested) {
        s_metadata_retry_after_us = 0;
        s_metadata_retry_backoff_us = 500000ULL;
        s_play_prepared = 0;
        s_stop_playback_requested = 0;
        s_play_current_requested = 0;
        s_navigation_requested = 0;
        s_network_retry_pending = 0;
        s_advance_triggered = 0;
        s_absolute_seek_pending = 0;
        s_absolute_position_ms = 0;
        s_absolute_track_id[0] = '\0';
        audio_player_stop();
        wave_report_poll_first_pcm();
        /* WAVE-REPORT: manual stop after first PCM → skip (snapshot taken
         * after the worker died, so audible_ms is final). */
        wave_report_note_old(&s_report_ctx, 0);
    }

    /* WAVE-REPORT: first really-audible block → /play-audio (+trackStarted). */
    wave_report_poll_first_pcm();
    if (s_report_ctx.terminal_pending)
        wave_report_note_old(&s_report_ctx, s_report_ctx.terminal_finished);
    playback_report_checkpoint();

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
                s_absolute_seek_pending = s_restored_position_ms > 0;
                s_absolute_position_ms = s_restored_position_ms;
                strncpy(s_absolute_track_id, s_restored_track_id,
                        sizeof(s_absolute_track_id) - 1);
                s_absolute_track_id[sizeof(s_absolute_track_id) - 1] = '\0';
            }
            s_play_current_requested = 1;
        }
    }

    /* Manual navigation has priority over retries and automatic advance. */
    playback_controller_service_navigation();

    if (s_network_retry_pending &&
        pb_now_us() >= s_network_retry_after_us) {
        if (net_stack_is_ready()) {
            PlaybackIntentResult result =
                playback_controller_start_track(&g_playback.current_track, -1, 1);
            if (result == PLAYBACK_INTENT_ACCEPTED) {
                logLine("pb: network recovered, retry track_id='%s'\n",
                        g_playback.current_track.id);
                if (!s_absolute_seek_pending) last_play_save();
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

    if (s_play_current_requested) {
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

    if (s_absolute_seek_pending) {
        AudioPlayerSnapshot resume_snapshot;
        int seek_ready = 1;
        if (s_absolute_position_ms > 0) {
            AudioCacheStatus cache;
            if (!audio_cache_get_status(&cache) ||
                strcmp(cache.track_id, s_absolute_track_id) != 0 ||
                (cache.state != AUDIO_CACHE_READY &&
                 cache.state != AUDIO_CACHE_PROGRESSIVE_READY)) {
                seek_ready = 0;
            } else if (!cache.from_disk_cache) {
                logLine("pb: restored position reset, cache miss track_id='%s'\n",
                        s_absolute_track_id);
                s_absolute_seek_pending = 0;
                s_absolute_position_ms = 0;
                s_absolute_track_id[0] = '\0';
                last_play_save();
                seek_ready = 0;
            }
        }
        if (seek_ready && audio_player_get_snapshot(&resume_snapshot) &&
            strcmp(resume_snapshot.track_id, s_absolute_track_id) == 0 &&
            audio_player_seek_to_ms(s_absolute_position_ms) == 0) {
            logLine("pb: absolute seek accepted track_id='%s' position_ms=%d\n",
                    s_absolute_track_id, s_absolute_position_ms);
            s_absolute_seek_pending = 0;
            s_absolute_position_ms = 0;
            s_absolute_track_id[0] = '\0';
        }
    }

    if (!s_prefetch_triggered && s_navigation_requested == 0 &&
        !s_play_current_requested && net_stack_is_ready()) {
        AudioPlayerSnapshot snap;
        AudioCacheStatus ac_cur;
        audio_player_get_snapshot(&snap);
        audio_cache_get_status(&ac_cur);
        /* Предзагрузка следующего трека — только после ПОЛНОЙ докачки
         * текущего (требование пользователя); позиция не важна. */
        if (snap.state == AUDIO_PLAYER_PLAYING &&
            ac_cur.complete &&
            g_playback.current_track.id[0] != '\0' &&
            strcmp(ac_cur.track_id, g_playback.current_track.id) == 0) {
            TrackEntry next;
            int rc = playback_queue_get_adjacent(1, &next, NULL);
            if (rc == PLAYBACK_QUEUE_OK) {
                if (!next.available) {
                    logLine("pb: prefetch skipped unavailable track_id='%s'\n",
                            next.id);
                } else {
                    char token[256];
                    token[0] = '\0';
                    if (token_loader_read(token, sizeof(token)) == 0) {
                        int accepted = audio_cache_prefetch_start(&next, token);
                        memset(token, 0, sizeof(token));
                        s_prefetch_triggered = accepted == 0;
                        if (accepted == 0) {
                            snprintf(s_prefetch_target.id, sizeof(s_prefetch_target.id), "%s", next.id);
                            s_prefetch_target.album_id = next.album_id;
                        }
                        s_cover_prefetch_triggered = 0;
                        logLine("pb: prefetch triggered track_id='%s'\n", next.id);
                    }
                }
            } else if (rc == PLAYBACK_QUEUE_PENDING) {
                pb_request_track_hydration(&next);
            }
        }
    }

    if (s_prefetch_triggered && !s_cover_prefetch_triggered &&
        audio_cache_prefetch_get_state(s_prefetch_target.id) == AUDIO_CACHE_READY) {
        TrackEntry next;
        if (playback_queue_get_adjacent(1, &next, NULL) == PLAYBACK_QUEUE_OK &&
            strcmp(next.id, s_prefetch_target.id) == 0 &&
            next.album_id == s_prefetch_target.album_id) {
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
        s_navigation_requested = 1;
        playback_controller_service_navigation();
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
                if (s_error_skips <= PB_MAX_CONSECUTIVE_ERROR_SKIPS) {
                    s_navigation_requested = 1;
                    playback_controller_service_navigation();
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

    if (s_play_prepared) {
        track = g_playback.current_track;
        s_play_prepared = 0;
    } else {
        int rc = playback_queue_get_current(&track);
        if (rc == PLAYBACK_QUEUE_EMPTY) {
            logLine("pb: play_current failed, queue empty\n");
            return PLAYBACK_INTENT_REJECTED;
        }
        if (rc == PLAYBACK_QUEUE_PENDING) {
            /* Resolution uses the RAM store; the worker handles missing data. */
            if (pb_request_track_hydration(&track) < 0) return PLAYBACK_INTENT_REJECTED;
            s_play_current_requested = 1;
            return PLAYBACK_INTENT_ACCEPTED;
        }
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
        PlaybackIntentResult result = playback_controller_start_track(&track, -1, 0);
        logLine("pb: play_current start result=%d track=%s album=%d\n", result, track.id, track.album_id);
        logger_flush();
        if (result == PLAYBACK_INTENT_ACCEPTED && !s_absolute_seek_pending) {
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
