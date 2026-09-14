// Персональная волна (rotor station). См. ym_api_wave.h.
#include "services/ym_api_wave.h"

#include <stdio.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "core/logger.h"
#include "services/net_http.h"

int ym_api_wave_station_tracks(YmApiContext *ctx, const char *station_id,
                               ListIndexId *out_ids, int max_count)
{
    NetHttpResponse resp;
    char url[256];
    cJSON *root = NULL, *result, *seq;
    int n = 0, i, count;

    if (!ctx || !ctx->oauth_token || !station_id || !station_id[0] ||
        !out_ids || max_count <= 0) {
        return -1;
    }
    snprintf(url, sizeof(url),
             "https://api.music.yandex.net/rotor/station/%s/tracks",
             station_id);
    url[sizeof(url) - 1] = '\0';

    memset(&resp, 0, sizeof(resp));
    if (http_request(&(HttpRequest){
            .method = HTTP_GET,
            .url = url,
            .token = ctx->oauth_token,
            .initial_buffer = HTTP_TLS_BUFFER_MEDIUM,
        }, &resp) != 0) {
        logLine("ym_api_wave: transport failed\n");
        net_http_response_free(&resp);
        return -1;
    }
    if (resp.status_code != 200 || !resp.body || resp.body_size <= 0) {
        logLine("ym_api_wave: bad status=%d\n", resp.status_code);
        net_http_response_free(&resp);
        return -1;
    }
    root = cJSON_Parse(resp.body);
    if (!root) {
        logLine("ym_api_wave: parse failed\n");
        net_http_response_free(&resp);
        return -1;
    }
    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    seq = cJSON_IsObject(result)
        ? cJSON_GetObjectItemCaseSensitive(result, "sequence") : NULL;
    if (!cJSON_IsArray(seq)) {
        logLine("ym_api_wave: no sequence array\n");
        cJSON_Delete(root);
        net_http_response_free(&resp);
        return -1;
    }
    count = cJSON_GetArraySize(seq);
    if (count > max_count) {
        count = max_count;
    }
    for (i = 0; i < count; i++) {
        cJSON *e = cJSON_GetArrayItem(seq, i);
        cJSON *t, *v;
        if (!cJSON_IsObject(e)) {
            continue;
        }
        t = cJSON_GetObjectItemCaseSensitive(e, "track");
        if (!cJSON_IsObject(t)) {
            continue;
        }
        v = cJSON_GetObjectItemCaseSensitive(t, "id");
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            snprintf(out_ids[n], sizeof(out_ids[n]), "%s", v->valuestring);
            n++;
        } else if (cJSON_IsNumber(v)) {
            snprintf(out_ids[n], sizeof(out_ids[n]), "%d", v->valueint);
            n++;
        }
    }
    cJSON_Delete(root);
    net_http_response_free(&resp);
    logLine("ym_api_wave: station tracks=%d\n", n);
    logger_flush();
    return (n > 0) ? n : -1;
}
