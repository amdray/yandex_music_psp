#ifndef YM_SERVICES_PLAYBACK_REPORTER_H
#define YM_SERVICES_PLAYBACK_REPORTER_H

#include <stddef.h>

#include "services/playback_queue.h"

/* UI enqueues immutable events; the worker owns disk and network I/O.
 * /play-audio records are persisted per account without credentials. The
 * journal retains the start and latest positive snapshot per playId.
 * Successful delivery or HTTP 400 removes a record; other failures retain it for the next
 * delivery cycle or application start. Rotor feedback uses bounded retries.
 * No server-side idempotency guarantee is assumed. */
typedef struct {
    int active;                    /* 1 = FLOW session: Rotor feedback applies */
    int had_first_pcm;
    int start_reported;
    int terminal_pending;
    int terminal_finished;
    int terminal_reported;         /* skip/trackFinished already queued */
    unsigned int generation;       /* wave generation (FLOW) or local seq */
    char track_id[40];
    int album_id;
    char play_id[40];              /* unique per track start; stable across
                                    * pause/buffering/network recovery */
    char batch_id[64];             /* Rotor batch of this track */
    char radio_session_id[64];     /* Session captured with this track */
    int duration_ms;
    int audible_ms;                /* last known really-audible time */
    int position_ms;
    int uid;
    char token[256];              /* memory only; bound to the starting account */
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

int playback_reporter_init(void);
void playback_reporter_set_account(int uid, const char *token);
void playback_reporter_shutdown(void);
int playback_reporter_quiesce(void);

/* Fill a fresh context for a track start (clears flags, stores ids/batch,
 * source and generation). play_id is left empty: assign with
 * playback_reporter_new_play_id(). */
int playback_report_context_start(PlaybackReportContext *ctx,
                                   const char *track_id, int album_id,
                                   const char *batch_id,
                                   int duration_ms,
                                   unsigned int generation,
                                   PlaybackQueueSource source,
                                   const char *token);

/* New play-id from calendar time, full uptime and process counter. 0 ok. */
int playback_reporter_new_play_id(char *out, size_t out_size);

/* Uses the account captured in ctx. store_only saves a checkpoint without
 * sending. 0 means the worker accepted ownership of an immutable copy. */
int playback_reporter_enqueue_play_audio(const PlaybackReportContext *ctx,
                                         int total_played_ms, int position_ms,
                                         int store_only);

/* Enqueue one Rotor feedback event. total_played_ms/track_len_ms are milliseconds
 * (pass 0 total for start-like events). The session id is copied into the
 * event (immutable, generation-safe). radioStarted carries no batch or
 * track: use playback_reporter_enqueue_radio_started() instead. */
int playback_reporter_enqueue_feedback(PlaybackReportType type,
                                       const PlaybackReportContext *ctx,
                                       const char *token,
                                       const char *radio_session_id,
                                       int total_played_ms,
                                       int track_len_ms);

/* Enqueue radioStarted for a fresh Rotor session. */
int playback_reporter_enqueue_radio_started(const char *radio_session_id,
                                            const char *token);

#endif
