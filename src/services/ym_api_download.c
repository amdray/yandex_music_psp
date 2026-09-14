#include "services/ym_api.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <cjson/cJSON.h>

#include "core/logger.h"
#include "services/net_http.h"

#define YM_FILE_INFO_SECRET "kzqU4XhfCaY6B6JTHODeq5"
#define YM_FILE_INFO_QUALITY "nq"
#define YM_FILE_INFO_CODECS "mp3"
#define YM_FILE_INFO_TRANSPORTS "raw"

static int ym_url_encode_component(const char *in, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;

    if (!in || !out || out_size == 0) {
        return -1;
    }

    while (*in) {
        unsigned char c = (unsigned char)*in++;
        int plain = ((c >= 'A' && c <= 'Z') ||
                     (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~');
        if (plain) {
            if (used + 1 >= out_size) {
                return -1;
            }
            out[used++] = (char)c;
        } else {
            if (used + 3 >= out_size) {
                return -1;
            }
            out[used++] = '%';
            out[used++] = hex[(c >> 4) & 0x0F];
            out[used++] = hex[c & 0x0F];
        }
    }
    out[used] = '\0';
    return 0;
}

static int ym_download_build_mp3_raw_url(const char *track_id,
                                         char *out_url, size_t out_size)
{
    const mbedtls_md_info_t *md_info;
    time_t now;
    char ts[24];
    char sign_input[96];
    unsigned char digest[32];
    unsigned char sign_b64_raw[64];
    char sign_b64[64];
    char sign_query[96];
    size_t olen = 0;
    int url_len;

    if (!track_id || !out_url || out_size == 0) {
        return -1;
    }

    now = time(NULL);
    if (now <= 0) {
        logLine("ym_api_download: cannot read system time\n");
        return -1;
    }
    snprintf(ts, sizeof(ts), "%ld", (long)now);
    ts[sizeof(ts) - 1] = '\0';

    snprintf(sign_input, sizeof(sign_input), "%s%s%s%s%s",
             ts, track_id, YM_FILE_INFO_QUALITY,
             YM_FILE_INFO_CODECS, YM_FILE_INFO_TRANSPORTS);
    sign_input[sizeof(sign_input) - 1] = '\0';

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info ||
        mbedtls_md_hmac(md_info,
                        (const unsigned char *)YM_FILE_INFO_SECRET,
                        strlen(YM_FILE_INFO_SECRET),
                        (const unsigned char *)sign_input,
                        strlen(sign_input),
                        digest) != 0) {
        logLine("ym_api_download: HMAC-SHA256 failed\n");
        return -1;
    }

    if (mbedtls_base64_encode(sign_b64_raw, sizeof(sign_b64_raw), &olen,
                              digest, sizeof(digest)) != 0 ||
        olen >= sizeof(sign_b64)) {
        logLine("ym_api_download: base64 failed\n");
        return -1;
    }
    memcpy(sign_b64, sign_b64_raw, olen);
    sign_b64[olen] = '\0';
    while (olen > 0 && sign_b64[olen - 1] == '=') {
        sign_b64[--olen] = '\0';
    }

    if (ym_url_encode_component(sign_b64, sign_query, sizeof(sign_query)) != 0) {
        logLine("ym_api_download: sign urlencode failed\n");
        return -1;
    }

    url_len = snprintf(out_url, out_size,
                       "https://api.music.yandex.net/get-file-info"
                       "?ts=%s&trackId=%s&quality=%s&codecs=%s&sign=%s&transports=%s",
                       ts, track_id, YM_FILE_INFO_QUALITY, YM_FILE_INFO_CODECS,
                       sign_query, YM_FILE_INFO_TRANSPORTS);
    if (url_len < 0 || (size_t)url_len >= out_size) {
        logLine("ym_api_download: URL truncated need=%d have=%u\n",
                url_len, (unsigned int)out_size);
        out_url[0] = '\0';
        return -1;
    }
    return 0;
}

static int ym_json_string_nonempty(cJSON *node)
{
    return node && cJSON_IsString(node) && node->valuestring && node->valuestring[0] != '\0';
}

YmStatus ym_api_download_get_mp3_raw(YmApiContext *ctx,
                                     const char *track_id,
                                     YmDownloadVariant *out)
{
    NetHttpResponse response;
    char url[256];
    cJSON *root = NULL;
    cJSON *result = NULL;
    cJSON *download_info = NULL;
    cJSON *codec = NULL;
    cJSON *transport = NULL;
    cJSON *bitrate = NULL;
    cJSON *key = NULL;
    cJSON *url_node = NULL;
    const char *direct_url = NULL;
    int bitrate_kbps = 0;
    int ret;

    if (!ctx || !ctx->oauth_token || !track_id || !out || track_id[0] == '\0') {
        logLine("ym_api_download: invalid args ctx=%d token=%d track_id=%d out=%d\n",
                ctx ? 1 : 0,
                (ctx && ctx->oauth_token) ? 1 : 0,
                track_id ? 1 : 0,
                out ? 1 : 0);
        logger_flush();
        return YM_ERR_PARSE;
    }
    memset(out, 0, sizeof(*out));

    if (ym_download_build_mp3_raw_url(track_id, url, sizeof(url)) != 0) {
        logger_flush();
        return YM_ERR_PARSE;
    }

    logLine("ym_api_download: fetch mp3/raw track_id='%s'\n", track_id);

    memset(&response, 0, sizeof(response));
    ret = http_request(&(HttpRequest){
        .method = HTTP_GET, .url = url, .token = ctx->oauth_token,
        .initial_buffer = HTTP_TLS_BUFFER_SMALL
    }, &response);
    if (ret != 0) {
        logLine("ym_api_download: HTTP GET failed rc=%d\n", ret);
        logger_flush();
        return net_http_error_is_network(ret) ? YM_ERR_NETWORK : YM_ERR_HTTP;
    }

    if (response.status_code == 401 || response.status_code == 403) {
        logLine("ym_api_download: auth failed status=%d\n", response.status_code);
        logger_flush();
        net_http_response_free(&response);
        return YM_ERR_AUTH;
    }

    if (response.status_code != 200 || !response.body || response.body_size <= 0) {
        logLine("ym_api_download: bad response status=%d body=%d size=%d\n",
                response.status_code,
                response.body ? 1 : 0,
                response.body_size);
        logger_flush();
        net_http_response_free(&response);
        return YM_ERR_HTTP;
    }

    logLine("ym_api_download: response status=%d size=%d\n",
            response.status_code, response.body_size);

    root = cJSON_Parse(response.body);
    net_http_response_free(&response);
    memset(&response, 0, sizeof(response));

    if (!root) {
        logLine("ym_api_download: JSON parse failed\n");
        logger_flush();
        return YM_ERR_PARSE;
    }

    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (result && cJSON_IsObject(result)) {
        download_info = cJSON_GetObjectItemCaseSensitive(result, "downloadInfo");
    }

    if (!download_info || !cJSON_IsObject(download_info)) {
        logLine("ym_api_download: missing result.downloadInfo\n");
        cJSON_Delete(root);
        logger_flush();
        return YM_ERR_PARSE;
    }

    codec = cJSON_GetObjectItemCaseSensitive(download_info, "codec");
    transport = cJSON_GetObjectItemCaseSensitive(download_info, "transport");
    bitrate = cJSON_GetObjectItemCaseSensitive(download_info, "bitrate");
    key = cJSON_GetObjectItemCaseSensitive(download_info, "key");

    if (!ym_json_string_nonempty(codec) || strcmp(codec->valuestring, "mp3") != 0) {
        logLine("ym_api_download: rejected codec='%s'\n",
                (codec && cJSON_IsString(codec) && codec->valuestring) ? codec->valuestring : "(missing)");
        cJSON_Delete(root);
        logger_flush();
        return YM_ERR_UNSUPPORTED;
    }

    if (!ym_json_string_nonempty(transport) || strcmp(transport->valuestring, "raw") != 0) {
        logLine("ym_api_download: rejected transport='%s'\n",
                (transport && cJSON_IsString(transport) && transport->valuestring) ? transport->valuestring : "(missing)");
        cJSON_Delete(root);
        logger_flush();
        return YM_ERR_UNSUPPORTED;
    }

    if (key && !cJSON_IsNull(key) && !(cJSON_IsString(key) && (!key->valuestring || key->valuestring[0] == '\0'))) {
        logLine("ym_api_download: rejected encrypted response key_present=1\n");
        cJSON_Delete(root);
        logger_flush();
        return YM_ERR_UNSUPPORTED;
    }

    if (bitrate && cJSON_IsNumber(bitrate)) {
        bitrate_kbps = bitrate->valueint;
    }

    url_node = cJSON_GetObjectItemCaseSensitive(download_info, "url");
    if (ym_json_string_nonempty(url_node)) {
        direct_url = url_node->valuestring;
    } else {
        cJSON *urls = cJSON_GetObjectItemCaseSensitive(download_info, "urls");
        if (urls && cJSON_IsArray(urls)) {
            cJSON *first = cJSON_GetArrayItem(urls, 0);
            if (ym_json_string_nonempty(first)) {
                direct_url = first->valuestring;
            }
        }
    }

    if (!direct_url || direct_url[0] == '\0') {
        logLine("ym_api_download: missing direct mp3 url\n");
        cJSON_Delete(root);
        logger_flush();
        return YM_ERR_NOT_FOUND;
    }

    if (strlen(direct_url) >= sizeof(out->url)) {
        logLine("ym_api_download: direct MP3 URL too long (%u >= %u)\n",
                (unsigned int)strlen(direct_url), (unsigned int)sizeof(out->url));
        cJSON_Delete(root);
        logger_flush();
        return YM_ERR_PARSE;
    }

    strcpy(out->url, direct_url);
    strcpy(out->codec, "mp3");
    strcpy(out->transport, "raw");
    out->bitrate_kbps = bitrate_kbps;
    out->encrypted = 0;
    logLine("ym_api_download: direct MP3 url ok codec=mp3 transport=raw bitrate=%d key=none url_len=%u\n",
            out->bitrate_kbps, (unsigned int)strlen(out->url));
    cJSON_Delete(root);
    logger_flush();
    return YM_OK;
}
