#ifndef YM_SERVICES_NET_CLIENT_H
#define YM_SERVICES_NET_CLIENT_H

#include <stddef.h>

#include "app/user.h"
#include "app/playlist.h"
#include "app/track.h"
#include "app/artist.h"

// Forward declaration
struct AppState;

int net_client_init(int profile_id);
int net_client_poll(void);
int net_client_is_ready(void);
int net_client_runtime_started(void);
int net_client_has_error(void);
int net_client_requires_process_exit(void);
int net_client_get_state(void);
int net_client_shutdown(void);

int net_client_fetch_user_info(const char *token, UserInfo *info);

typedef enum NetPlaylistLoadKind {
    NET_PLAYLIST_LOAD_MY = 0,
    NET_PLAYLIST_LOAD_LIKED = 1
} NetPlaylistLoadKind;

int net_client_playlist_load_init(void);
int net_client_playlist_load_shutdown(void);
int net_client_playlist_load_quiesce(void);
int net_client_playlist_load_start(NetPlaylistLoadKind kind, const char *token, int uid, int generation);
void net_client_playlist_load_poll(struct AppState *state);

typedef enum NetArtistLoadKind {
    NET_ARTIST_LOAD_LIKED = 0,
    NET_ARTIST_LOAD_BRIEF = 1
} NetArtistLoadKind;

int net_client_artist_load_init(void);
int net_client_artist_load_shutdown(void);
int net_client_artist_load_quiesce(void);
int net_client_liked_artists_load_start(const char *token, int uid, int generation);
int net_client_liked_artists_load_poll(int *out_generation, int *out_rc,
                                       ArtistEntry *out_entries, int max_count, int *out_count);
int net_client_artist_brief_load_start(const char *token, int artist_id, int generation);
int net_client_artist_brief_load_poll(int *out_generation, int *out_rc, ArtistBriefInfo *out);

int net_client_track_bootstrap_init(void);
int net_client_track_bootstrap_shutdown(void);
int net_client_track_bootstrap_quiesce(void);
int net_client_track_bootstrap_start(const char *token, int uid, int playlist_kind, int revision, int generation, const char *uuid, struct AppState *state);
void net_client_track_store_lock(void);
void net_client_track_store_unlock(void);

/* Slide the hydration window of the open list to cover `focus` and request
   background hydration for any not-yet-valid rows in it. Cheap to call every
   frame: repeat requests are deduplicated and rate-limited. */
void net_client_track_window_focus(struct AppState *state, const char *token, int focus);
/* То же, но гидратирует только span строк (быстрый первый экран гейта;
 * полный obtained окном доберёт обычный update). */
void net_client_track_window_focus_span(struct AppState *state, const char *token,
                                        int focus, int span);

/* 1 when rows [pos, pos+span) are hydrated (span clamped to the list end).
   Caller holds the track-store lock. */
int net_client_track_window_ready_locked(const struct AppState *state, int pos, int span);

// Wi-Fi connection info filled by net_client_get_apctl_info().
// All string fields are null-terminated. valid=1 if populated, 0 if not connected.
typedef struct {
    char profile[64];
    char ssid[33];
    char bssid[18];
    char ip[16];
    char subnet[16];
    char gateway[16];
    char dns1[16];
    char dns2[16];
    unsigned int strength;      /* 0-99 */
    unsigned int channel;       /* 1-14 */
    unsigned int security_type; /* 0=None 1=WEP 2=WPA 3=WPA2 */
    int valid;
} NetApctlInfo;

void net_client_get_apctl_info(NetApctlInfo *out);
int net_client_get_apctl_strength(unsigned int *out_strength,
                                  unsigned int *out_generation);

// Build the complete cover URL, replacing %% with size. On failure, out_url is empty.
int net_client_build_cover_url(const char *cover_uri, const char *size, char *out_url, size_t out_size);

// Probe endpoints: fetch response and save to data/logs/ for calibration.
// No parsing, just raw save.
int net_client_probe_liked_albums(const char *token, int uid);

// Fetch liked playlists (other users' playlists the user liked).
int net_client_fetch_liked_artists(const char *token, int uid, ArtistEntry *out, int max_count, int *out_count);
int net_client_fetch_artist_brief_info(const char *token, int artist_id, ArtistBriefInfo *out);

#endif
