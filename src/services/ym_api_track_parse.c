/* Shared full-track parser. See ym_api_track_parse.h.
 * Moved from ym_api_playlists.c (hydrate path) with the documented fixes. */
#include "services/ym_api_track_parse.h"
#include "services/ym_api_genres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/logger.h"

int ym_api_parse_track_id_node(const cJSON *id_node, char *out, size_t out_size)
{
    int written;

    if (!id_node || !out || out_size == 0) {
        return -1;
    }
    if (cJSON_IsString(id_node) && id_node->valuestring && id_node->valuestring[0]) {
        if (strlen(id_node->valuestring) >= out_size) {
            return -1;
        }
        snprintf(out, out_size, "%s", id_node->valuestring);
        return 0;
    }
    if (cJSON_IsNumber(id_node)) {
        written = snprintf(out, out_size, "%.0f", id_node->valuedouble);
        if (written <= 0 || (size_t)written >= out_size) {
            return -1;
        }
        return 0;
    }
    return -1;
}

int ym_api_parse_track_cutout_uri(cJSON *track_obj, char *out, size_t out_size)
{
    cJSON *artists_node;
    int artist_count;
    int artist_index;

    if (!track_obj || !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    artists_node = cJSON_GetObjectItemCaseSensitive(track_obj, "artists");
    if (!cJSON_IsArray(artists_node)) {
        return 1;
    }
    artist_count = cJSON_GetArraySize(artists_node);
    for (artist_index = 0; artist_index < artist_count; ++artist_index) {
        cJSON *artist = cJSON_GetArrayItem(artists_node, artist_index);
        cJSON *cutout_node;
        cJSON *uri_node;

        if (!cJSON_IsObject(artist)) {
            continue;
        }
        cutout_node = cJSON_GetObjectItemCaseSensitive(artist, "cutoutCover");
        if (!cJSON_IsObject(cutout_node)) {
            continue;
        }
        uri_node = cJSON_GetObjectItemCaseSensitive(cutout_node, "uri");
        if (cJSON_IsString(uri_node) && uri_node->valuestring && uri_node->valuestring[0]) {
            if (strlen(uri_node->valuestring) >= out_size) {
                return -1;
            }
            snprintf(out, out_size, "%s", uri_node->valuestring);
            return 0;
        }
    }
    return 1;
}

int ym_api_parse_track_from_object_for_album(cJSON *track_obj,
                                              int preferred_album_id,
                                              TrackEntry *entry)
{
    cJSON *error_node;
    cJSON *title_node;
    cJSON *artists_node;
    cJSON *duration_node;
    cJSON *id_node;
    cJSON *albums_node;
    cJSON *cover_node;
    cJSON *version_node;
    cJSON *background_video_node;
    cJSON *genre_node;
    cJSON *year_node;
    cJSON *explicit_node;
    cJSON *available_node;
    int no_rights = 0;

    if (!track_obj || !entry) {
        return -1;
    }
    if (!cJSON_IsObject(track_obj)) {
        return -1;
    }

    /* A rights-blocked item is still a resolved playlist position. The API
     * supplies its id/title/artists but may omit duration and albums, so parse
     * the available metadata and mark the entry as non-playable. */
    error_node = cJSON_GetObjectItemCaseSensitive(track_obj, "error");
    if (error_node && cJSON_IsString(error_node) && error_node->valuestring) {
        if (strcmp(error_node->valuestring, "no-rights") == 0) {
            no_rights = 1;
        }
    }

    memset(entry, 0, sizeof(TrackEntry));
    entry->available = 1;

    title_node = cJSON_GetObjectItemCaseSensitive(track_obj, "title");
    if (title_node && cJSON_IsString(title_node) && title_node->valuestring) {
        strncpy(entry->title, title_node->valuestring, sizeof(entry->title) - 1);
        entry->title[sizeof(entry->title) - 1] = '\0';
    }

    artists_node = cJSON_GetObjectItemCaseSensitive(track_obj, "artists");
    if (artists_node && cJSON_IsArray(artists_node) && cJSON_GetArraySize(artists_node) > 0) {
        int artist_count = cJSON_GetArraySize(artists_node);
        int artist_index;
        size_t used = 0;

        for (artist_index = 0; artist_index < artist_count; ++artist_index) {
            cJSON *artist = cJSON_GetArrayItem(artists_node, artist_index);
            cJSON *name_node = artist ? cJSON_GetObjectItemCaseSensitive(artist, "name") : NULL;
            if (name_node && cJSON_IsString(name_node) && name_node->valuestring) {
                const char *separator = used > 0 ? ", " : "";
                size_t remaining = sizeof(entry->artist) - used;
                int written = snprintf(entry->artist + used, remaining, "%s%s",
                                       separator, name_node->valuestring);
                if (written < 0) {
                    break;
                }
                if ((size_t)written >= remaining) {
                    entry->artist[sizeof(entry->artist) - 1] = '\0';
                    logLine("ym_track_parse: artist list truncated\n");
                    break;
                }
                used += (size_t)written;
            }
        }
    }

    duration_node = cJSON_GetObjectItemCaseSensitive(track_obj, "durationMs");
    if (duration_node && cJSON_IsNumber(duration_node)) {
        entry->duration_ms = (int)duration_node->valuedouble;
    }

    id_node = cJSON_GetObjectItemCaseSensitive(track_obj, "id");
    if (ym_api_parse_track_id_node(id_node, entry->id, sizeof(entry->id)) != 0) {
        return 1;
    }

    genre_node = NULL;
    albums_node = cJSON_GetObjectItemCaseSensitive(track_obj, "albums");
    if (albums_node && cJSON_IsArray(albums_node) && cJSON_GetArraySize(albums_node) > 0) {
        cJSON *first_album = NULL;
        int album_count = cJSON_GetArraySize(albums_node);
        int album_index;

        if (preferred_album_id > 0) {
            for (album_index = 0; album_index < album_count; ++album_index) {
                cJSON *candidate = cJSON_GetArrayItem(albums_node, album_index);
                cJSON *candidate_id = candidate
                    ? cJSON_GetObjectItemCaseSensitive(candidate, "id") : NULL;
                int value = 0;
                if (cJSON_IsNumber(candidate_id)) {
                    value = candidate_id->valueint;
                } else if (cJSON_IsString(candidate_id) &&
                           candidate_id->valuestring) {
                    value = atoi(candidate_id->valuestring);
                }
                if (value == preferred_album_id) {
                    first_album = candidate;
                    break;
                }
            }
            if (!first_album) {
                logLine("ym_track_parse: track %s lacks requested album %d\n",
                        entry->id, preferred_album_id);
                return 1;
            }
        } else {
            first_album = cJSON_GetArrayItem(albums_node, 0);
        }
        if (first_album) {
            genre_node = cJSON_GetObjectItemCaseSensitive(first_album, "genre");
            cJSON *album_title_node = cJSON_GetObjectItemCaseSensitive(first_album, "title");
            if (album_title_node && cJSON_IsString(album_title_node) && album_title_node->valuestring) {
                strncpy(entry->album, album_title_node->valuestring, sizeof(entry->album) - 1);
                entry->album[sizeof(entry->album) - 1] = '\0';
            }
            cJSON *album_version_node = cJSON_GetObjectItemCaseSensitive(first_album, "version");
            if (album_version_node && cJSON_IsString(album_version_node) &&
                album_version_node->valuestring) {
                strncpy(entry->album_version, album_version_node->valuestring,
                        sizeof(entry->album_version) - 1);
                entry->album_version[sizeof(entry->album_version) - 1] = '\0';
            }
            cJSON *album_id_node = cJSON_GetObjectItemCaseSensitive(first_album, "id");
            if (album_id_node && cJSON_IsNumber(album_id_node)) {
                entry->album_id = album_id_node->valueint;
            } else if (album_id_node && cJSON_IsString(album_id_node) &&
                       album_id_node->valuestring) {
                entry->album_id = atoi(album_id_node->valuestring);
            }

            cJSON *album_cover_node = cJSON_GetObjectItemCaseSensitive(first_album, "coverUri");
            if (album_cover_node && cJSON_IsString(album_cover_node) && album_cover_node->valuestring) {
                strncpy(entry->cover_uri, album_cover_node->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
        }
    }

    cover_node = cJSON_GetObjectItemCaseSensitive(track_obj, "coverUri");
    if (preferred_album_id == 0 && cover_node && cJSON_IsString(cover_node) &&
        cover_node->valuestring) {
        entry->cover_uri[0] = '\0';
        strncpy(entry->cover_uri, cover_node->valuestring, sizeof(entry->cover_uri) - 1);
        entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
    }

    version_node = cJSON_GetObjectItemCaseSensitive(track_obj, "version");
    background_video_node = cJSON_GetObjectItemCaseSensitive(track_obj, "backgroundVideoUri");
    if (background_video_node && cJSON_IsString(background_video_node) &&
        background_video_node->valuestring) {
        strncpy(entry->background_video_uri,
                background_video_node->valuestring,
                sizeof(entry->background_video_uri) - 1);
        entry->background_video_uri[sizeof(entry->background_video_uri) - 1] = '\0';
    }

    if (version_node && cJSON_IsString(version_node) && version_node->valuestring) {
        strncpy(entry->version, version_node->valuestring, sizeof(entry->version) - 1);
        entry->version[sizeof(entry->version) - 1] = '\0';
    }

    if (genre_node && cJSON_IsString(genre_node) && genre_node->valuestring) {
        ym_api_genres_resolve(genre_node->valuestring,
                              entry->genre, sizeof(entry->genre));
    }

    year_node = cJSON_GetObjectItemCaseSensitive(track_obj, "year");
    if (year_node && cJSON_IsNumber(year_node)) {
        entry->year = year_node->valueint;
    }

    explicit_node = cJSON_GetObjectItemCaseSensitive(track_obj, "contentWarning");
    if (explicit_node && cJSON_IsString(explicit_node) && explicit_node->valuestring) {
        entry->explicit_content = (strcmp(explicit_node->valuestring, "explicit") == 0) ? 1 : 0;
    }

    available_node = cJSON_GetObjectItemCaseSensitive(track_obj, "available");
    if (cJSON_IsTrue(available_node)) {
        entry->available = 1;
    } else if (cJSON_IsFalse(available_node)) {
        entry->available = 0;
    } else if (available_node && cJSON_IsNumber(available_node)) {
        entry->available = (available_node->valueint != 0) ? 1 : 0;
    }
    if (no_rights) {
        entry->available = 0;
    }

    return 0;
}

int ym_api_parse_track_from_object(cJSON *track_obj, TrackEntry *entry)
{
    return ym_api_parse_track_from_object_for_album(track_obj, 0, entry);
}
