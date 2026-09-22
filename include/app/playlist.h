#ifndef YM_APP_PLAYLIST_H
#define YM_APP_PLAYLIST_H

/* Server-defined playlist kind for the user's liked tracks. */
#define PLAYLIST_KIND_LIKED_TRACKS 3

// Unlike track.h, these field sizes were NOT re-verified in the 2026-04-03
// calibration pass (no fresh playlists-list JSON was in the data/logs sample
// at the time) — sizing predates that snapshot and hasn't been rechecked since.
typedef struct {
    char title[64];
    int track_count;
    int playlist_id;  // Playlist ID (переименовано из kind)
    char cover_uri[96];  // URL обложки (с %% для размера)
    int owner_uid;  // 0 = текущий пользователь; != 0 для лайкнутых плейлистов чужого юзера
    char owner_name[96];  // отображаемое имя владельца для лайкнутых плейлистов
    char modified_date[11];  // дата изменения из modified: YYYY-MM-DD
    char uuid[48];  // playlistUuid — глобально уникальный идентификатор
    int revision;   // API revision counter — bumps on any edit; track-cache key
} PlaylistEntry;

#endif
