#ifndef YM_APP_TRACK_H
#define YM_APP_TRACK_H

#define TRACK_ID_SIZE 40

/* Identity of a track in its release context. The same provider track id may
   occur in several albums, so album_id is part of a queue position. */
typedef struct {
    char id[TRACK_ID_SIZE];
    int album_id;
} TrackRef;

// Field sizes calibrated 2026-04-03 against real data/logs/*.json dumps
// (2000+ tracks sampled). Observed max / field size:
//   id[40]        vs max 36  (UUID form, e.g. "24f44571-0519-4907-a886-a9ed5390882c")
//   title[144]    vs max 40
//   artist[256]   vs max 74  (all artist names joined with ", ")
//   album[144]    vs max 133 (tight, ~11 bytes of margin)
//   version[96]   vs max 83  (tight, ~13 bytes of margin)
//   cover_uri[96] vs max 70
// genre stores the localized display title resolved from the provider catalog.
typedef struct {
    char title[144];
    char artist[256];
    char id[TRACK_ID_SIZE];  // Track ID (строка, может быть большим числом)
    int album_id;  // Album ID (число)
    char album[144];  // Название альбома
    char version[96];
    char album_version[96];
    char genre[96];
    int year;
    int explicit_content;  // 1 if explicit, 0 otherwise
    int duration_ms;
    int available;
    char cover_uri[96];  // URL обложки альбома (с %% для размера)
    char background_video_uri[192];
} TrackEntry;

#endif
