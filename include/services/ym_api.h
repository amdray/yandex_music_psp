#ifndef YM_SERVICES_YM_API_H
#define YM_SERVICES_YM_API_H

#include <stddef.h>

#include "services/ym_api_types.h"
#include "app/playlist.h"

/* Load-failure cause carried in PlaylistLoadState.error_code and surfaced to the
   user. Non-negative values are HTTP status codes (e.g. 429, 401, 500); the
   negatives below distinguish non-HTTP failures. */
#define NET_LOAD_ERR_INTERNAL  (-1)  /* bad args / missing token / internal   */
#define NET_LOAD_ERR_TRANSPORT (-2)  /* no HTTP response (connection failed)  */
#define NET_LOAD_ERR_DATA      (-3)  /* HTTP 200 but empty/unparseable body   */

YmStatus ym_api_download_get_mp3_raw(YmApiContext *ctx,
                                     const char *track_id,
                                     const char *quality,
                                     YmDownloadVariant *out);

/* FLAC family with quality=lossless. FLAC-MP4 remains in its container.
 * YM_ERR_UNSUPPORTED, если сервер выбрал другой кодек (aac/mp3). */
YmStatus ym_api_download_get_flac_raw(YmApiContext *ctx,
                                      const char *track_id,
                                      YmDownloadVariant *out);

/* Классический AAC: codecs=aac-mp4, quality задаёт вызывающий, без HE-AAC.
 * YM_ERR_UNSUPPORTED, если сервер выбрал другой кодек.
 * Контейнер MP4; фактический битрейт определяется ответом сервера. */
YmStatus ym_api_download_get_aac_raw(YmApiContext *ctx,
                                     const char *track_id,
                                     const char *quality,
                                     YmDownloadVariant *out);

/* out_status (optional) receives the load-failure cause: 0 on success,
   an HTTP status on non-200, or a NET_LOAD_ERR_* code otherwise. */
int ym_api_playlists_list(YmApiContext *ctx,
                          int uid,
                          PlaylistEntry **out,
                          int *out_count,
                          int *out_status);

int ym_api_playlist_metadata(YmApiContext *ctx,
                             int uid,
                             int playlist_kind,
                             PlaylistEntry *out,
                             int *out_status);

int ym_api_liked_playlists(YmApiContext *ctx,
                           int uid,
                           PlaylistEntry **out,
                           int *out_count,
                           int *out_status);

int ym_api_playlist_tracks_build_url(int uid,
                                     int playlist_kind,
                                     char *out,
                                     size_t out_size);

/* Fetch metadata in request order. Validate all returned IDs before callbacks.
   Rights-blocked entries retain their positions with available=0.
   YM_API_HYDRATE_MAX is the application's batch capacity, not a server limit. */
#define YM_API_HYDRATE_MAX 64

int ym_api_tracks_hydrate_refs(YmApiContext *ctx,
                               const TrackRef *refs,
                               int count,
                               YmApiTrackCallback on_track,
                               void *user_data,
                               int *out_status);

/* Parse composition and its revision from one complete response. Caller owns refs. */
int ym_api_parse_playlist_composition(const char *json, size_t size,
                                      TrackRef **out_refs, int *out_count, int *out_revision);

#endif
