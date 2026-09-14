// Лайк/анлайк трека. Паттерн запроса — как ym_api_playlists.c.
#include "services/ym_api_like.h"

#include <stdio.h>
#include <string.h>

#include "core/logger.h"
#include "services/net_http.h"

int ym_api_track_like(YmApiContext *ctx, int uid,
                      const char *track_id, int like)
{
    NetHttpResponse response;
    char url[256];
    int r;

    if (!ctx || !ctx->oauth_token || uid <= 0 || !track_id || !track_id[0]) {
        return -1;
    }
    memset(&response, 0, sizeof(response));
    snprintf(url, sizeof(url),
             "https://api.music.yandex.net/users/%d/likes/tracks/%s?track-ids=%s",
             uid, like ? "add-multiple" : "remove", track_id);
    url[sizeof(url) - 1] = '\0';

    r = http_request(&(HttpRequest){
        .method = HTTP_POST,
        .url = url,
        .token = ctx->oauth_token,
        .content_type = "application/x-www-form-urlencoded",
        .payload = "",
        .payload_size = 0,
        .initial_buffer = HTTP_TLS_BUFFER_SMALL,
    }, &response);
    if (r != 0) {
        logLine("ym_api_like: transport failed %d id='%s' like=%d\n",
                r, track_id, like);
        net_http_response_free(&response);
        return -1;
    }
    logLine("ym_api_like: ok status=%d id='%s' like=%d\n",
            response.status_code, track_id, like);
    logger_flush();
    r = (response.status_code == 200) ? 0 : -1;
    net_http_response_free(&response);
    return r;
}
