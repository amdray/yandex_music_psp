#ifndef YM_APP_ARTIST_H
#define YM_APP_ARTIST_H

#define MAX_LIKED_ARTISTS      256
#define ARTIST_VISIBLE_SLOTS     5
#define ARTIST_MAX_ALBUMS         10  /* server returns max 9, counts.directAlbums may be higher (paginated) */
#define ARTIST_MAX_ALSO_ALBUMS    10  /* server returns max 9 alsoAlbums */
#define ARTIST_MAX_POPULAR_TRACKS 10

typedef struct {
    char id[12];         /* max 8 digits in real data */
    char name[64];       /* max 50 bytes UTF-8 in real data */
    char cover_uri[80];  /* max 70 chars (with %%), no https:// */
    int  track_count;
    int  album_count;
    char genre[20];      /* max 16 bytes UTF-8 in real data */
    int  artist_id;      /* numeric id for cover cache key */
} ArtistEntry;

/* One album from brief-info response (used for both albums[] and alsoAlbums[]) */
typedef struct {
    int  album_id;       /* id: int */
    char title[80];      /* title: max 68 UTF-8 bytes (alsoAlbums) across 15 responses */
    char version[96];    /* optional edition label, e.g. Deluxe Edition */
    int  year;
    char cover_uri[80];  /* coverUri without https://, with %%; max 70 chars */
    int  track_count;
} ArtistAlbumEntry;

/* One popular track from brief-info response */
typedef struct {
    int  track_id;       /* id: string in JSON but numeric */
    char title[64];
    int  duration_ms;
    char cover_uri[80];
} ArtistBriefTrack;

/* Full brief-info for the currently viewed artist */
typedef struct {
    int               artist_id;
    int               count_tracks;          /* artist.counts.tracks */
    int               count_direct_albums;   /* artist.counts.directAlbums */
    ArtistAlbumEntry  albums[ARTIST_MAX_ALBUMS];
    int               album_count;
    ArtistAlbumEntry  also_albums[ARTIST_MAX_ALSO_ALBUMS];
    int               also_album_count;
    ArtistBriefTrack  popular_tracks[ARTIST_MAX_POPULAR_TRACKS];
    int               popular_track_count;
} ArtistBriefInfo;

#endif
