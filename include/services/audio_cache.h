#ifndef YM_AUDIO_CACHE_H
#define YM_AUDIO_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app/track.h"
#include "services/audio_stream_buf.h"

/* State of the cache entry for the current track. */
typedef enum {
    AUDIO_CACHE_MISS              = 0,
    AUDIO_CACHE_IDLE              = 0,
    AUDIO_CACHE_DOWNLOADING       = 1,
    AUDIO_CACHE_PROGRESSIVE_READY = 2,
    AUDIO_CACHE_READY             = 3,
    AUDIO_CACHE_ERROR             = 4,
    AUDIO_CACHE_CANCELLED         = 5
} AudioCacheState;

/* Consistent snapshot of cache/source state.
 * Only audio_cache_get_status() guarantees all fields are coherent.
 * Do NOT read path from a raw volatile — use get_status(). */
typedef struct {
    AudioCacheState state;
    char            track_id[40];
    char            path[128];
    int64_t         downloaded_bytes;
    int64_t         content_length;
    int             bitrate_kbps;
    int             complete;
    int             progressive;
    int             source_ref_count;
    int             error_code;
} AudioCacheStatus;

typedef struct {
    AudioCacheState state;
    char            track_id[40];
    char            path[128];
    int64_t         downloaded_bytes;
    int64_t         content_length;
    int             bitrate_kbps;
    int             complete;
    int             progressive;
    int             error_code;
    int             use_stream_buf; /* 1 = live stream (read from RAM), 0 = cached file */
    int             slot_id;
    AudioStreamBuf *stream_buf;
} AudioSourceSnapshot;

/* Lifecycle — call from ui_screens_init / ui_screens_shutdown. */
void audio_cache_init(void);
int audio_cache_shutdown(void);
int audio_cache_quiesce(void);

/* Start a cache download for track.
 * token must be pre-loaded by the caller on the calling (UI) thread.
 * - Same track already DOWNLOADING or READY: no-op, returns 0.
 * - Different track in progress: posts cancel + new job, never blocks.
 * cover_manager_pause/resume_network() are called internally.
 * Returns 0 if the job is accepted (or already satisfied), <0 on refusal
 * (invalid args, source still held by the player, worker unavailable) —
 * the caller must NOT start the player on refusal. */
int audio_cache_start(const TrackEntry *track, const char *token);

/* Start a background download for the next track without changing active state.
 * Returns immediately. A new prefetch cancels any previous prefetch slot. */
void audio_cache_prefetch_start(const TrackEntry *track, const char *token);
int  audio_cache_prefetch_swap(void);
AudioCacheState audio_cache_prefetch_get_state(void);

/* Cheap polling — reads volatile s_state without locking.
 * Use for per-frame UI status checks.
 * MUST NOT be used to infer path or other related fields. */
AudioCacheState audio_cache_get_state(void);

/* Consistent snapshot under mutex.
 * Use this whenever path, track_id, or error_code are needed. */
bool audio_cache_get_status(AudioCacheStatus *out);
bool audio_cache_error_is_network(void);

/* Acquire/release a source for audio_player.
 * Returns 0 only for PROGRESSIVE_READY or READY sources.
 */
int audio_cache_get_source(AudioSourceSnapshot *out);
void audio_cache_release_source(const char *track_id);

#endif /* YM_AUDIO_CACHE_H */
