// Альбомы: лайкнутые + треки альбома. Формы ответов повторяют наш
// psp_yandex (liked_entries("albums"), /albums/{id}/with-tracks),
// парсинг — местным cJSON.
#include "services/ym_api_albums.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "core/fs.h"
#include "core/logger.h"
#include "services/net_http.h"
#include "services/track_meta_store.h"
#include "services/ym_api_track_parse.h"

static void copy_json_string(const cJSON *node, char *dst, int dst_size)
{
    if (!dst || dst_size <= 0) {
        return;
    }
    dst[0] = '\0';
    if (cJSON_IsString(node) && node->valuestring) {
        strncpy(dst, node->valuestring, (size_t)(dst_size - 1));
        dst[dst_size - 1] = '\0';
    }
}

static int json_int(const cJSON *node, int fallback)
{
    if (cJSON_IsNumber(node)) {
        return node->valueint;
    }
    if (cJSON_IsString(node) && node->valuestring) {
        return atoi(node->valuestring);
    }
    return fallback;
}

static int fetch_json(YmApiContext *ctx, const char *url,
                      NetHttpResponse *resp)
{
    int r;

    memset(resp, 0, sizeof(*resp));
    r = http_request(&(HttpRequest){
        .method = HTTP_GET,
        .url = url,
        .token = ctx->oauth_token,
        .initial_buffer = HTTP_TLS_BUFFER_MEDIUM,
    }, resp);
    if (r != 0) {
        logLine("ym_api_albums: transport failed %d\n", r);
        net_http_response_free(resp);
        return -1;
    }
    if (resp->status_code != 200 || !resp->body || resp->body_size <= 0) {
        logLine("ym_api_albums: bad status=%d\n", resp->status_code);
        net_http_response_free(resp);
        return -1;
    }
    return 0;
}

static void save_response_dump(const char *path, const NetHttpResponse *resp)
{
    char abs_path[512];
    int rc;

    if (!path || !resp || !resp->body || resp->body_size <= 0) {
        return;
    }
    if (fs_ensure_dir(path) != 0) {
        logLine("ym_api_albums: cannot prepare dump path %s\n", path);
        return;
    }
    if (fs_make_abs_path(path, abs_path, sizeof(abs_path)) != 0) {
        logLine("ym_api_albums: cannot resolve dump path %s\n", path);
        return;
    }
    rc = fs_write_atomic(abs_path, resp->body, (size_t)resp->body_size);
    if (rc < 0) {
        logLine("ym_api_albums: cannot save %s rc=%d\n", path, rc);
        return;
    }
    logLine("ym_api_albums: saved %s (%d bytes)\n",
            path, resp->body_size);
}

int ym_api_liked_albums(YmApiContext *ctx, int uid,
                        ArtistAlbumEntry **out, int *out_count)
{
    NetHttpResponse resp;
    char url[256];
    cJSON *root = NULL, *result, *arr;
    ArtistAlbumEntry *list = NULL;
    int cap = 0, n = 0, i, count;

    if (!ctx || !ctx->oauth_token || uid <= 0 || !out || !out_count) {
        return -1;
    }
    *out = NULL;
    *out_count = 0;
    snprintf(url, sizeof(url),
             "https://api.music.yandex.net/users/%d/likes/albums?rich=true",
             uid);
    url[sizeof(url) - 1] = '\0';
    if (fetch_json(ctx, url, &resp) != 0) {
        return -1;
    }
    root = cJSON_Parse(resp.body);
    if (!root) {
        logLine("ym_api_albums: parse failed\n");
        net_http_response_free(&resp);
        return -1;
    }
    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    arr = NULL;
    if (cJSON_IsObject(result)) {
        // result.library.albums[] или result.albums[]
        cJSON *lib = cJSON_GetObjectItemCaseSensitive(result, "library");
        if (cJSON_IsObject(lib)) {
            arr = cJSON_GetObjectItemCaseSensitive(lib, "albums");
        }
        if (!cJSON_IsArray(arr)) {
            arr = cJSON_GetObjectItemCaseSensitive(result, "albums");
        }
    } else if (cJSON_IsArray(result)) {
        arr = result;
    }
    if (!cJSON_IsArray(arr)) {
        logLine("ym_api_albums: no albums array\n");
        cJSON_Delete(root);
        net_http_response_free(&resp);
        return -1;
    }
    count = cJSON_GetArraySize(arr);
    for (i = 0; i < count; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        cJSON *a, *v;
        ArtistAlbumEntry *o;
        if (!cJSON_IsObject(e)) {
            continue;
        }
        // Иногда элемент — обёртка {album: {...}}.
        a = cJSON_GetObjectItemCaseSensitive(e, "album");
        e = (cJSON_IsObject(a)) ? a : e;
        if (n >= cap) {
            int ncap = (cap == 0) ? 32 : cap * 2;
            ArtistAlbumEntry *grown = (ArtistAlbumEntry *)realloc(
                list, (size_t)ncap * sizeof(*grown));
            if (!grown) {
                break;
            }
            list = grown;
            cap = ncap;
        }
        o = &list[n];
        memset(o, 0, sizeof(*o));
        v = cJSON_GetObjectItemCaseSensitive(e, "id");
        o->album_id = json_int(v, 0);
        if (o->album_id <= 0) {
            continue;
        }
        v = cJSON_GetObjectItemCaseSensitive(e, "title");
        if (!cJSON_IsString(v)) {
            v = cJSON_GetObjectItemCaseSensitive(e, "name");
        }
        copy_json_string(v, o->title, sizeof(o->title));
        copy_json_string(cJSON_GetObjectItemCaseSensitive(e, "version"),
                         o->version, sizeof(o->version));
        copy_json_string(cJSON_GetObjectItemCaseSensitive(e, "coverUri"),
                         o->cover_uri, sizeof(o->cover_uri));
        o->year = json_int(cJSON_GetObjectItemCaseSensitive(e, "year"), 0);
        o->track_count =
            json_int(cJSON_GetObjectItemCaseSensitive(e, "trackCount"), 0);
        n++;
    }
    cJSON_Delete(root);
    net_http_response_free(&resp);
    logLine("ym_api_albums: parsed %d liked albums\n", n);
    logger_flush();
    *out = list;
    *out_count = n;
    return 0;
}

int ym_api_album_tracks(YmApiContext *ctx, int album_id,
                        TrackRef **out, int *out_count)
{
    NetHttpResponse resp;
    char url[256];
    char log_path[96];
    cJSON *root = NULL, *result, *volumes;
    TrackRef *refs = NULL;
    int cap = 0, n = 0, vi, vcount;
    int allocation_failed = 0;

    if (!ctx || !ctx->oauth_token || album_id <= 0 || !out || !out_count) {
        return -1;
    }
    *out = NULL;
    *out_count = 0;
    snprintf(url, sizeof(url),
             "https://api.music.yandex.net/albums/%d/with-tracks", album_id);
    url[sizeof(url) - 1] = '\0';
    if (fetch_json(ctx, url, &resp) != 0) {
        return -1;
    }
    snprintf(log_path, sizeof(log_path),
             "data/logs/album_%d_with_tracks_response.json", album_id);
    log_path[sizeof(log_path) - 1] = '\0';
    save_response_dump(log_path, &resp);
    root = cJSON_Parse(resp.body);
    if (!root) {
        logLine("ym_api_albums: tracks parse failed\n");
        net_http_response_free(&resp);
        return -1;
    }
    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    volumes = cJSON_IsObject(result)
        ? cJSON_GetObjectItemCaseSensitive(result, "volumes") : NULL;
    if (!cJSON_IsArray(volumes)) {
        logLine("ym_api_albums: no volumes array\n");
        cJSON_Delete(root);
        net_http_response_free(&resp);
        return -1;
    }
    vcount = cJSON_GetArraySize(volumes);
    for (vi = 0; vi < vcount && n < YM_API_ALBUM_TRACKS_MAX; vi++) {
        cJSON *arr = cJSON_GetArrayItem(volumes, vi);
        int ti, tcount;
        if (!cJSON_IsArray(arr)) {
            continue;
        }
        tcount = cJSON_GetArraySize(arr);
        for (ti = 0; ti < tcount && n < YM_API_ALBUM_TRACKS_MAX; ti++) {
            cJSON *e = cJSON_GetArrayItem(arr, ti);
            cJSON *v;
            char idbuf[40];
            TrackEntry entry;
            if (!cJSON_IsObject(e)) {
                continue;
            }
            v = cJSON_GetObjectItemCaseSensitive(e, "id");
            if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
                snprintf(idbuf, sizeof(idbuf), "%s", v->valuestring);
            } else if (cJSON_IsNumber(v)) {
                snprintf(idbuf, sizeof(idbuf), "%d", v->valueint);
            } else {
                continue;
            }
            if (n >= cap) {
                int ncap = (cap == 0) ? 32 : cap * 2;
                TrackRef *grown;
                if (ncap > YM_API_ALBUM_TRACKS_MAX) {
                    ncap = YM_API_ALBUM_TRACKS_MAX;
                }
                grown = (TrackRef *)realloc(refs,
                    (size_t)ncap * sizeof(*grown));
                if (!grown) {
                    allocation_failed = 1;
                    break;
                }
                refs = grown;
                cap = ncap;
            }
            if (n >= cap) {
                break;
            }
            memset(&refs[n], 0, sizeof(refs[n]));
            snprintf(refs[n].id, sizeof(refs[n].id), "%s", idbuf);
            refs[n].album_id = album_id;
            if (ym_api_parse_track_from_object_for_album(e, album_id,
                                                          &entry) == 0) {
                if (track_meta_store_put(&entry) != 0) {
                    logLine("ym_api_albums: metadata store failed track=%s album=%d\n",
                            idbuf, album_id);
                }
            }
            n++;
        }
        if (allocation_failed) {
            break;
        }
    }
    cJSON_Delete(root);
    net_http_response_free(&resp);
    if (allocation_failed || n <= 0) {
        if (allocation_failed) {
            logLine("ym_api_albums: no memory for album %d tracks\n",
                    album_id);
        }
        free(refs);
        return -1;
    }
    logLine("ym_api_albums: album %d tracks=%d\n", album_id, n);
    logger_flush();
    *out = refs;
    *out_count = n;
    return 0;
}
