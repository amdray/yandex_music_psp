#ifndef YM_SERVICES_YM_API_H
#define YM_SERVICES_YM_API_H

#include <stddef.h>

#include "services/ym_api_types.h"
#include "services/list_index.h"
#include "app/playlist.h"

/* Load-failure cause carried in PlaylistLoadState.error_code and surfaced to the
   user. Non-negative values are HTTP status codes (e.g. 429, 401, 500); the
   negatives below distinguish non-HTTP failures. */
#define NET_LOAD_ERR_INTERNAL  (-1)  /* bad args / missing token / internal   */
#define NET_LOAD_ERR_TRANSPORT (-2)  /* no HTTP response (connection failed)  */
#define NET_LOAD_ERR_DATA      (-3)  /* HTTP 200 but empty/unparseable body   */

YmStatus ym_api_download_get_mp3_raw(YmApiContext *ctx,
                                     const char *track_id,
                                     YmDownloadVariant *out);

/* Качество MP3 для следующих треков: "nq" (192) / "hq" (320). */
void ym_api_download_set_quality(const char *q);
const char *ym_api_download_quality(void);

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

/* Hydration: fetch full metadata for up to YM_API_HYDRATE_MAX track ids in one
   POST /tracks (server accepts >=500 per request — the cap here is sized to the
   UI window, not the API). on_track is called per parsed entry; no-rights items
   are skipped, so fewer callbacks than ids is normal. out_status as above. */
#define YM_API_HYDRATE_MAX 64

int ym_api_tracks_hydrate(YmApiContext *ctx,
                          const ListIndexId *ids,
                          int count,
                          YmApiTrackCallback on_track,
                          void *user_data,
                          int *out_status);

int ym_api_playlist_tracks_parser_init(YmPlaylistTracksParser *parser,
                                       YmApiTrackIdCallback on_track_id,
                                       void *user_data);

int ym_api_playlist_tracks_parser_feed(YmPlaylistTracksParser *parser,
                                       const char *data,
                                       size_t size);

void ym_api_playlist_tracks_parser_destroy(YmPlaylistTracksParser *parser);

#endif
