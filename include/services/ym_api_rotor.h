#ifndef YM_SERVICES_YM_API_ROTOR_H
#define YM_SERVICES_YM_API_ROTOR_H

#include <stddef.h>

#include "services/ym_api_types.h"
#include "services/wave.h"
#include "app/track.h"

/* Yandex Rotor wave backend (MY_WAVE_INTEGRATION.md sections 1-11, except
 * UI/visualization). All requests go directly from the PSP to
 * api.music.yandex.net through the shared http_request() API.
 *
 * Conventions:
 * - every function returns 0 on success, -1 on failure (transport, HTTP
 *   status or protocol mismatch). No function logs tokens, Authorization
 *   headers or full response bodies.
 * - transient failures (transport/network errors, HTTP 408/429/5xx) return
 *   YM_ROTOR_ERR_RETRY (-2) and may be retried with a bounded backoff;
 *   YM_ROTOR_ERR (-1) is final (bad args, other 4xx, protocol mismatch).
 * - Rotor endpoints speak JSON; /play-audio uses
 *   application/x-www-form-urlencoded with per-value
 *   encoding (values are never spliced raw into the body).
 * - Rotor feedback success is HTTP 200 plus a "result" object; a 200
 *   without "result" is a protocol error. /play-audio success is HTTP 200
 *   plus result="ok".
 * - Timestamps are UTC ISO8601 with millis ("YYYY-MM-DDThh:mm:ss.SSSZ").
 *   If ym_time_format(..., YM_TIME_UTC) cannot obtain a timestamp, the
 *   reporting call returns an error without sending the event.
 * - TrackEntry.id keeps the plain track id; the composite "track-id:album-id"
 *   form exists only inside Rotor requests (ym_api_rotor_composite_id()).
 * - Library reactions belong to ym_api_reactions.h. Rotor feedback is
 *   a separate protocol and does not replace collection mutations.
 */

#define YM_ROTOR_SESSION_ID_SIZE 64
#define YM_ROTOR_BATCH_ID_SIZE 64
#define YM_ROTOR_SEQUENCE_MAX 32
#define YM_ROTOR_SEEDS_MAX WAVE_SEEDS_MAX
#define YM_ROTOR_SEED_SIZE WAVE_SEED_SIZE
#define YM_ROTOR_QUEUE_MAX 64
#define YM_ROTOR_CUTOUT_URI_SIZE 256
#define YM_ROTOR_TIMESTAMP_SIZE 32

#define YM_ROTOR_FROM "web-home-rup_main-radio-default"
#define YM_ROTOR_DEFAULT_SEED "user:onyourwave"

#define YM_ROTOR_OK 0
#define YM_ROTOR_ERR (-1)
#define YM_ROTOR_ERR_RETRY (-2)
#define YM_ROTOR_ERR_BAD_REPORT (-3) /* /play-audio rejected with HTTP 400 */

/* Parsed session/new, session/clone or session/tracks result header. */
typedef struct {
    char radio_session_id[YM_ROTOR_SESSION_ID_SIZE];
    char batch_id[YM_ROTOR_BATCH_ID_SIZE];
    int terminated;
    int unknown_session; /* session/tracks only: server forgot the session */
    int interactive;
    WaveRequestParams context; /* wave.seeds returned by session/clone */
} YmRotorSessionInfo;

/* One sequence[] element: full track plus its batch and like flag.
 * cutout_uri is the first non-empty artists[].cutoutCover.uri in server
 * order ("" when absent); it has its own cache key and never replaces
 * track.cover_uri. */
typedef struct {
    TrackEntry track;
    char batch_id[YM_ROTOR_BATCH_ID_SIZE];
    int liked;
    char cutout_uri[YM_ROTOR_CUTOUT_URI_SIZE];
} YmRotorSequenceItem;

/* Build the Rotor composite id "track-id:album-id". 0 ok, -1 fail. */
int ym_api_rotor_composite_id(const char *track_id, int album_id,
                              char *out, size_t out_size);

/* POST /rotor/session/new with the given seeds (NULL/0 => default seed).
 * include_wave_model != 0 sets includeWaveModel (track-wave sessions).
 * Parses radioSessionId/batchId/sequence[]; full tracks need no extra
 * /tracks hydration. out_items holds up to max_items entries, *out_count
 * the parsed (available) count. Success requires HTTP 200, non-empty
 * session+batch ids, >= 1 available track and terminated != true. */
int ym_api_rotor_session_new(YmApiContext *ctx,
                             const char *const *seeds, int seed_count,
                             int include_wave_model,
                             YmRotorSessionInfo *out_session,
                             YmRotorSequenceItem *out_items, int max_items,
                             int *out_count);

/* Clone a saved session. queue uses composite IDs; track_to_start_from is a
 * plain track ID. Success includes that current track first and wave.seeds. */
int ym_api_rotor_session_clone(YmApiContext *ctx,
                               const char *radio_session_id,
                               const char *const *queue, int queue_count,
                               const char *track_to_start_from,
                               YmRotorSessionInfo *out_session,
                               YmRotorSequenceItem *out_items, int max_items,
                               int *out_count);

/* POST /rotor/session/{id}/tracks with feedbacks:[] and the composite queue
 * of already received tracks in session order (up to YM_ROTOR_QUEUE_MAX).
 * Applies the same sequence[] parsing; honors unknownSession/terminated. */
int ym_api_rotor_session_tracks(YmApiContext *ctx,
                                const char *radio_session_id,
                                const char *const *queue, int queue_count,
                                YmRotorSessionInfo *out_session,
                                YmRotorSequenceItem *out_items, int max_items,
                                int *out_count);

/* POST /rotor/session/{id}/feedback events. *_ms times are nonnegative milliseconds
 * (total played / track length). batch_id is omitted only for radioStarted. */
int ym_api_rotor_feedback_radio_started(YmApiContext *ctx,
                                        const char *radio_session_id);
int ym_api_rotor_feedback_track_started(YmApiContext *ctx,
                                        const char *radio_session_id,
                                        const char *batch_id,
                                        const char *track_id, int album_id,
                                        int track_len_ms);
int ym_api_rotor_feedback_track_finished(YmApiContext *ctx,
                                         const char *radio_session_id,
                                         const char *batch_id,
                                         const char *track_id, int album_id,
                                         int total_played_ms,
                                         int track_len_ms);
int ym_api_rotor_feedback_skip(YmApiContext *ctx,
                               const char *radio_session_id,
                               const char *batch_id,
                               const char *track_id, int album_id,
                               int total_played_ms,
                               int track_len_ms);
int ym_api_rotor_feedback_like(YmApiContext *ctx,
                               const char *radio_session_id,
                               const char *batch_id,
                               const char *track_id, int album_id,
                               int track_len_ms);
int ym_api_rotor_feedback_unlike(YmApiContext *ctx,
                                 const char *radio_session_id,
                                 const char *batch_id,
                                 const char *track_id, int album_id,
                                 int track_len_ms);

/* POST /play-audio (form-urlencoded). from_cache is 0 on PSP (streaming).
 * timestamp_utc/client_now_utc come from ym_time_format(..., YM_TIME_UTC).
 * track_len_ms is the duration in milliseconds. Success is HTTP 200 + result="ok". */
int ym_api_rotor_play_audio(YmApiContext *ctx, int uid,
                            const char *track_id, int album_id,
                            const char *play_id, int from_cache,
                            const char *timestamp_utc,
                            const char *client_now_utc,
                            int track_len_ms, int total_played_ms, int position_ms);

/* Validate the Yandex OAuth token (x_token from the device flow) and
 * resolve the account uid via GET /account/status. The current public API
 * accepts the OAuth token directly, so the "music token" is the same token
 * echoed back: out_music_token receives a copy of ctx->oauth_token and
 * *out_uid the account uid. 0 on success, -1 otherwise. */
int ym_api_rotor_exchange_music_token(YmApiContext *ctx,
                                      char *out_music_token,
                                      size_t out_music_token_size,
                                      int *out_uid);

/* Same as above when only the uid is needed. 0 on success. */
int ym_api_rotor_resolve_uid(YmApiContext *ctx, int *out_uid);

/* GET /albums/{album_id}/similar-entities, keep result.items[] with
 * type=="wave_agent_item" in server order (cap max_cards, >=0 returned,
 * negative verdict on error). Cards without wave.seeds are dropped.
 * Raster/animation URIs are stored verbatim (%% template kept). */
#define YM_ROTOR_CARDS_MAX 8
int ym_api_rotor_album_wave_cards(YmApiContext *ctx, int album_id,
                                  WaveCarouselItem *out_cards, int max_cards);

#endif
