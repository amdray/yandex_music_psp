#ifndef YM_SERVICES_WAVE_H
#define YM_SERVICES_WAVE_H

#include <stddef.h>
#include "app/track.h"

#define WAVE_SEEDS_MAX 4
#define WAVE_SEED_SIZE 64

/* Rotor "My wave" session (spec sections 2-4, 7-9).
 *
 * prepare/poll/commit/cancel operate on a request id. Only commit replaces
 * the active session and queue. service runs after playback_controller_service
 * on every frame, including after the requesting screen has left.
 */

typedef struct {
    int active;
    unsigned int generation;
    char radio_session_id[64];
    int refill_in_flight;
    int terminated;
    int radio_started;
} WaveSession;

/* Context carousel card (spec 2.1/section 7). Stored/parsed by the UI layer;
 * selecting a card starts a new Rotor session with its full seeds array. */
typedef struct {
    char name[128];
    char description[160];
    char seeds[WAVE_SEEDS_MAX][WAVE_SEED_SIZE];
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

typedef struct WaveRequestParams {
    char seeds[WAVE_SEEDS_MAX][WAVE_SEED_SIZE];
    int seed_count;
    int include_wave_model;
} WaveRequestParams;

typedef enum {
    WAVE_PREPARE_FAILED,
    WAVE_PREPARE_PENDING,
    WAVE_PREPARE_EXISTING
} WavePrepareStatus;

typedef struct {
    WavePrepareStatus status;
    unsigned int request_id; /* Only PENDING owns a request. */
} WavePrepareResult;

/* Equal contexts reuse the active queue or outstanding request. Context identity
 * is the seed multiset; include_wave_model only controls the response format.
 * Only PENDING requires playback stop before the next service call. */
WavePrepareResult wave_prepare(const WaveRequestParams *params);
/* Clone the saved FLOW queue; poll/commit/cancel use the same request lifecycle. */
WavePrepareResult wave_prepare_restore(void);
/* 0 pending, 1 full first-track data ready, -1 failed or superseded. */
int wave_poll(unsigned int request_id, TrackEntry *first);
/* Commit once, only for the current ready request. 0 success, -1 failure. */
int wave_commit(unsigned int request_id, TrackEntry *first);
void wave_cancel(unsigned int request_id);
/* Cleanup and first requests run unconditionally; refill_enabled follows the
 * playback screen's existing admission policy. */
void wave_service(int refill_enabled);
int wave_quiesce(void);
/* Session introspection for the controller/reporter (0 ok, -1 inactive). */
int wave_is_active(void);
unsigned int wave_current_generation(void);
int wave_get_session(WaveSession *out);
int wave_get_session_id(char *out, size_t out_size);
/* Notify the first audible PCM once per active session. */
void wave_report_started(unsigned int generation, const char *token);

#endif
