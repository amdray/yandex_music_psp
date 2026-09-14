#include "services/ym_api.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/api_parser.h"
#include "services/net_http.h"

/* Load-failure cause reported via *out_status (see ym_api.h): HTTP status when
   the server answered non-200; NET_LOAD_ERR_TRANSPORT when no HTTP response;
   NET_LOAD_ERR_DATA when the response was 200 but empty. */
static int ym_api_request_and_log(const HttpRequest *request,
                                  const char *log_path,
                                  NetHttpResponse *response,
                                  int *out_status)
{
    int ret;

    if (out_status) {
        *out_status = NET_LOAD_ERR_TRANSPORT;
    }
    memset(response, 0, sizeof(*response));
    ret = http_request(request, response);
    if (ret != 0) {
        logLine("ym_api_playlists: request failed ret=%d\n", ret);
        return -1;
    }

    logLine("ym_api_playlists: request ok status=%d body=%d\n",
            response->status_code, response->body_size);
    if (response->status_code != 200 || !response->body || response->body_size <= 0) {
        logLine("ym_api_playlists: non-200 or empty status=%d\n", response->status_code);
        if (out_status) {
            *out_status = (response->status_code == 200)
                              ? NET_LOAD_ERR_DATA
                              : response->status_code;
        }
        net_http_response_free(response);
        return -1;
    }

    if (log_path) {
        fs_ensure_dir(log_path);
        SceUID fd = fs_open(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            fs_write(fd, response->body, response->body_size);
            fs_close(fd);
            logLine("ym_api_playlists: saved %s (%d bytes)\n", log_path, response->body_size);
        }
    }

    return 0;
}

int ym_api_playlists_list(YmApiContext *ctx,
                          int uid,
                          PlaylistEntry **out,
                          int *out_count,
                          int *out_status)
{
    NetHttpResponse response;
    char url[256];
    int ret = -1;
    int status = NET_LOAD_ERR_INTERNAL;

    if (out_status) {
        *out_status = NET_LOAD_ERR_INTERNAL;
    }
    if (!ctx || !ctx->oauth_token || uid <= 0 || !out || !out_count) {
        return -1;
    }

    *out = NULL;
    *out_count = 0;
    logLine("ym_api_playlists: fetch playlists list start (uid=%d)\n", uid);
    snprintf(url, sizeof(url), "https://api.music.yandex.net/users/%d/playlists/list", uid);
    url[sizeof(url) - 1] = '\0';

    if (ym_api_request_and_log(&(HttpRequest){
            .method = HTTP_GET,
            .url = url,
            .token = ctx->oauth_token,
            .initial_buffer = HTTP_TLS_BUFFER_MEDIUM
        }, "data/logs/playlists_list_response.json", &response, &status) != 0) {
        if (out_status) {
            *out_status = status;
        }
        return -1;
    }

    {
        int maxFree = sceKernelMaxFreeMemSize();
        logLine("mem: after http get playlists (body_size=%d) max_free=%d\n",
                response.body_size, maxFree);
    }

    ret = (api_parser_playlists_list(response.body, (size_t)response.body_size,
                                     out, out_count) == 0) ? 0 : -1;
    if (ret == 0) {
        logLine("ym_api_playlists: parsed %d playlists\n", *out_count);
        if (out_status) {
            *out_status = 0;
        }
    } else {
        logLine("ym_api_playlists: failed to parse playlists\n");
        if (out_status) {
            *out_status = NET_LOAD_ERR_DATA;
        }
    }

    net_http_response_free(&response);
    return ret;
}

int ym_api_playlist_metadata(YmApiContext *ctx,
                             int uid,
                             int playlist_kind,
                             PlaylistEntry *out,
                             int *out_status)
{
    static const char url[] = "https://api.music.yandex.net/playlists/list";
    NetHttpResponse response;
    PlaylistEntry *entries = NULL;
    char payload[64];
    int count = 0;
    int payload_len;
    int status = NET_LOAD_ERR_INTERNAL;
    int ret = -1;

    if (out_status) {
        *out_status = NET_LOAD_ERR_INTERNAL;
    }
    if (!ctx || !ctx->oauth_token || uid <= 0 || playlist_kind <= 0 || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    payload_len = snprintf(payload, sizeof(payload),
                           "playlist-ids=%d%%3A%d", uid, playlist_kind);
    if (payload_len < 0 || (size_t)payload_len >= sizeof(payload)) {
        return -1;
    }

    logLine("ym_api_playlists: fetch metadata uid=%d kind=%d\n",
            uid, playlist_kind);
    if (ym_api_request_and_log(&(HttpRequest){
            .method = HTTP_POST,
            .url = url,
            .token = ctx->oauth_token,
            .content_type = "application/x-www-form-urlencoded",
            .payload = payload,
            .payload_size = payload_len,
            .initial_buffer = HTTP_TLS_BUFFER_SMALL
        }, NULL, &response, &status) != 0) {
        if (out_status) {
            *out_status = status;
        }
        return -1;
    }

    if (api_parser_playlists_list(response.body, (size_t)response.body_size,
                                  &entries, &count) == 0 &&
        count == 1 && entries[0].playlist_id == playlist_kind) {
        *out = entries[0];
        ret = 0;
        status = 0;
    } else {
        status = NET_LOAD_ERR_DATA;
        logLine("ym_api_playlists: invalid metadata response kind=%d count=%d\n",
                playlist_kind, count);
    }

    free(entries);
    net_http_response_free(&response);
    if (out_status) {
        *out_status = status;
    }
    return ret;
}

int ym_api_liked_playlists(YmApiContext *ctx,
                           int uid,
                           PlaylistEntry **out,
                           int *out_count,
                           int *out_status)
{
    NetHttpResponse response;
    char url[256];
    int ret = -1;
    int status = NET_LOAD_ERR_INTERNAL;

    if (out_status) {
        *out_status = NET_LOAD_ERR_INTERNAL;
    }
    if (!ctx || !ctx->oauth_token || uid <= 0 || !out || !out_count) {
        return -1;
    }

    *out = NULL;
    *out_count = 0;
    logLine("ym_api_playlists: fetch liked playlists start (uid=%d)\n", uid);
    snprintf(url, sizeof(url), "https://api.music.yandex.net/users/%d/likes/playlists", uid);
    url[sizeof(url) - 1] = '\0';

    if (ym_api_request_and_log(&(HttpRequest){
            .method = HTTP_GET,
            .url = url,
            .token = ctx->oauth_token,
            .initial_buffer = HTTP_TLS_BUFFER_MEDIUM
        }, "data/logs/liked_playlists_response.json", &response, &status) != 0) {
        if (out_status) {
            *out_status = status;
        }
        return -1;
    }

    ret = (api_parser_liked_playlists(response.body, (size_t)response.body_size,
                                      out, out_count) == 0) ? 0 : -1;
    if (ret == 0) {
        logLine("ym_api_playlists: parsed %d liked playlists\n", *out_count);
        if (out_status) {
            *out_status = 0;
        }
    } else {
        logLine("ym_api_playlists: failed to parse liked playlists\n");
        if (out_status) {
            *out_status = NET_LOAD_ERR_DATA;
        }
    }

    net_http_response_free(&response);
    return ret;
}

int ym_api_playlist_tracks_build_url(int uid,
                                     int playlist_kind,
                                     char *out,
                                     size_t out_size)
{
    int url_len;

    if (uid <= 0 || playlist_kind <= 0 || !out || out_size == 0) {
        return -1;
    }

    url_len = snprintf(out, out_size,
                       "https://api.music.yandex.net/users/%d/playlists/%d?rich-tracks=false",
                       uid, playlist_kind);
    if (url_len < 0 || (size_t)url_len >= out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return -1;
    }

    return 0;
}

static int ym_api_parse_track_from_object(cJSON *track_obj, TrackEntry *entry)
{
    if (!track_obj || !entry) {
        return -1;
    }

    /* API marks region/rights-blocked tracks with "error":"no-rights" and omits
     * duration/albums for them; we skip such tracks, so the final parsed count
     * can legitimately be lower than the count reported by the API response. */
    cJSON *error_node = cJSON_GetObjectItem(track_obj, "error");
    if (error_node && cJSON_IsString(error_node) && error_node->valuestring) {
        if (strcmp(error_node->valuestring, "no-rights") == 0) {
            return 1;
        }
    }

    memset(entry, 0, sizeof(TrackEntry));

    cJSON *title_node = cJSON_GetObjectItem(track_obj, "title");
    if (title_node && cJSON_IsString(title_node) && title_node->valuestring) {
        strncpy(entry->title, title_node->valuestring, sizeof(entry->title) - 1);
        entry->title[sizeof(entry->title) - 1] = '\0';
    }

    cJSON *artists_node = cJSON_GetObjectItem(track_obj, "artists");
    if (artists_node && cJSON_IsArray(artists_node) && cJSON_GetArraySize(artists_node) > 0) {
        int artist_count = cJSON_GetArraySize(artists_node);
        int artist_index;
        size_t used = 0;

        for (artist_index = 0; artist_index < artist_count; ++artist_index) {
            cJSON *artist = cJSON_GetArrayItem(artists_node, artist_index);
            cJSON *name_node = artist ? cJSON_GetObjectItem(artist, "name") : NULL;
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
                    logLine("ym_api_playlists: artist list truncated\n");
                    break;
                }
                used += (size_t)written;
            }
        }
    }

    cJSON *duration_node = cJSON_GetObjectItem(track_obj, "durationMs");
    if (duration_node && cJSON_IsNumber(duration_node)) {
        entry->duration_ms = (int)duration_node->valuedouble;
    }

    cJSON *id_node = cJSON_GetObjectItem(track_obj, "id");
    if (id_node && cJSON_IsString(id_node) && id_node->valuestring) {
        strncat(entry->id, id_node->valuestring, sizeof(entry->id) - 1);
    }

    cJSON *albums_node = cJSON_GetObjectItem(track_obj, "albums");
    if (albums_node && cJSON_IsArray(albums_node) && cJSON_GetArraySize(albums_node) > 0) {
        cJSON *first_album = cJSON_GetArrayItem(albums_node, 0);
        if (first_album) {
            cJSON *album_title_node = cJSON_GetObjectItem(first_album, "title");
            if (album_title_node && cJSON_IsString(album_title_node) && album_title_node->valuestring) {
                strncpy(entry->album, album_title_node->valuestring, sizeof(entry->album) - 1);
                entry->album[sizeof(entry->album) - 1] = '\0';
            }
            cJSON *album_version_node = cJSON_GetObjectItem(first_album, "version");
            if (album_version_node && cJSON_IsString(album_version_node) &&
                album_version_node->valuestring) {
                strncpy(entry->album_version, album_version_node->valuestring,
                        sizeof(entry->album_version) - 1);
                entry->album_version[sizeof(entry->album_version) - 1] = '\0';
            }
            cJSON *album_id_node = cJSON_GetObjectItem(first_album, "id");
            if (album_id_node && cJSON_IsNumber(album_id_node)) {
                entry->album_id = album_id_node->valueint;
            }

            cJSON *album_cover_node = cJSON_GetObjectItem(first_album, "coverUri");
            if (album_cover_node && cJSON_IsString(album_cover_node) && album_cover_node->valuestring) {
                strncat(entry->cover_uri, album_cover_node->valuestring, sizeof(entry->cover_uri) - 1);
            }
        }
    }

    cJSON *cover_node = cJSON_GetObjectItem(track_obj, "coverUri");
    if (cover_node && cJSON_IsString(cover_node) && cover_node->valuestring) {
        entry->cover_uri[0] = '\0';
        strncat(entry->cover_uri, cover_node->valuestring, sizeof(entry->cover_uri) - 1);
    }

    cJSON *version_node = cJSON_GetObjectItem(track_obj, "version");
    cJSON *background_video_node = cJSON_GetObjectItem(track_obj, "backgroundVideoUri");
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

    cJSON *genre_node = cJSON_GetObjectItem(track_obj, "genre");
    if (genre_node && cJSON_IsString(genre_node) && genre_node->valuestring) {
        strncpy(entry->genre, genre_node->valuestring, sizeof(entry->genre) - 1);
        entry->genre[sizeof(entry->genre) - 1] = '\0';
    }

    cJSON *year_node = cJSON_GetObjectItem(track_obj, "year");
    if (year_node && cJSON_IsNumber(year_node)) {
        entry->year = year_node->valueint;
    }

    cJSON *explicit_node = cJSON_GetObjectItem(track_obj, "contentWarning");
    if (explicit_node && cJSON_IsString(explicit_node) && explicit_node->valuestring) {
        entry->explicit_content = (strcmp(explicit_node->valuestring, "explicit") == 0) ? 1 : 0;
    }

    return 0;
}

static int ym_api_parse_track_id(const char *item_json,
                                 size_t item_size,
                                 char *out_id,
                                 size_t out_size)
{
    cJSON *root;
    cJSON *id;

    if (!item_json || item_size == 0 || !out_id || out_size == 0) {
        return -1;
    }
    root = cJSON_ParseWithLength(item_json, item_size);
    if (!root) {
        return 1;
    }
    id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!id) {
        cJSON_Delete(root);
        return 1;
    }
    if (cJSON_IsString(id) && id->valuestring && id->valuestring[0]) {
        if (strlen(id->valuestring) >= out_size) {
            cJSON_Delete(root);
            return 1;
        }
        snprintf(out_id, out_size, "%s", id->valuestring);
    } else if (cJSON_IsNumber(id)) {
        int written = snprintf(out_id, out_size, "%.0f", id->valuedouble);
        if (written <= 0 || (size_t)written >= out_size) {
            cJSON_Delete(root);
            return 1;
        }
    } else {
        cJSON_Delete(root);
        return 1;
    }
    cJSON_Delete(root);
    return 0;
}

int ym_api_tracks_hydrate(YmApiContext *ctx,
                          const ListIndexId *ids,
                          int count,
                          YmApiTrackCallback on_track,
                          void *user_data,
                          int *out_status)
{
    char payload[YM_API_HYDRATE_MAX * (LIST_INDEX_ID_SIZE + 1) + 16];
    NetHttpResponse response;
    cJSON *root = NULL;
    cJSON *result = NULL;
    int payload_len;
    int i;
    int delivered = 0;

    if (out_status) {
        *out_status = NET_LOAD_ERR_INTERNAL;
    }
    if (!ctx || !ctx->oauth_token || !ids || !on_track ||
        count <= 0 || count > YM_API_HYDRATE_MAX) {
        return -1;
    }

    payload_len = snprintf(payload, sizeof(payload), "track-ids=");
    for (i = 0; i < count; i++) {
        int n = snprintf(payload + payload_len,
                         sizeof(payload) - (size_t)payload_len,
                         "%s%s", i ? "," : "", ids[i]);
        if (n < 0 || (size_t)(payload_len + n) >= sizeof(payload)) {
            return -1;
        }
        payload_len += n;
    }

    if (out_status) {
        *out_status = NET_LOAD_ERR_TRANSPORT;
    }
    memset(&response, 0, sizeof(response));
    if (http_request(&(HttpRequest){
            .method = HTTP_POST,
            .url = "https://api.music.yandex.net/tracks",
            .token = ctx->oauth_token,
            .content_type = "application/x-www-form-urlencoded",
            .payload = payload, .payload_size = payload_len
        }, &response) != 0) {
        logLine("ym_api_tracks: POST failed\n");
        return -1;
    }
    if (response.status_code != 200 || !response.body || response.body_size <= 0) {
        logLine("ym_api_tracks: non-200 or empty status=%d\n", response.status_code);
        if (out_status) {
            *out_status = (response.status_code == 200)
                              ? NET_LOAD_ERR_DATA
                              : response.status_code;
        }
        net_http_response_free(&response);
        return -1;
    }

    root = cJSON_ParseWithLength(response.body, (size_t)response.body_size);
    net_http_response_free(&response);
    if (!root) {
        if (out_status) {
            *out_status = NET_LOAD_ERR_DATA;
        }
        return -1;
    }

    result = cJSON_GetObjectItem(root, "result");
    if (result && cJSON_IsArray(result)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, result) {
            TrackEntry entry;
            int rc = ym_api_parse_track_from_object(item, &entry);
            if (rc == 0) {
                delivered++;
                if (on_track(&entry, user_data) != YM_API_STREAM_CONTINUE) {
                    break;
                }
            }
            /* rc == 1: no-rights/unparseable item — skipped by design. */
        }
    }
    cJSON_Delete(root);

    if (out_status) {
        *out_status = 0;
    }
    logLine("ym_api_tracks: hydrated %d/%d\n", delivered, count);
    return 0;
}

int ym_api_playlist_tracks_parser_init(YmPlaylistTracksParser *parser,
                                       YmApiTrackIdCallback on_track_id,
                                       void *user_data)
{
    if (!parser || !on_track_id) {
        return -1;
    }

    memset(parser, 0, sizeof(*parser));
    parser->on_track_id = on_track_id;
    parser->user_data = user_data;
    return 0;
}

static int ym_api_playlist_tracks_feed_char(YmPlaylistTracksParser *parser, char c)
{
    static const char result_key[] = "\"result\"";
    static const char tracks_key[] = "\"tracks\"";

    if (!parser) {
        return -1;
    }

    if (!parser->in_result_object) {
        if (parser->waiting_result_object) {
            if (c == '{') {
                parser->in_result_object = 1;
                parser->waiting_result_object = 0;
            }
            return 0;
        }

        if (c == result_key[parser->result_key_match]) {
            parser->result_key_match++;
            if (result_key[parser->result_key_match] == '\0') {
                parser->result_key_match = 0;
                parser->waiting_result_object = 1;
            }
        } else {
            parser->result_key_match = (c == result_key[0]) ? 1 : 0;
        }
        return 0;
    }

    if (!parser->in_tracks_array) {
        if (parser->waiting_array_start) {
            if (c == '[') {
                parser->in_tracks_array = 1;
                parser->waiting_array_start = 0;
            }
            return 0;
        }

        if (c == tracks_key[parser->key_match]) {
            parser->key_match++;
            if (tracks_key[parser->key_match] == '\0') {
                parser->key_match = 0;
                parser->waiting_array_start = 1;
            }
        } else {
            parser->key_match = (c == tracks_key[0]) ? 1 : 0;
        }
        return 0;
    }

    if (!parser->capturing_object) {
        if (c == ']') {
            parser->in_tracks_array = 0;
            return 0;
        }
        if (c == '{') {
            parser->capturing_object = 1;
            parser->object_depth = 1;
            parser->in_string = 0;
            parser->escape_next = 0;
            parser->obj_len = 0;
            if (parser->obj_cap <= 0) {
                parser->obj_cap = 2048;
                parser->obj_buf = (char *)malloc((size_t)parser->obj_cap);
                if (!parser->obj_buf) {
                    return -1;
                }
            }
            parser->obj_buf[parser->obj_len++] = c;
        }
        return 0;
    }

    if (parser->obj_len + 2 > parser->obj_cap) {
        int new_cap = parser->obj_cap * 2;
        char *new_buf = (char *)realloc(parser->obj_buf, (size_t)new_cap);
        if (!new_buf) {
            return -1;
        }
        parser->obj_buf = new_buf;
        parser->obj_cap = new_cap;
    }
    parser->obj_buf[parser->obj_len++] = c;

    if (parser->in_string) {
        if (parser->escape_next) {
            parser->escape_next = 0;
        } else if (c == '\\') {
            parser->escape_next = 1;
        } else if (c == '"') {
            parser->in_string = 0;
        }
        return 0;
    }

    if (c == '"') {
        parser->in_string = 1;
        return 0;
    }
    if (c == '{') {
        parser->object_depth++;
        return 0;
    }
    if (c == '}') {
        parser->object_depth--;
        if (parser->object_depth == 0) {
            int parse_rc;
            YmApiStreamDecision decision = YM_API_STREAM_CONTINUE;

            parser->obj_buf[parser->obj_len] = '\0';
            char track_id[LIST_INDEX_ID_SIZE];
            parse_rc = ym_api_parse_track_id(parser->obj_buf,
                                             (size_t)parser->obj_len,
                                             track_id, sizeof(track_id));
            if (parse_rc == 0) {
                decision = parser->on_track_id(track_id, parser->user_data);
            }
            parser->capturing_object = 0;
            parser->obj_len = 0;

            if (parse_rc == 0) {
                if (decision == YM_API_STREAM_STOP) {
                    return 1;
                }
                if (decision == YM_API_STREAM_ERROR) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

int ym_api_playlist_tracks_parser_feed(YmPlaylistTracksParser *parser,
                                       const char *data,
                                       size_t size)
{
    if (!parser || (!data && size > 0)) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    for (size_t i = 0; i < size; ++i) {
        int rc = ym_api_playlist_tracks_feed_char(parser, data[i]);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

void ym_api_playlist_tracks_parser_destroy(YmPlaylistTracksParser *parser)
{
    if (!parser) {
        return;
    }
    if (parser->obj_buf) {
        free(parser->obj_buf);
        parser->obj_buf = NULL;
    }
    parser->obj_len = 0;
    parser->obj_cap = 0;
}
