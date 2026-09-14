#ifndef YM_SERVICES_YM_API_ALBUMS_H
#define YM_SERVICES_YM_API_ALBUMS_H

#include "services/ym_api_types.h"
#include "services/list_index.h"
#include "app/artist.h"  // ArtistAlbumEntry как строка альбома

// Лайкнутые альбомы: GET /users/{uid}/likes/albums.
// Массив malloc-ится (как liked playlists), освободить через free().
int ym_api_liked_albums(YmApiContext *ctx, int uid,
                        ArtistAlbumEntry **out, int *out_count);

// Треки альбома: GET /albums/{id}/with-tracks -> result.volumes[][].
// Только id (метаданные подтянет гидратор). Массив malloc, free().
#define YM_API_ALBUM_TRACKS_MAX 256
int ym_api_album_track_ids(YmApiContext *ctx, int album_id,
                           ListIndexId **out, int *out_count);

#endif
