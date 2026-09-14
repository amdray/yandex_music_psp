#ifndef YM_PLAYBACK_CONTROLLER_H
#define YM_PLAYBACK_CONTROLLER_H

#include "app/track.h"
#include "services/audio_cache.h"


/* Playback state from the controller's perspective.
 * Maps to AudioCacheState; extended later when audio_player is added. */
typedef enum {
    PB_IDLE      = 0,  /* no active track                  */
    PB_BUFFERING = 1,  /* audio_cache is downloading       */
    PB_READY     = 2,  /* file cached, ready for playback  */
    PB_ERROR     = 3   /* download or URL fetch failed     */
} PlaybackStatus;

typedef enum {
    PLAYBACK_INTENT_REJECTED = 0,
    PLAYBACK_INTENT_ACCEPTED = 1
} PlaybackIntentResult;

/* Public state — current_track is set by playback_controller_play_current().
 * Read pb_status via playback_controller_get_status(), not from g_playback
 * directly; the field is removed and the getter wraps audio_cache. */
typedef struct {
    TrackEntry current_track;
} PlaybackState;

extern PlaybackState g_playback;

void          playback_controller_init(void);
void          playback_controller_shutdown(void);
int           playback_controller_quiesce(void);
void          playback_controller_request_play_current(void);
/* Deferred queue navigation. UI code only submits intent; the controller
 * resolves metadata and switches the player/cache from service(). */
void          playback_controller_request_next(void);
void          playback_controller_request_previous(void);
void          playback_controller_request_toggle_pause(void);
void          playback_controller_request_stop(void);
/* SEEK: queue a relative seek (ms, signed) for UI hold handling. Deferred
 * like other intents: service() forwards position_ms + delta to
 * audio_player_seek_to_ms once per frame; dropped when the engine is
 * busy/idle (hold repeats re-issue). */
void          playback_controller_request_seek_relative(int delta_ms);
void          playback_controller_service(void);

/* Start playback for the current queue item.
 * The queue snapshot must already be populated by the caller. */
PlaybackIntentResult playback_controller_play_current(void);

/* Map audio_cache_get_state() to PlaybackStatus.
 * Cheap — no locking, uses volatile poll. */
PlaybackStatus playback_controller_get_status(void);

#endif /* YM_PLAYBACK_CONTROLLER_H */
