#include "services/api_parser.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/logger.h"

#include "cjson/cJSON.h"

#include "app/app_state.h"

static void set_string_field(char *dst, size_t dst_size, const cJSON *node)
{
    if (!dst || dst_size == 0 || !node) {
        return;
    }
    if (cJSON_IsString(node) && node->valuestring) {
        strncpy(dst, node->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

static void set_iso_date_field(char dst[11], const cJSON *node)
{
    const char *value;
    if (!dst || !node || !cJSON_IsString(node) || !node->valuestring) {
        return;
    }
    value = node->valuestring;
    if (strlen(value) < 10 || value[4] != '-' || value[7] != '-') {
        return;
    }
    memcpy(dst, value, 10);
    dst[10] = '\0';
}

int api_parser_account_status(const char *json_body, size_t json_size, UserInfo *info)
{
    cJSON *root = NULL;
    cJSON *result = NULL;
    cJSON *account = NULL;
    int ret = -1;

    if (!info || !json_body || json_size == 0) {
        return -1;
    }

    memset(info, 0, sizeof(UserInfo));

    root = cJSON_Parse(json_body);
    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            logLine("json: account parse failed at: %s\n", error_ptr);
        } else {
            logLine("json: account parse failed (unknown error)\n");
        }
        return -1;
    }

    if (!cJSON_IsObject(root)) {
        logLine("json: account root is not an object\n");
        cJSON_Delete(root);
        return -1;
    }

    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!result || !cJSON_IsObject(result)) {
        logLine("json: result object not found\n");
        cJSON_Delete(root);
        return -1;
    }

    account = cJSON_GetObjectItemCaseSensitive(result, "account");
    if (!account) {
        logLine("json: account object not found\n");
    } else if (!cJSON_IsObject(account)) {
        logLine("json: account is not an object\n");
    }
    if (account && cJSON_IsObject(account)) {
        set_string_field(info->display_name, sizeof(info->display_name),
                         cJSON_GetObjectItemCaseSensitive(account, "displayName"));

        cJSON *uid_node = cJSON_GetObjectItemCaseSensitive(account, "uid");
        if (!uid_node) {
            logLine("json: uid field not found in account\n");
        } else if (!cJSON_IsNumber(uid_node)) {
            logLine("json: uid is not a number (type=%d)\n", uid_node->type);
        }
        if (uid_node && cJSON_IsNumber(uid_node)) {
            info->uid = uid_node->valueint;
            logLine("json: found uid=%d\n", info->uid);
        }
    }

    cJSON *subscription = cJSON_GetObjectItemCaseSensitive(result, "subscription");
    if (subscription && cJSON_IsObject(subscription)) {
        cJSON *auto_renewable = cJSON_GetObjectItemCaseSensitive(subscription, "autoRenewable");
        if (auto_renewable && cJSON_IsArray(auto_renewable) && cJSON_GetArraySize(auto_renewable) > 0) {
            cJSON *first = cJSON_GetArrayItem(auto_renewable, 0);
            if (first && cJSON_IsObject(first)) {
                set_string_field(info->subscription_end, sizeof(info->subscription_end),
                                 cJSON_GetObjectItemCaseSensitive(first, "expires"));
                if (info->subscription_end[0]) {
                    info->subscription_active = 1;
                }
            }
        }
    }

    if (info->uid > 0) {
        logLine("parse: success, uid=%d name='%s'\n", info->uid, info->display_name);
        ret = 0;
    } else {
        logLine("parse: failed to find required fields\n");
        ret = -1;
    }

    cJSON_Delete(root);
    return ret;
}

int api_parser_playlists_list(const char *json_body, size_t json_size,
                              PlaylistEntry **out, int *out_count)
{
    cJSON *root = NULL;
    cJSON *result = NULL;
    cJSON *items = NULL;
    int count_out = 0;
    PlaylistEntry *entries = NULL;

    if (!json_body || json_size == 0 || !out || !out_count) {
        return -1;
    }

    *out = NULL;
    *out_count = 0;

    root = cJSON_Parse(json_body);
    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            logLine("json: playlists parse failed at: %s\n", error_ptr);
        } else {
            logLine("json: playlists parse failed (unknown error)\n");
        }
        return -1;
    }

    // Ищем массив result
    if (cJSON_IsObject(root)) {
        result = cJSON_GetObjectItemCaseSensitive(root, "result");
        if (result && cJSON_IsArray(result)) {
            items = result;
        }
    }

    if (!items || !cJSON_IsArray(items)) {
        logLine("json: playlists array not found\n");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(items);
    logLine("json: playlists array size %d\n", count);
    if (count > 0) {
        entries = (PlaylistEntry *)calloc((size_t)count, sizeof(*entries));
        if (!entries) {
            cJSON_Delete(root);
            return -1;
        }
    }

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        if (!item || !cJSON_IsObject(item)) {
            continue;
        }

        cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        if (!title || !cJSON_IsString(title) || !title->valuestring) {
            continue;
        }

        PlaylistEntry *entry = &entries[count_out++];
        memset(entry, 0, sizeof(PlaylistEntry));

        // Title
        strncpy(entry->title, title->valuestring, sizeof(entry->title) - 1);
        entry->title[sizeof(entry->title) - 1] = '\0';

        // Track count
        cJSON *track_count = cJSON_GetObjectItemCaseSensitive(item, "trackCount");
        if (track_count && cJSON_IsNumber(track_count)) {
            entry->track_count = track_count->valueint;
        } else {
            entry->track_count = -1;
        }

        // Kind (playlist ID)
        cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
        if (kind && cJSON_IsNumber(kind)) {
            entry->playlist_id = kind->valueint;
        } else {
            entry->playlist_id = 0;
        }

        cJSON *revision = cJSON_GetObjectItemCaseSensitive(item, "revision");
        entry->revision = (revision && cJSON_IsNumber(revision)) ? revision->valueint : 0;

        set_iso_date_field(entry->modified_date,
                           cJSON_GetObjectItemCaseSensitive(item, "modified"));

        // playlistUuid
        cJSON *pu = cJSON_GetObjectItemCaseSensitive(item, "playlistUuid");
        if (pu && cJSON_IsString(pu) && pu->valuestring) {
            strncpy(entry->uuid, pu->valuestring, sizeof(entry->uuid) - 1);
            entry->uuid[sizeof(entry->uuid) - 1] = '\0';
        }

        // Cover URI
        cJSON *cover = cJSON_GetObjectItemCaseSensitive(item, "cover");
        if (cover && cJSON_IsObject(cover)) {
            cJSON *uri = cJSON_GetObjectItemCaseSensitive(cover, "uri");
            if (uri && cJSON_IsString(uri) && uri->valuestring) {
                strncpy(entry->cover_uri, uri->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
        }
        
        // Fallback to ogImage if no cover.uri
        if (!entry->cover_uri[0]) {
            cJSON *og_image = cJSON_GetObjectItemCaseSensitive(item, "ogImage");
            if (og_image && cJSON_IsString(og_image) && og_image->valuestring) {
                strncpy(entry->cover_uri, og_image->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
        }

        logLine("json: playlist %d '%s' tracks=%d playlist_id=%d cover=%s\n",
                count_out, entry->title, entry->track_count, entry->playlist_id,
                entry->cover_uri[0] ? entry->cover_uri : "none");
    }

    if (count_out == 0) {
        free(entries);
        entries = NULL;
    } else if (count_out < count) {
        PlaylistEntry *shrunk = (PlaylistEntry *)realloc(
            entries, (size_t)count_out * sizeof(*entries));
        if (shrunk) {
            entries = shrunk;
        }
    }
    *out = entries;
    *out_count = count_out;
    cJSON_Delete(root);
    return 0;
}

/* Парсит ответ GET /users/{uid}/likes/playlists.
 * Формат отличается от playlists_list: каждый элемент result — обёртка
 * {"playlist": {...}, "timestamp": "..."}, а поле uid внутри playlist —
 * это uid владельца (может отличаться от текущего пользователя). */
int api_parser_liked_playlists(const char *json_body, size_t json_size,
                               PlaylistEntry **out, int *out_count)
{
    cJSON *root = NULL;
    cJSON *result = NULL;
    cJSON *items = NULL;
    int count_out = 0;
    PlaylistEntry *entries = NULL;

    if (!json_body || json_size == 0 || !out || !out_count) {
        return -1;
    }

    *out = NULL;
    *out_count = 0;

    root = cJSON_Parse(json_body);
    if (!root) {
        logLine("json: liked_playlists parse failed\n");
        return -1;
    }

    if (cJSON_IsObject(root)) {
        result = cJSON_GetObjectItemCaseSensitive(root, "result");
        if (result && cJSON_IsArray(result)) {
            items = result;
        }
    }

    if (!items || !cJSON_IsArray(items)) {
        logLine("json: liked_playlists array not found\n");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(items);
    logLine("json: liked_playlists array size %d\n", count);
    if (count > 0) {
        entries = (PlaylistEntry *)calloc((size_t)count, sizeof(*entries));
        if (!entries) {
            cJSON_Delete(root);
            return -1;
        }
    }

    for (int i = 0; i < count; i++) {
        cJSON *wrapper = cJSON_GetArrayItem(items, i);
        if (!wrapper || !cJSON_IsObject(wrapper)) {
            continue;
        }

        /* Unwrap "playlist" key — отличие от playlists_list */
        cJSON *item = cJSON_GetObjectItemCaseSensitive(wrapper, "playlist");
        if (!item || !cJSON_IsObject(item)) {
            continue;
        }

        cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        if (!title || !cJSON_IsString(title) || !title->valuestring) {
            continue;
        }

        PlaylistEntry *entry = &entries[count_out++];
        memset(entry, 0, sizeof(PlaylistEntry));

        strncpy(entry->title, title->valuestring, sizeof(entry->title) - 1);
        entry->title[sizeof(entry->title) - 1] = '\0';

        cJSON *track_count = cJSON_GetObjectItemCaseSensitive(item, "trackCount");
        if (track_count && cJSON_IsNumber(track_count)) {
            entry->track_count = track_count->valueint;
        } else {
            entry->track_count = -1;
        }

        cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
        if (kind && cJSON_IsNumber(kind)) {
            entry->playlist_id = kind->valueint;
        }

        cJSON *revision = cJSON_GetObjectItemCaseSensitive(item, "revision");
        entry->revision = (revision && cJSON_IsNumber(revision)) ? revision->valueint : 0;

        set_iso_date_field(entry->modified_date,
                           cJSON_GetObjectItemCaseSensitive(item, "modified"));

        // playlistUuid
        cJSON *pu = cJSON_GetObjectItemCaseSensitive(item, "playlistUuid");
        if (pu && cJSON_IsString(pu) && pu->valuestring) {
            strncpy(entry->uuid, pu->valuestring, sizeof(entry->uuid) - 1);
            entry->uuid[sizeof(entry->uuid) - 1] = '\0';
        }

        /* owner_uid: uid владельца плейлиста */
        cJSON *uid = cJSON_GetObjectItemCaseSensitive(item, "uid");
        if (uid && cJSON_IsNumber(uid)) {
            entry->owner_uid = uid->valueint;
        }

        {
            cJSON *owner = cJSON_GetObjectItemCaseSensitive(item, "owner");
            if (owner && cJSON_IsObject(owner)) {
                set_string_field(entry->owner_name, sizeof(entry->owner_name),
                                 cJSON_GetObjectItemCaseSensitive(owner, "name"));
            }
        }

        cJSON *cover = cJSON_GetObjectItemCaseSensitive(item, "cover");
        if (cover && cJSON_IsObject(cover)) {
            cJSON *uri = cJSON_GetObjectItemCaseSensitive(cover, "uri");
            if (uri && cJSON_IsString(uri) && uri->valuestring) {
                strncpy(entry->cover_uri, uri->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
        }

        if (!entry->cover_uri[0]) {
            cJSON *og_image = cJSON_GetObjectItemCaseSensitive(item, "ogImage");
            if (og_image && cJSON_IsString(og_image) && og_image->valuestring) {
                strncpy(entry->cover_uri, og_image->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
        }

        logLine("json: liked_playlist %d '%s' tracks=%d kind=%d owner_uid=%d\n",
                count_out, entry->title, entry->track_count, entry->playlist_id, entry->owner_uid);
    }

    if (count_out == 0) {
        free(entries);
        entries = NULL;
    } else if (count_out < count) {
        PlaylistEntry *shrunk = (PlaylistEntry *)realloc(
            entries, (size_t)count_out * sizeof(*entries));
        if (shrunk) {
            entries = shrunk;
        }
    }
    *out = entries;
    *out_count = count_out;
    cJSON_Delete(root);
    return 0;
}

/* Парсит ответ GET /users/{uid}/likes/artists?with-timestamps=True.
 * Формат: {"result": [{"artist": {...}, "timestamp": "..."}, ...]}
 * artist: {id, name, cover.uri, ogImage, genres[], counts:{tracks, directAlbums}} */
int api_parser_liked_artists(const char *json_body, size_t json_size, ArtistEntry *out, int max_count, int *out_count)
{
    cJSON *root = NULL;
    cJSON *result = NULL;
    int count_out = 0;
    int ret = -1;

    if (!json_body || json_size == 0 || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;

    root = cJSON_Parse(json_body);
    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            logLine("json: liked_artists parse failed at: %s\n", error_ptr);
        } else {
            logLine("json: liked_artists parse failed (unknown error)\n");
        }
        return -1;
    }

    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!result || !cJSON_IsArray(result)) {
        logLine("json: liked_artists result array not found\n");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(result);
    logLine("json: liked_artists array size %d\n", count);

    for (int i = 0; i < count && count_out < max_count; i++) {
        cJSON *wrapper = cJSON_GetArrayItem(result, i);
        if (!wrapper || !cJSON_IsObject(wrapper)) {
            continue;
        }
        cJSON *artist = cJSON_GetObjectItemCaseSensitive(wrapper, "artist");
        if (!artist || !cJSON_IsObject(artist)) {
            continue;
        }

        cJSON *name = cJSON_GetObjectItemCaseSensitive(artist, "name");
        if (!name || !cJSON_IsString(name) || !name->valuestring) {
            continue;
        }

        ArtistEntry *entry = &out[count_out++];
        memset(entry, 0, sizeof(ArtistEntry));

        /* name */
        strncpy(entry->name, name->valuestring, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';

        /* id (string) + numeric artist_id */
        cJSON *id = cJSON_GetObjectItemCaseSensitive(artist, "id");
        if (id && cJSON_IsNumber(id)) {
            entry->artist_id = (int)id->valuedouble;
            snprintf(entry->id, sizeof(entry->id), "%d", entry->artist_id);
        } else if (id && cJSON_IsString(id) && id->valuestring) {
            strncpy(entry->id, id->valuestring, sizeof(entry->id) - 1);
            entry->id[sizeof(entry->id) - 1] = '\0';
            entry->artist_id = (int)atoi(entry->id);
        }

        /* cover.uri */
        cJSON *cover = cJSON_GetObjectItemCaseSensitive(artist, "cover");
        if (cover && cJSON_IsObject(cover)) {
            cJSON *uri = cJSON_GetObjectItemCaseSensitive(cover, "uri");
            if (uri && cJSON_IsString(uri) && uri->valuestring) {
                strncpy(entry->cover_uri, uri->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
        }
        if (!entry->cover_uri[0]) {
            cJSON *og = cJSON_GetObjectItemCaseSensitive(artist, "ogImage");
            if (og && cJSON_IsString(og) && og->valuestring) {
                strncpy(entry->cover_uri, og->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
        }

        /* counts */
        cJSON *counts = cJSON_GetObjectItemCaseSensitive(artist, "counts");
        if (counts && cJSON_IsObject(counts)) {
            cJSON *tracks = cJSON_GetObjectItemCaseSensitive(counts, "tracks");
            if (tracks && cJSON_IsNumber(tracks)) {
                entry->track_count = tracks->valueint;
            }
            cJSON *albums = cJSON_GetObjectItemCaseSensitive(counts, "directAlbums");
            if (albums && cJSON_IsNumber(albums)) {
                entry->album_count = albums->valueint;
            }
        }

        /* genres[0] */
        cJSON *genres = cJSON_GetObjectItemCaseSensitive(artist, "genres");
        if (genres && cJSON_IsArray(genres)) {
            cJSON *g0 = cJSON_GetArrayItem(genres, 0);
            if (g0 && cJSON_IsString(g0) && g0->valuestring) {
                strncpy(entry->genre, g0->valuestring, sizeof(entry->genre) - 1);
                entry->genre[sizeof(entry->genre) - 1] = '\0';
            }
        }

        logLine("json: artist %d '%s' id=%d albums=%d tracks=%d\n",
                count_out, entry->name, entry->artist_id, entry->album_count, entry->track_count);
    }

    *out_count = count_out;
    cJSON_Delete(root);

    if (count_out > 0) {
        ret = 0;
    }

    return ret;
}

int api_parser_artist_brief_info(const char *json_body, size_t json_size, ArtistBriefInfo *out)
{
    if (!json_body || json_size == 0 || !out) {
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(json_body, json_size);
    if (!root) {
        return -1;
    }

    int ret = -1;

    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!result) {
        cJSON_Delete(root);
        return -1;
    }

    /* artist.id */
    cJSON *artist = cJSON_GetObjectItemCaseSensitive(result, "artist");
    if (artist) {
        cJSON *id_node = cJSON_GetObjectItemCaseSensitive(artist, "id");
        if (cJSON_IsString(id_node) && id_node->valuestring) {
            out->artist_id = atoi(id_node->valuestring);
        } else if (cJSON_IsNumber(id_node)) {
            out->artist_id = (int)id_node->valuedouble;
        }
    }

    /* artist.counts */
    if (artist) {
        cJSON *counts = cJSON_GetObjectItemCaseSensitive(artist, "counts");
        if (counts) {
            cJSON *ct = cJSON_GetObjectItemCaseSensitive(counts, "tracks");
            if (cJSON_IsNumber(ct)) {
                out->count_tracks = (int)ct->valuedouble;
            }
            cJSON *cda = cJSON_GetObjectItemCaseSensitive(counts, "directAlbums");
            if (cJSON_IsNumber(cda)) {
                out->count_direct_albums = (int)cda->valuedouble;
            }
        }
    }

    /* albums[] */
    out->album_count = 0;
    cJSON *albums = cJSON_GetObjectItemCaseSensitive(result, "albums");
    if (cJSON_IsArray(albums)) {
        cJSON *alb = NULL;
        cJSON_ArrayForEach(alb, albums) {
            if (out->album_count >= ARTIST_MAX_ALBUMS) {
                break;
            }
            ArtistAlbumEntry *entry = &out->albums[out->album_count];
            cJSON *alb_id = cJSON_GetObjectItemCaseSensitive(alb, "id");
            if (cJSON_IsNumber(alb_id)) {
                entry->album_id = (int)alb_id->valuedouble;
            }
            cJSON *t = cJSON_GetObjectItemCaseSensitive(alb, "title");
            if (cJSON_IsString(t) && t->valuestring) {
                strncpy(entry->title, t->valuestring, sizeof(entry->title) - 1);
                entry->title[sizeof(entry->title) - 1] = '\0';
            }
            cJSON *v = cJSON_GetObjectItemCaseSensitive(alb, "version");
            if (cJSON_IsString(v) && v->valuestring) {
                strncpy(entry->version, v->valuestring, sizeof(entry->version) - 1);
                entry->version[sizeof(entry->version) - 1] = '\0';
            }
            cJSON *yr = cJSON_GetObjectItemCaseSensitive(alb, "year");
            if (cJSON_IsNumber(yr)) {
                entry->year = (int)yr->valuedouble;
            }
            cJSON *cu = cJSON_GetObjectItemCaseSensitive(alb, "coverUri");
            if (cJSON_IsString(cu) && cu->valuestring) {
                strncpy(entry->cover_uri, cu->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
            cJSON *tc = cJSON_GetObjectItemCaseSensitive(alb, "trackCount");
            if (cJSON_IsNumber(tc)) {
                entry->track_count = (int)tc->valuedouble;
            }
            out->album_count++;
        }
    }

    /* alsoAlbums[] */
    out->also_album_count = 0;
    cJSON *also_albums = cJSON_GetObjectItemCaseSensitive(result, "alsoAlbums");
    if (cJSON_IsArray(also_albums)) {
        cJSON *alb = NULL;
        cJSON_ArrayForEach(alb, also_albums) {
            if (out->also_album_count >= ARTIST_MAX_ALSO_ALBUMS) {
                break;
            }
            ArtistAlbumEntry *entry = &out->also_albums[out->also_album_count];
            cJSON *alb_id = cJSON_GetObjectItemCaseSensitive(alb, "id");
            if (cJSON_IsNumber(alb_id)) {
                entry->album_id = (int)alb_id->valuedouble;
            }
            cJSON *t = cJSON_GetObjectItemCaseSensitive(alb, "title");
            if (cJSON_IsString(t) && t->valuestring) {
                strncpy(entry->title, t->valuestring, sizeof(entry->title) - 1);
                entry->title[sizeof(entry->title) - 1] = '\0';
            }
            cJSON *v = cJSON_GetObjectItemCaseSensitive(alb, "version");
            if (cJSON_IsString(v) && v->valuestring) {
                strncpy(entry->version, v->valuestring, sizeof(entry->version) - 1);
                entry->version[sizeof(entry->version) - 1] = '\0';
            }
            cJSON *yr = cJSON_GetObjectItemCaseSensitive(alb, "year");
            if (cJSON_IsNumber(yr)) {
                entry->year = (int)yr->valuedouble;
            }
            cJSON *cu = cJSON_GetObjectItemCaseSensitive(alb, "coverUri");
            if (cJSON_IsString(cu) && cu->valuestring) {
                strncpy(entry->cover_uri, cu->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
            cJSON *tc = cJSON_GetObjectItemCaseSensitive(alb, "trackCount");
            if (cJSON_IsNumber(tc)) {
                entry->track_count = (int)tc->valuedouble;
            }
            out->also_album_count++;
        }
    }

    /* popularTracks[] */
    out->popular_track_count = 0;
    cJSON *tracks = cJSON_GetObjectItemCaseSensitive(result, "popularTracks");
    if (cJSON_IsArray(tracks)) {
        cJSON *tr = NULL;
        cJSON_ArrayForEach(tr, tracks) {
            if (out->popular_track_count >= ARTIST_MAX_POPULAR_TRACKS) {
                break;
            }
            ArtistBriefTrack *entry = &out->popular_tracks[out->popular_track_count];
            cJSON *tr_id = cJSON_GetObjectItemCaseSensitive(tr, "id");
            if (cJSON_IsString(tr_id) && tr_id->valuestring) {
                entry->track_id = atoi(tr_id->valuestring);
            } else if (cJSON_IsNumber(tr_id)) {
                entry->track_id = (int)tr_id->valuedouble;
            }
            cJSON *tt = cJSON_GetObjectItemCaseSensitive(tr, "title");
            if (cJSON_IsString(tt) && tt->valuestring) {
                strncpy(entry->title, tt->valuestring, sizeof(entry->title) - 1);
                entry->title[sizeof(entry->title) - 1] = '\0';
            }
            cJSON *dur = cJSON_GetObjectItemCaseSensitive(tr, "durationMs");
            if (cJSON_IsNumber(dur)) {
                entry->duration_ms = (int)dur->valuedouble;
            }
            cJSON *cu = cJSON_GetObjectItemCaseSensitive(tr, "coverUri");
            if (cJSON_IsString(cu) && cu->valuestring) {
                strncpy(entry->cover_uri, cu->valuestring, sizeof(entry->cover_uri) - 1);
                entry->cover_uri[sizeof(entry->cover_uri) - 1] = '\0';
            }
            out->popular_track_count++;
        }
    }

    ret = (out->artist_id != 0) ? 0 : -1;
    cJSON_Delete(root);
    return ret;
}
