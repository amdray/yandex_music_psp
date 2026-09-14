#ifndef YM_SERVICES_WAVE_H
#define YM_SERVICES_WAVE_H

#include <stddef.h>

/* Rotor "My wave" session (spec sections 2-4, 7-9).
 *
 * Lifecycle (unchanged for existing UI callers):
 *   wave_start_first(token) fetches session/new on a worker;
 *   wave_poll_first() applies the first batch to the queue exactly once;
 *   wave_service() refills near the tail every frame from now_playing;
 *   wave_reset() drops the session (generation-guarded: a late worker
 *   result never applies to a newer session).
 *
 * Reporting hooks for the playback controller (spec 8.2/8.3/8.5):
 *   wave_note_skip()/wave_note_finished() enqueue the terminal Rotor event
 *   for the given track with really-audible time; they resolve batch_id
 *   from the queue FLOW metadata and drop the event when the session is
 *   gone. radioFinished is never sent (spec 4.6).
 */

typedef struct {
    int active;
    unsigned int generation;
    char radio_session_id[64];
    int refill_in_flight;
    int terminated;
} WaveSession;

/* Context carousel card (spec 2.1/section 7). Stored/parsed by the UI layer;
 * selecting a card starts a new Rotor session with its full seeds array. */
typedef struct {
    char name[128];
    char description[160];
    char seeds[4][64];
    int seed_count;
    char cover_uri[256];
    char animation_uri[256];
    char entity_type[16];
} WaveCarouselItem;

/* Artist cutout reference (spec 2, section 7): cutoutCover.uri template with
 * its own cache key, never overwriting the album cover. */
typedef struct {
    char uri[256];
    int track_id;
    unsigned int generation;
} WaveArtistCutout;

void wave_start_first(const char *token);
/* Start with explicit seeds (track-wave: one "track:<id>" seed with
 * include_wave_model=1; carousel: full data.wave.seeds array). */
void wave_start_seeds(const char *token, const char *const *seeds,
                      int seed_count, int include_wave_model);
/* 0 идёт, 1 готово (first_id заполнен), -1 провал. Одноразовый. */
int wave_poll_first(char *out_first_id, int id_size);
/* Держать волну бесконечной: вызывать каждый кадр из now_playing. */
void wave_service(void);
void wave_reset(void);
/* Уход с экрана без убийства сессии (см. wave.c). */
void wave_dismiss_screen(void);

/* Session introspection for the controller/reporter (0 ok, -1 inactive). */
int wave_is_active(void);
unsigned int wave_current_generation(void);
int wave_get_session(WaveSession *out);
int wave_get_session_id(char *out, size_t out_size);

/* Manual-transition terminal events (spec 8.3/8.5): enqueue skip (manual
 * next/previous/stop, decoder error after first PCM) or trackFinished
 * (natural EOF) with really-audible time. played_ms/len_ms are
 * milliseconds; conversion to seconds happens inside. Dropped silently
 * (with a short log) when the session is gone. */
void wave_note_skip(const char *track_id, int album_id,
                    int played_ms, int len_ms);
void wave_note_finished(const char *track_id, int album_id,
                        int played_ms, int len_ms);

#endif
