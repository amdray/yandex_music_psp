#ifndef YM_APP_STATE_H
#define YM_APP_STATE_H

#include <psptypes.h>

#include "hal/hal_input.h"
#include "app/user.h"
#include "app/playlist.h"
#include "app/track.h"
#include "app/artist.h"
#include "services/list_index.h"

typedef enum ScreenId {
    SCREEN_SPLASH = 0,
    SCREEN_MENU,
    SCREEN_NOW_PLAYING,
    SCREEN_ALBUM_LIST,
    SCREEN_PLAYLIST_LIST,
    SCREEN_TRACK_LIST,
    SCREEN_ACCOUNT,
    SCREEN_ARTIST,
    SCREEN_ARTIST_MENU,
    SCREEN_NET_INFO,
    SCREEN_DEVICE_LOGIN,
    SCREEN_EQ,
    SCREEN_WAVE,
    SCREEN_SEARCH,
    SCREEN_HELP,
    SCREEN_COUNT
} ScreenId;

typedef enum TrackBootStatus {
    TRACK_BOOT_IDLE = 0,
    TRACK_BOOT_LOADING,    /* id order still arriving (cold stream) */
    TRACK_BOOT_FULL_READY, /* id order complete; rows hydrate on demand */
    TRACK_BOOT_ERROR
} TrackBootStatus;

typedef enum PlaylistLoadStatus {
    PLAYLIST_LOAD_IDLE = 0,
    PLAYLIST_LOAD_LOADING,
    PLAYLIST_LOAD_READY,
    PLAYLIST_LOAD_ERROR
} PlaylistLoadStatus;

typedef enum AppResourceMode {
    APP_MODE_BROWSING = 0,
    APP_MODE_PLAYBACK
} AppResourceMode;

/* Rows visible on the track-list screen; the transition gate must have this
   many rows ready around the entry position (see screen-transition contract:
   no entering a screen whose visible content is not loaded yet). */
#define TRACK_LIST_VISIBLE_ROWS 5

/* Hydrated rows kept in RAM around the focus position. Everything outside
   the window lives as an id only; rows re-hydrate from the metadata store
   (or one POST /tracks) when the window slides over them. */
#define TRACK_WINDOW_CAPACITY 48

/* Cursor anchor of a previously visited playlist: navigation identity is the
   track id, never the ordinal — an external edit shifts positions, so on
   re-entry the id is re-located (pos is just the scan hint / removal
   fallback). One slot per playlist uuid, LRU-less rolling table. */
#define TRACK_ANCHOR_CAPACITY 32

typedef struct TrackAnchor {
    char uuid[48];
    char track_id[40];
    int pos;
} TrackAnchor;

typedef struct TrackUiState {
    int pending_playlist_selected_index;
    int track_selected;
    int track_scroll;
} TrackUiState;

typedef struct TrackBootstrapState {
    int generation;
    int target_playlist_kind;
    char target_uuid[48];  /* uuid of the playlist being loaded / shown */
    char target_playlist_title[64];
    TrackBootStatus status;
    int loaded_count;
    int error_code;
} TrackBootstrapState;

/* The open list: the full id order (cheap, grows unbounded) plus a hydrated
   window of full TrackEntry rows around the focus. Guarded by the track-store
   mutex; the window is filled by the hydrator sink on its worker thread. */
typedef struct TrackRuntimeStorage {
    ListIndexId *ids;   /* id order of the open list */
    int ids_capacity;
    int count;          /* ids populated so far */
    int ids_complete;   /* 1 when count is the final list length */

    TrackEntry window[TRACK_WINDOW_CAPACITY];
    unsigned char window_valid[TRACK_WINDOW_CAPACITY];
    int window_start;   /* list position of window[0] */
} TrackRuntimeStorage;

typedef struct PlaylistLoadState {
    int generation;
    PlaylistLoadStatus status;
    int error_code;
} PlaylistLoadState;

#define APP_SCREEN_STACK_CAPACITY SCREEN_COUNT

typedef struct AppState {
    // Navigation depth is bounded by the number of distinct screens in the app.
    ScreenId stack[APP_SCREEN_STACK_CAPACITY];
    int stack_size;
    AppResourceMode resource_mode;
    int menu_index;
    u32 splash_start_ms;
    u64 ui_now_us;

    UserInfo currentUser;
    
    // Playlists
    PlaylistEntry *playlists;
    int playlist_count;
    int playlist_capacity;
    int playlist_selected;
    int playlist_scroll;
    PlaylistLoadState playlists_load;

    // Liked playlists (чужие плейлисты с лайком)
    PlaylistEntry *liked_playlists;
    int liked_playlist_count;
    int liked_playlist_capacity;
    int playlist_tab;            // 0 = личные (playlists[]), 1 = лайкнутые (liked_playlists[])
    int liked_playlist_selected;
    int liked_playlist_scroll;
    PlaylistLoadState liked_playlists_load;
    
    // Track UI / bootstrap / runtime storage split
    TrackUiState track_ui;
    TrackBootstrapState track_boot;
    TrackRuntimeStorage track_store;
    
    // Now Playing
    TrackEntry now_playing_track;  // Full track info for now playing screen

    // Liked artists
    ArtistEntry liked_artists[MAX_LIKED_ARTISTS];
    int liked_artist_count;
    int artist_selected;
    int artist_scroll;

    // Artist brief-info (for SCREEN_ARTIST_MENU)
    ArtistBriefInfo artist_brief;
    int             artist_brief_loading;  /* 0=idle 1=loading 2=done -1=error */
    int             artist_menu_selected;

    // Per-playlist cursor anchors (identity-based re-entry)
    TrackAnchor track_anchors[TRACK_ANCHOR_CAPACITY];
    int track_anchor_count;
    int track_anchor_next;  /* rolling overwrite slot when the table is full */
} AppState;

static inline ScreenId app_state_get_current(const AppState *state)
{
    return (state && state->stack_size > 0) ? state->stack[state->stack_size - 1] : SCREEN_COUNT;
}

static inline AppResourceMode app_state_get_resource_mode(const AppState *state)
{
    return state ? state->resource_mode : APP_MODE_BROWSING;
}

int app_state_init(AppState *state);
void app_state_shutdown(AppState *state);
void app_state_free_tracks(AppState *state);
void app_state_reset_track_bootstrap(AppState *state, int playlist_kind,
                                     int playlist_selected_index,
                                     const char *playlist_uuid,
                                     const char *playlist_title);
void app_state_anchor_save(AppState *state, const char *uuid, const char *track_id, int pos);
const TrackAnchor *app_state_anchor_find(const AppState *state, const char *uuid);
void app_state_cancel_track_bootstrap(AppState *state);
void app_state_reset(AppState *state, ScreenId screen);
void app_state_push(AppState *state, ScreenId screen);
void app_state_pop(AppState *state);
void app_state_set(AppState *state, ScreenId screen);

#endif
