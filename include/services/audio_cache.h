#ifndef YM_AUDIO_CACHE_H
#define YM_AUDIO_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app/track.h"
#include "app/audio_quality.h"

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
    AudioQuality    quality;
    int             from_disk_cache;
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
} AudioSourceSnapshot;

/* Lifecycle — call from ui_screens_init / ui_screens_shutdown. */
void audio_cache_init(void);
int audio_cache_shutdown(void);
int audio_cache_quiesce(void);

/* UI setting, loaded by init; changes govern cleanup for subsequent jobs.
 * Off preserves the temporary cache; on retains downloads for reuse. */
int audio_cache_retain_downloads(void);
void audio_cache_set_retain_downloads(int enabled);

/* Select the source or request for track from either cache slot.
 * token must be pre-loaded by the caller on the calling (UI) thread.
 * - Matching pending or running request, or a source satisfying quality:
 *   reuse it; a matching prefetch slot becomes active without restarting its job.
 * - No matching request: queues replacement without blocking the caller.
 *   Its worker starts after both retired slots have released their resources.
 * cover_manager_pause/resume_network() are called internally.
 * Returns 0 if the job is accepted (or already satisfied), <0 on refusal
 * (invalid args, source still held by the player, worker unavailable) —
 * the caller must NOT start the player on refusal. */
int audio_cache_start(const TrackEntry *track, const char *token);

/* Start a background download for the next track without changing active state.
 * Returns immediately. Replacement waits for the previous download and
 * source holders to release the slot; only its worker frees the buffer. */
int audio_cache_prefetch_start(const TrackEntry *track, const char *token);
AudioCacheState audio_cache_prefetch_get_state(const char *track_id);
int audio_cache_prefetch_cancel(void);

/* Подсказка для чистки cache_music: id следующего трека очереди.
 * trim идёт до пота prefetch-задания — без подсказки файл будущего
 * prefetch вычищается и его приходится качать заново. NULL/пусто —
 * сброс. Вызывать при старте трека (playback_controller). */
void audio_cache_hint_next(const char *track_id);

/* Cheap polling — reads volatile s_state without locking.
 * Use for per-frame UI status checks.
 * MUST NOT be used to infer path or other related fields. */
AudioCacheState audio_cache_get_state(void);

/* Consistent snapshot under mutex.
 * Use this whenever path, track_id, or error_code are needed. */
bool audio_cache_get_status(AudioCacheStatus *out);
bool audio_cache_error_is_network(void);

/* Acquire/release a source for audio_player.
 * Returns 0 only for the requested PROGRESSIVE_READY or READY source not being retired
 * or committed. READY means complete audio; its path may still end in .part.
 * Every successful acquire requires release after all reader handles close.
 * The slot worker commits the cache only after the last release.
 */
int audio_cache_get_source(const char *track_id, AudioSourceSnapshot *out);
void audio_cache_release_source(const char *track_id);

/* One active player. Reports current state immediately, then changes under
 * the cache mutex. Notification must not
 * call cache APIs. Unwatch completes before the callback's user is destroyed. */
typedef void (*AudioCacheSourceNotify)(const AudioCacheStatus *, void *user);
int audio_cache_watch_source(const char *track_id, AudioCacheSourceNotify notify,
                             void *user);
void audio_cache_unwatch_source(void *user);

#endif /* YM_AUDIO_CACHE_H */
