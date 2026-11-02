#include "services/ym_api.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include "core/json.h"

#include "core/fs.h"
#include "core/logger.h"
#include "services/api_parser.h"
#include "services/net_http.h"
#include "services/ym_api_track_parse.h"

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

    if (YM_DEBUG_RESPONSE_DUMPS && log_path) {
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

/* Full-track parsing lives in ym_api_track_parse.c (shared with Rotor);
 * this unit keeps only the playlist id-stream helpers. */
int ym_api_tracks_hydrate_refs(YmApiContext *ctx,
                               const TrackRef *refs,
                               int count,
                               YmApiTrackCallback on_track,
                               void *user_data,
                               int *out_status)
{
    char payload[YM_API_HYDRATE_MAX * (TRACK_ID_SIZE + sizeof("&trackIds=") - 1 + 11) + sizeof("removeDuplicates=false&withProgress=true")];
    NetHttpResponse response;
    cJSON *root = NULL;
    cJSON *result = NULL;
    int payload_len;
    int i;
    int delivered = 0;

    if (out_status) {
        *out_status = NET_LOAD_ERR_INTERNAL;
    }
    if (!ctx || !ctx->oauth_token || !refs || !on_track ||
        count <= 0 || count > YM_API_HYDRATE_MAX) {
        return -1;
    }

    payload_len = snprintf(payload, sizeof(payload), "removeDuplicates=false&withProgress=true");
    for (i = 0; i < count; i++) {
        int n;
        if (refs[i].album_id > 0) {
            n = snprintf(payload + payload_len,
                         sizeof(payload) - (size_t)payload_len,
                         "&trackIds=%s:%d", refs[i].id,
                         refs[i].album_id);
        } else {
            n = snprintf(payload + payload_len,
                         sizeof(payload) - (size_t)payload_len,
                         "&trackIds=%s", refs[i].id);
        }
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

    root = ym_json_parse_len(response.body, (size_t)response.body_size, NULL);
    net_http_response_free(&response);
    if (!root) {
        if (out_status) {
            *out_status = NET_LOAD_ERR_DATA;
        }
        return -1;
    }

    result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsArray(result) || cJSON_GetArraySize(result) != count) {
        logLine("ym_api_tracks: response count mismatch requested=%d received=%d\n",
                count, cJSON_IsArray(result) ? cJSON_GetArraySize(result) : -1);
        if (out_status) *out_status = NET_LOAD_ERR_DATA;
        cJSON_Delete(root);
        return -1;
    }
    /* Validate the complete positional mapping before publishing any entry. */
    cJSON *checked;
    i = 0;
    cJSON_ArrayForEach(checked, result) {
        char id[TRACK_ID_SIZE];
        if (ym_api_parse_track_id_node(cJSON_GetObjectItemCaseSensitive(checked, "id"), id, sizeof(id)) != 0 ||
            strcmp(id, refs[i++].id) != 0) {
            logLine("ym_api_tracks: response identity mismatch\n");
            if (out_status) *out_status = NET_LOAD_ERR_DATA;
            cJSON_Delete(root);
            return -1;
        }
    }
    cJSON *item = NULL;
    i = 0;
    cJSON_ArrayForEach(item, result) {
        TrackEntry entry;
        int request_index = i++;
        if (ym_api_parse_track_from_object(item, &entry) == 0) {
            delivered++;
            if (on_track(&refs[request_index], &entry, user_data) != YM_API_STREAM_CONTINUE) {
                break;
            }
        }
    }
    cJSON_Delete(root);

    if (out_status) {
        *out_status = 0;
    }
    logLine("ym_api_tracks: hydrated %d/%d\n", delivered, count);
    return 0;
}

int ym_api_parse_playlist_composition(const char *json, size_t size,
                                      TrackRef **out_refs, int *out_count, int *out_revision)
{
    if (!json || !out_refs || !out_count || !out_revision) return -1;
    *out_refs = NULL;
    *out_count = 0;
    cJSON *root = ym_json_parse_len(json, size, NULL);
    if (!root) return -1;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(result, "revision");
    cJSON *tracks = cJSON_GetObjectItemCaseSensitive(result, "tracks");
    if (!cJSON_IsNumber(revision) || revision->valuedouble < 0 ||
        revision->valuedouble > 2147483647.0 || revision->valuedouble != revision->valueint ||
        !cJSON_IsArray(tracks)) { cJSON_Delete(root); return -1; }
    int count = cJSON_GetArraySize(tracks);
    TrackRef *refs = count ? calloc((size_t)count, sizeof(*refs)) : NULL;
    if (count && !refs) { cJSON_Delete(root); return -1; }
    int i = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, tracks) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON *album = cJSON_GetObjectItemCaseSensitive(item, "albumId");
        if (!cJSON_IsNumber(id) || !cJSON_IsNumber(album) ||
            id->valuedouble <= 0 || id->valuedouble > 9007199254740991.0 ||
            (double)(long long)id->valuedouble != id->valuedouble ||
            album->valuedouble <= 0 || album->valuedouble > 2147483647.0 ||
            album->valuedouble != album->valueint) {
            free(refs); cJSON_Delete(root); return -1;
        }
        snprintf(refs[i].id, sizeof(refs[i].id), "%lld", (long long)id->valuedouble);
        refs[i++].album_id = album->valueint;
    }
    *out_refs = refs;
    *out_count = count;
    *out_revision = revision->valueint;
    cJSON_Delete(root);
    return 0;
}
