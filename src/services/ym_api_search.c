#include <sys/select.h>
#include <curl/curl.h>
// Поиск треков Яндекс Музыки: /search?type=track.
// Форма ответа как в psp_yandex (ya_search):
// result.tracks.results[] — объекты треков, id строкой или числом.
// Парсинг — местным cJSON, транспорт — http_request.
#include "services/ym_api_search.h"

#include <stdio.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "core/json.h"
#include "core/logger.h"
#include "services/net_http.h"

int ym_api_search_tracks(YmApiContext *ctx, const char *query,
                         TrackRef *out_refs, int max_ids)
{
    NetHttpResponse resp;
    char *enc;
    char url[768];
    cJSON *root = NULL, *result, *tracks, *arr;
    int n = 0, i, count, url_len;

    if (!ctx || !ctx->oauth_token || !query || !query[0] ||
        !out_refs || max_ids <= 0) {
        return -1;
    }
    if (max_ids > YM_API_SEARCH_MAX) {
        max_ids = YM_API_SEARCH_MAX;
    }
    enc = curl_easy_escape(NULL, query, 0);
    if (!enc) {
        logLine("ym_api_search: query urlencode failed\n");
        logger_flush();
        return -1;
    }
    url_len = snprintf(url, sizeof(url),
                       "https://api.music.yandex.net/search"
                       "?text=%s&type=track&page=0",
                       enc);
    curl_free(enc);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        logLine("ym_api_search: URL truncated\n");
        logger_flush();
        return -1;
    }

    memset(&resp, 0, sizeof(resp));
    if (http_request(&(HttpRequest){
            .method = HTTP_GET,
            .url = url,
            .token = ctx->oauth_token,
            .initial_buffer = HTTP_TLS_BUFFER_MEDIUM,
        }, &resp) != 0) {
        logLine("ym_api_search: transport failed\n");
        net_http_response_free(&resp);
        logger_flush();
        return -1;
    }
    if (resp.status_code != 200 || !resp.body || resp.body_size <= 0) {
        logLine("ym_api_search: bad status=%d\n", resp.status_code);
        net_http_response_free(&resp);
        logger_flush();
        return -1;
    }
    root = ym_json_parse(resp.body, NULL);
    if (!root) {
        logLine("ym_api_search: parse failed\n");
        net_http_response_free(&resp);
        logger_flush();
        return -1;
    }
    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    tracks = cJSON_IsObject(result)
        ? cJSON_GetObjectItemCaseSensitive(result, "tracks") : NULL;
    arr = cJSON_IsObject(tracks)
        ? cJSON_GetObjectItemCaseSensitive(tracks, "results") : NULL;
    if (!cJSON_IsArray(arr)) {
        logLine("ym_api_search: no tracks.results array\n");
        cJSON_Delete(root);
        net_http_response_free(&resp);
        logger_flush();
        return -1;
    }
    count = cJSON_GetArraySize(arr);
    for (i = 0; i < count && n < max_ids; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        cJSON *t, *v;
        memset(&out_refs[n], 0, sizeof(out_refs[n]));
        if (!cJSON_IsObject(e)) {
            continue;
        }
        // На всякий случай терпим обёртку {track: {...}}.
        t = cJSON_GetObjectItemCaseSensitive(e, "track");
        e = (cJSON_IsObject(t)) ? t : e;
        v = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            snprintf(out_refs[n].id, sizeof(out_refs[n].id), "%s", v->valuestring);
            n++;
        } else if (cJSON_IsNumber(v)) {
            snprintf(out_refs[n].id, sizeof(out_refs[n].id), "%d", v->valueint);
            n++;
        }
    }
    cJSON_Delete(root);
    net_http_response_free(&resp);
    logLine("ym_api_search: tracks=%d\n", n);
    logger_flush();
    return (n > 0) ? n : -1;
}
