#ifndef YM_SERVICES_API_PARSER_H
#define YM_SERVICES_API_PARSER_H

#include <stddef.h>

#include "app/user.h"
#include "app/playlist.h"
#include "app/track.h"
#include "app/artist.h"

// Forward declaration
struct AppState;

int api_parser_account_status(const char *json_body, size_t json_size, UserInfo *info);
int api_parser_playlists_list(const char *json_body, size_t json_size,
                              PlaylistEntry **out, int *out_count);
int api_parser_liked_playlists(const char *json_body, size_t json_size,
                               PlaylistEntry **out, int *out_count);
int api_parser_liked_artists(const char *json_body, size_t json_size, ArtistEntry *out, int max_count, int *out_count);
int api_parser_artist_brief_info(const char *json_body, size_t json_size, ArtistBriefInfo *out);

#endif
