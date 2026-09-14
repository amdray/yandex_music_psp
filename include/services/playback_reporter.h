#ifndef YM_SERVICES_PLAYBACK_REPORTER_H
#define YM_SERVICES_PLAYBACK_REPORTER_H

#include <stddef.h>

#include "services/playback_queue.h"

/* Playback reporter: bounded FIFO + worker thread for /play-audio and Rotor
 * feedback (spec sections 5, 8, 10).
 *
 * Audio/UI threads only enqueue immutable event copies; all network I/O and
 * JSON/form building run on the reporter worker through http_request().
 * Enqueue never blocks on the network and never allocates (static pool), so
 * it is safe to call from time-sensitive paths. FIFO order is preserved.
 *
 * Delivery rules: 408, 429, transport errors and 5xx are retried with a
 * bounded backoff (3 retries: ~1s/2s/4s); other 4xx are dropped. Attempts are
 * bounded; a reporter failure never changes player state. Tokens,
 * Authorization headers and full response bodies are never logged.
 *
 * Idempotency is NOT assumed: each queued event is sent as built; after an
 * indeterminate result the same event is never re-sent through another
 * endpoint.
 */

typedef struct {
    int active;                    /* 1 = FLOW session: Rotor feedback applies */
    int first_pcm_reported;        /* /play-audio + trackStarted already queued */
    int terminal_reported;         /* skip/trackFinished already queued */
    unsigned int generation;       /* wave generation (FLOW) or local seq */
    char track_id[40];
    int album_id;
    char play_id[40];              /* unique per track start; stable across
                                    * pause/buffering/network recovery */
    char batch_id[64];             /* Rotor batch of this track */
    int duration_ms;
    int audible_ms;                /* last known really-audible time */
    PlaybackQueueSource source;
} PlaybackReportContext;

typedef enum {
    PLAYBACK_REPORT_PLAY_AUDIO = 0,
    PLAYBACK_REPORT_RADIO_STARTED,
    PLAYBACK_REPORT_TRACK_STARTED,
    PLAYBACK_REPORT_TRACK_FINISHED,
    PLAYBACK_REPORT_SKIP,
    PLAYBACK_REPORT_LIKE,
    PLAYBACK_REPORT_UNLIKE
} PlaybackReportType;

void playback_reporter_init(void);
void playback_reporter_shutdown(void);
int playback_reporter_quiesce(void);

/* Fill a fresh context for a track start (clears flags, stores ids/batch,
 * source and generation). play_id is left empty: assign with
 * playback_reporter_new_play_id(). */
void playback_report_context_start(PlaybackReportContext *ctx,
                                   const char *track_id, int album_id,
                                   const char *batch_id,
                                   int duration_ms,
                                   unsigned int generation,
                                   PlaybackQueueSource source);

/* Unique play-id for a new track ("<time-hex>-<counter-hex>"). 0 ok. */
int playback_reporter_new_play_id(char *out, size_t out_size);

/* Enqueue /play-audio for ctx (uid <= 0 => worker resolves via
 * /account/status with the event token and caches it). 0 = queued. */
int playback_reporter_enqueue_play_audio(const PlaybackReportContext *ctx,
                                         const char *token, int uid);

/* Enqueue one Rotor feedback event. total_played_s/track_len_s are seconds
 * (pass 0.0 total for start-like events). The session id is copied into the
 * event (immutable, generation-safe). radioStarted carries no batch or
 * track: use playback_reporter_enqueue_radio_started() instead. */
int playback_reporter_enqueue_feedback(PlaybackReportType type,
                                       const PlaybackReportContext *ctx,
                                       const char *token,
                                       const char *radio_session_id,
                                       double total_played_s,
                                       double track_len_s);

/* Enqueue radioStarted for a fresh Rotor session. */
int playback_reporter_enqueue_radio_started(const char *radio_session_id,
                                            const char *token);

#endif
