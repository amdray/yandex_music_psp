/* Yandex Rotor wave backend. See ym_api_rotor.h for the contract. */
#include "services/ym_api_rotor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "core/logger.h"
#include "services/api_parser.h"
#include "services/list_index.h"
#include "services/net_http.h"
#include "services/ym_api_track_parse.h"

#define ROTOR_HOST "https://api.music.yandex.net"

int ym_api_rotor_timestamp_utc(char *out, size_t out_size)
{
    time_t now;
    struct tm *parts;
    int written;

    if (!out || out_size < YM_ROTOR_TIMESTAMP_SIZE) {
        return -1;
    }
    now = time(NULL);
    if (now <= 0) {
        snprintf(out, out_size, "1970-01-01T00:00:00.000Z");
        return 1;
    }
    parts = gmtime(&now);
    if (!parts) {
        snprintf(out, out_size, "1970-01-01T00:00:00.000Z");
        return 1;
    }
    written = snprintf(out, out_size,
                       "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                       parts->tm_year + 1900, parts->tm_mon + 1, parts->tm_mday,
                       parts->tm_hour, parts->tm_min, parts->tm_sec);
    if (written <= 0 || (size_t)written >= out_size) {
        return -1;
    }
    return 0;
}

int ym_api_rotor_composite_id(const char *track_id, int album_id,
                              char *out, size_t out_size)
{
    int written;

    if (!track_id || !track_id[0] || !out || out_size == 0) {
        return -1;
    }
    written = snprintf(out, out_size, "%s:%d", track_id, album_id);
    if (written <= 0 || (size_t)written >= out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    return 0;
}

static int rotor_url_encode_component(const char *in, char *out, size_t out_size)
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

/* Minimal JSON string escaper for controlled values (seeds, ids). */
static void rotor_json_escape(const char *in, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    size_t used = 0;

    if (!in || !out || out_size == 0) {
        return;
    }
    while (*in && used + 1 < out_size) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\') {
            if (used + 2 >= out_size) {
                break;
            }
            out[used++] = '\\';
            out[used++] = (char)c;
        } else if (c < 0x20) {
            if (used + 6 >= out_size) {
                break;
            }
            out[used++] = '\\';
            out[used++] = 'u';
            out[used++] = '0';
            out[used++] = '0';
            out[used++] = hex[(c >> 4) & 0x0F];
            out[used++] = hex[c & 0x0F];
        } else {
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
}

static int rotor_check_context(const YmApiContext *ctx)
{
    if (!ctx || !ctx->oauth_token || !ctx->oauth_token[0]) {
        return -1;
    }
    return 0;
}

/* POST a JSON payload; returns 0 with *resp filled, or the http_request
 * transport code (<0) with *resp zeroed. HTTP status/body validation is the
 * caller's job (it must free *resp). */
static int rotor_post_json(const YmApiContext *ctx, const char *url,
                           const char *payload, NetHttpResponse *resp)
{
    int req_rc;

    memset(resp, 0, sizeof(*resp));
    req_rc = http_request(&(HttpRequest){
        .method = HTTP_POST,
        .url = url,
        .token = ctx->oauth_token,
        .content_type = "application/json",
        .payload = payload,
        .payload_size = -1,
        .initial_buffer = HTTP_TLS_BUFFER_MEDIUM,
    }, resp);
    if (req_rc != 0) {
        logLine("ym_rotor: POST transport failed rc=%d\n", req_rc);
        net_http_response_free(resp);
        memset(resp, 0, sizeof(*resp));
        return req_rc;
    }
    return 0;
}

/* Map a transport code to the public retry verdict. */
static int rotor_transport_verdict(int req_rc)
{
    return net_http_error_is_network(req_rc) ? YM_ROTOR_ERR_RETRY : YM_ROTOR_ERR;
}

/* Map an HTTP status to the public retry verdict (non-200 only). */
static int rotor_status_verdict(int status_code)
{
    if (status_code == 408 || status_code == 429) {
        return YM_ROTOR_ERR_RETRY;
    }
    if (status_code >= 500 && status_code <= 599) {
        return YM_ROTOR_ERR_RETRY;
    }
    return YM_ROTOR_ERR;
}

static int rotor_copy_id_node(cJSON *node, char *out, size_t out_size,
                              const char *field_name)
{
    if (!cJSON_IsString(node) || !node->valuestring || !node->valuestring[0]) {
        logLine("ym_rotor: missing '%s'\n", field_name);
        return -1;
    }
    if (strlen(node->valuestring) >= out_size) {
        logLine("ym_rotor: '%s' too long, rejected\n", field_name);
        return -1;
    }
    snprintf(out, out_size, "%s", node->valuestring);
    return 0;
}

static int rotor_bool_node(cJSON *node)
{
    if (cJSON_IsTrue(node)) {
        return 1;
    }
    if (cJSON_IsFalse(node)) {
        return 0;
    }
    if (cJSON_IsNumber(node)) {
        return (node->valueint != 0) ? 1 : 0;
    }
    return 0;
}

/* Parse result.sequence[] into items with the shared full-track parser.
 * Returns 0 with *out_count set (may be 0 when nothing usable arrived). */
static int rotor_parse_sequence(cJSON *result_obj, const char *batch_id,
                                YmRotorSequenceItem *items, int max_items,
                                int *out_count)
{
    cJSON *seq_node;
    int seq_size;
    int seq_index;
    int parsed = 0;

    if (out_count) {
        *out_count = 0;
    }
    if (!cJSON_IsObject(result_obj)) {
        return -1;
    }
    seq_node = cJSON_GetObjectItemCaseSensitive(result_obj, "sequence");
    if (!cJSON_IsArray(seq_node)) {
        logLine("ym_rotor: no sequence array\n");
        return -1;
    }
    seq_size = cJSON_GetArraySize(seq_node);
    for (seq_index = 0; seq_index < seq_size; ++seq_index) {
        cJSON *elem = cJSON_GetArrayItem(seq_node, seq_index);
        cJSON *track_node;
        cJSON *liked_node;
        TrackEntry entry;
        int parse_rc;

        if (!cJSON_IsObject(elem)) {
            continue;
        }
        track_node = cJSON_GetObjectItemCaseSensitive(elem, "track");
        if (!cJSON_IsObject(track_node)) {
            continue;
        }
        parse_rc = ym_api_parse_track_from_object(track_node, &entry);
        if (parse_rc != 0) {
            /* rc == 1: item without a usable id — skipped by design. */
            continue;
        }
        if (parsed >= max_items || !items) {
            continue;
        }
        memset(&items[parsed], 0, sizeof(items[parsed]));
        items[parsed].track = entry;
        snprintf(items[parsed].batch_id, sizeof(items[parsed].batch_id),
                 "%s", batch_id ? batch_id : "");
        liked_node = cJSON_GetObjectItemCaseSensitive(elem, "liked");
        items[parsed].liked = rotor_bool_node(liked_node);
        if (ym_api_parse_track_cutout_uri(track_node,
                                          items[parsed].cutout_uri,
                                          sizeof(items[parsed].cutout_uri)) != 0) {
            items[parsed].cutout_uri[0] = '\0';
        }
        parsed++;
    }
    if (out_count) {
        *out_count = parsed;
    }
    return 0;
}

int ym_api_rotor_session_new(YmApiContext *ctx,
                             const char *const *seeds, int seed_count,
                             int include_wave_model,
                             YmRotorSessionInfo *out_session,
                             YmRotorSequenceItem *out_items, int max_items,
                             int *out_count)
{
    static const char url[] = ROTOR_HOST "/rotor/session/new";
    char payload[1024];
    char escaped[YM_ROTOR_SEED_SIZE * 2 + 16];
    NetHttpResponse resp;
    cJSON *root = NULL;
    cJSON *result_obj = NULL;
    YmRotorSessionInfo session;
    int parsed_count = 0;
    int payload_len;
    int seed_index;
    int seed_total;

    if (out_count) {
        *out_count = 0;
    }
    if (rotor_check_context(ctx) != 0 || !out_session ||
        !out_items || max_items <= 0) {
        return -1;
    }
    memset(&session, 0, sizeof(session));

    payload_len = snprintf(payload, sizeof(payload),
                           "{\"includeTracksInResponse\":true,"
                           "\"includeWaveModel\":%s,"
                           "\"interactive\":true,\"seeds\":[",
                           include_wave_model ? "true" : "false");
    if (payload_len < 0 || (size_t)payload_len >= sizeof(payload)) {
        return -1;
    }
    seed_total = (seeds && seed_count > 0) ? seed_count : 1;
    if (seed_total > YM_ROTOR_SEEDS_MAX) {
        seed_total = YM_ROTOR_SEEDS_MAX;
    }
    for (seed_index = 0; seed_index < seed_total; ++seed_index) {
        const char *seed = (seeds && seed_count > 0 && seeds[seed_index])
            ? seeds[seed_index] : YM_ROTOR_DEFAULT_SEED;
        int add_len;

        rotor_json_escape(seed, escaped, sizeof(escaped));
        add_len = snprintf(payload + payload_len, sizeof(payload) - (size_t)payload_len,
                           "%s\"%s\"", (seed_index > 0) ? "," : "", escaped);
        if (add_len < 0 || (size_t)(payload_len + add_len) >= sizeof(payload)) {
            return -1;
        }
        payload_len += add_len;
    }
    {
        int tail_len = snprintf(payload + payload_len,
                                sizeof(payload) - (size_t)payload_len, "]}");
        if (tail_len < 0 || (size_t)(payload_len + tail_len) >= sizeof(payload)) {
            return -1;
        }
        payload_len += tail_len;
    }

    logLine("ym_rotor: session/new start seeds=%d wave_model=%d\n",
            seed_total, include_wave_model ? 1 : 0);
    {
        int post_rc = rotor_post_json(ctx, url, payload, &resp);
        if (post_rc != 0) {
            return rotor_transport_verdict(post_rc);
        }
    }
    if (resp.status_code != 200 || !resp.body || resp.body_size <= 0) {
        int verdict = rotor_status_verdict(resp.status_code);
        logLine("ym_rotor: session/new bad status=%d\n", resp.status_code);
        net_http_response_free(&resp);
        return verdict;
    }

    root = cJSON_ParseWithLength(resp.body, (size_t)resp.body_size);
    net_http_response_free(&resp);
    if (!root) {
        logLine("ym_rotor: session/new parse failed\n");
        return -1;
    }
    result_obj = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsObject(result_obj)) {
        logLine("ym_rotor: session/new no result object\n");
        cJSON_Delete(root);
        return -1;
    }
    if (rotor_copy_id_node(cJSON_GetObjectItemCaseSensitive(result_obj, "radioSessionId"),
                           session.radio_session_id,
                           sizeof(session.radio_session_id),
                           "radioSessionId") != 0 ||
        rotor_copy_id_node(cJSON_GetObjectItemCaseSensitive(result_obj, "batchId"),
                           session.batch_id, sizeof(session.batch_id),
                           "batchId") != 0) {
        cJSON_Delete(root);
        return -1;
    }
    session.terminated = rotor_bool_node(
        cJSON_GetObjectItemCaseSensitive(result_obj, "terminated"));
    session.interactive = rotor_bool_node(
        cJSON_GetObjectItemCaseSensitive(result_obj, "interactive"));
    if (rotor_parse_sequence(result_obj, session.batch_id,
                             out_items, max_items, &parsed_count) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_Delete(root);

    if (session.terminated || parsed_count <= 0) {
        logLine("ym_rotor: session/new unusable terminated=%d tracks=%d\n",
                session.terminated, parsed_count);
        return -1;
    }
    *out_session = session;
    if (out_count) {
        *out_count = parsed_count;
    }
    logLine("ym_rotor: session/new ok tracks=%d\n", parsed_count);
    logger_flush();
    return 0;
}

int ym_api_rotor_session_tracks(YmApiContext *ctx,
                                const char *radio_session_id,
                                const char *const *queue, int queue_count,
                                YmRotorSessionInfo *out_session,
                                YmRotorSequenceItem *out_items, int max_items,
                                int *out_count)
{
    char url[160];
    char payload[8192];
    char escaped[128];
    NetHttpResponse resp;
    cJSON *root = NULL;
    cJSON *result_obj = NULL;
    YmRotorSessionInfo session;
    int parsed_count = 0;
    int payload_len;
    int queue_index;
    int queue_total;

    if (out_count) {
        *out_count = 0;
    }
    if (rotor_check_context(ctx) != 0 || !radio_session_id ||
        !radio_session_id[0] || !out_session || !out_items || max_items <= 0) {
        return -1;
    }
    if (strlen(radio_session_id) >= sizeof(session.radio_session_id)) {
        logLine("ym_rotor: session id too long, rejected\n");
        return -1;
    }
    memset(&session, 0, sizeof(session));
    snprintf(session.radio_session_id, sizeof(session.radio_session_id),
             "%s", radio_session_id);

    snprintf(url, sizeof(url), ROTOR_HOST "/rotor/session/%s/tracks",
             radio_session_id);
    url[sizeof(url) - 1] = '\0';

    payload_len = snprintf(payload, sizeof(payload),
                           "{\"feedbacks\":[],\"queue\":[");
    if (payload_len < 0 || (size_t)payload_len >= sizeof(payload)) {
        return -1;
    }
    queue_total = (queue && queue_count > 0) ? queue_count : 0;
    if (queue_total > YM_ROTOR_QUEUE_MAX) {
        queue_total = YM_ROTOR_QUEUE_MAX;
    }
    for (queue_index = 0; queue_index < queue_total; ++queue_index) {
        int add_len;

        if (!queue[queue_index] || !queue[queue_index][0]) {
            continue;
        }
        rotor_json_escape(queue[queue_index], escaped, sizeof(escaped));
        add_len = snprintf(payload + payload_len,
                           sizeof(payload) - (size_t)payload_len,
                           "%s\"%s\"",
                           (queue_index > 0) ? "," : "", escaped);
        if (add_len < 0 || (size_t)(payload_len + add_len) >= sizeof(payload)) {
            return -1;
        }
        payload_len += add_len;
    }
    {
        int tail_len = snprintf(payload + payload_len,
                                sizeof(payload) - (size_t)payload_len, "]}");
        if (tail_len < 0 || (size_t)(payload_len + tail_len) >= sizeof(payload)) {
            return -1;
        }
        payload_len += tail_len;
    }

    logLine("ym_rotor: session/tracks start queue=%d\n", queue_total);
    {
        int post_rc = rotor_post_json(ctx, url, payload, &resp);
        if (post_rc != 0) {
            return rotor_transport_verdict(post_rc);
        }
    }
    if (resp.status_code != 200 || !resp.body || resp.body_size <= 0) {
        int verdict = rotor_status_verdict(resp.status_code);
        logLine("ym_rotor: session/tracks bad status=%d\n", resp.status_code);
        net_http_response_free(&resp);
        return verdict;
    }

    root = cJSON_ParseWithLength(resp.body, (size_t)resp.body_size);
    net_http_response_free(&resp);
    if (!root) {
        logLine("ym_rotor: session/tracks parse failed\n");
        return -1;
    }
    result_obj = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsObject(result_obj)) {
        logLine("ym_rotor: session/tracks no result object\n");
        cJSON_Delete(root);
        return -1;
    }
    session.unknown_session = rotor_bool_node(
        cJSON_GetObjectItemCaseSensitive(result_obj, "unknownSession"));
    if (session.unknown_session) {
        logLine("ym_rotor: session/tracks unknownSession\n");
        logger_flush();
        *out_session = session;
        cJSON_Delete(root);
        return -1;
    }
    if (rotor_copy_id_node(cJSON_GetObjectItemCaseSensitive(result_obj, "batchId"),
                           session.batch_id, sizeof(session.batch_id),
                           "batchId") != 0) {
        cJSON_Delete(root);
        return -1;
    }
    session.terminated = rotor_bool_node(
        cJSON_GetObjectItemCaseSensitive(result_obj, "terminated"));
    if (rotor_parse_sequence(result_obj, session.batch_id,
                             out_items, max_items, &parsed_count) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_Delete(root);

    *out_session = session;
    if (out_count) {
        *out_count = parsed_count;
    }
    logLine("ym_rotor: session/tracks ok tracks=%d terminated=%d\n",
            parsed_count, session.terminated);
    logger_flush();
    return 0;
}

/* Shared feedback POST. batch_id == NULL omits it (radioStarted only).
 * HTTP 200 with a "result" object is success; 200 without "result" is a
 * protocol error. */
static int rotor_post_feedback(const YmApiContext *ctx,
                               const char *radio_session_id,
                               const char *batch_id,
                               const char *event_body)
{
    char url[160];
    char payload[1024];
    char batch_escaped[YM_ROTOR_BATCH_ID_SIZE * 2 + 16];
    NetHttpResponse resp;
    cJSON *root = NULL;
    cJSON *result_obj = NULL;
    int payload_len;
    int ok = 0;

    if (rotor_check_context(ctx) != 0 || !radio_session_id ||
        !radio_session_id[0] || !event_body) {
        return -1;
    }
    snprintf(url, sizeof(url), ROTOR_HOST "/rotor/session/%s/feedback",
             radio_session_id);
    url[sizeof(url) - 1] = '\0';

    if (batch_id && batch_id[0]) {
        rotor_json_escape(batch_id, batch_escaped, sizeof(batch_escaped));
        payload_len = snprintf(payload, sizeof(payload),
                               "{\"from\":\"%s\",\"batchId\":\"%s\",\"event\":{%s}}",
                               YM_ROTOR_FROM, batch_escaped, event_body);
    } else {
        payload_len = snprintf(payload, sizeof(payload),
                               "{\"from\":\"%s\",\"event\":{%s}}",
                               YM_ROTOR_FROM, event_body);
    }
    if (payload_len < 0 || (size_t)payload_len >= sizeof(payload)) {
        return -1;
    }

    {
        int post_rc = rotor_post_json(ctx, url, payload, &resp);
        if (post_rc != 0) {
            return rotor_transport_verdict(post_rc);
        }
    }
    if (resp.status_code != 200 || !resp.body || resp.body_size <= 0) {
        int verdict = rotor_status_verdict(resp.status_code);
        logLine("ym_rotor: feedback bad status=%d\n", resp.status_code);
        net_http_response_free(&resp);
        return verdict;
    }
    root = cJSON_ParseWithLength(resp.body, (size_t)resp.body_size);
    net_http_response_free(&resp);
    if (!root) {
        logLine("ym_rotor: feedback parse failed\n");
        return -1;
    }
    result_obj = cJSON_GetObjectItemCaseSensitive(root, "result");
    ok = cJSON_IsObject(result_obj) ? 0 : -1;
    if (ok != 0) {
        logLine("ym_rotor: feedback 200 without result object\n");
    }
    cJSON_Delete(root);
    return ok;
}

static int rotor_feedback_timestamp(char *out, size_t out_size)
{
    int ts_rc = ym_api_rotor_timestamp_utc(out, out_size);

    if (ts_rc > 0) {
        logLine("ym_rotor: clock unavailable, epoch timestamp used\n");
    }
    return (ts_rc < 0) ? -1 : 0;
}

int ym_api_rotor_feedback_radio_started(YmApiContext *ctx,
                                        const char *radio_session_id)
{
    char timestamp[YM_ROTOR_TIMESTAMP_SIZE];
    char event_body[256];
    int event_len;

    if (rotor_feedback_timestamp(timestamp, sizeof(timestamp)) != 0) {
        return -1;
    }
    event_len = snprintf(event_body, sizeof(event_body),
                         "\"timestamp\":\"%s\",\"type\":\"radioStarted\","
                         "\"from\":\"%s\"",
                         timestamp, YM_ROTOR_FROM);
    if (event_len < 0 || (size_t)event_len >= sizeof(event_body)) {
        return -1;
    }
    return rotor_post_feedback(ctx, radio_session_id, NULL, event_body);
}

static int rotor_track_event(const YmApiContext *ctx,
                             const char *radio_session_id,
                             const char *batch_id,
                             const char *track_id, int album_id,
                             const char *event_type,
                             double total_played_s, int has_total,
                             double track_len_s)
{
    char timestamp[YM_ROTOR_TIMESTAMP_SIZE];
    char composite[LIST_INDEX_ID_SIZE + 16];
    char event_body[512];
    int event_len;

    if (!batch_id || !batch_id[0] || !track_id || !track_id[0]) {
        return -1;
    }
    if (rotor_feedback_timestamp(timestamp, sizeof(timestamp)) != 0) {
        return -1;
    }
    if (ym_api_rotor_composite_id(track_id, album_id,
                                  composite, sizeof(composite)) != 0) {
        return -1;
    }
    if (has_total) {
        event_len = snprintf(event_body, sizeof(event_body),
                             "\"timestamp\":\"%s\",\"type\":\"%s\","
                             "\"totalPlayedSeconds\":%.3f,"
                             "\"trackLengthSeconds\":%.3f,"
                             "\"trackId\":\"%s\"",
                             timestamp, event_type,
                             total_played_s, track_len_s, composite);
    } else {
        event_len = snprintf(event_body, sizeof(event_body),
                             "\"timestamp\":\"%s\",\"type\":\"%s\","
                             "\"trackLengthSeconds\":%.3f,"
                             "\"trackId\":\"%s\"",
                             timestamp, event_type, track_len_s, composite);
    }
    if (event_len < 0 || (size_t)event_len >= sizeof(event_body)) {
        return -1;
    }
    return rotor_post_feedback(ctx, radio_session_id, batch_id, event_body);
}

int ym_api_rotor_feedback_track_started(YmApiContext *ctx,
                                        const char *radio_session_id,
                                        const char *batch_id,
                                        const char *track_id, int album_id,
                                        double track_len_s)
{
    return rotor_track_event(ctx, radio_session_id, batch_id, track_id,
                             album_id, "trackStarted", 0.0, 0, track_len_s);
}

int ym_api_rotor_feedback_track_finished(YmApiContext *ctx,
                                         const char *radio_session_id,
                                         const char *batch_id,
                                         const char *track_id, int album_id,
                                         double total_played_s,
                                         double track_len_s)
{
    return rotor_track_event(ctx, radio_session_id, batch_id, track_id,
                             album_id, "trackFinished",
                             total_played_s, 1, track_len_s);
}

int ym_api_rotor_feedback_skip(YmApiContext *ctx,
                               const char *radio_session_id,
                               const char *batch_id,
                               const char *track_id, int album_id,
                               double total_played_s,
                               double track_len_s)
{
    return rotor_track_event(ctx, radio_session_id, batch_id, track_id,
                             album_id, "skip", total_played_s, 1, track_len_s);
}

int ym_api_rotor_feedback_like(YmApiContext *ctx,
                               const char *radio_session_id,
                               const char *batch_id,
                               const char *track_id, int album_id,
                               double track_len_s)
{
    return rotor_track_event(ctx, radio_session_id, batch_id, track_id,
                             album_id, "like", 0.0, 0, track_len_s);
}

int ym_api_rotor_feedback_unlike(YmApiContext *ctx,
                                 const char *radio_session_id,
                                 const char *batch_id,
                                 const char *track_id, int album_id,
                                 double track_len_s)
{
    return rotor_track_event(ctx, radio_session_id, batch_id, track_id,
                             album_id, "unlike", 0.0, 0, track_len_s);
}

int ym_api_rotor_play_audio(YmApiContext *ctx, int uid,
                            const char *track_id, int album_id,
                            const char *play_id, int from_cache,
                            const char *timestamp_utc,
                            const char *client_now_utc,
                            double track_len_s)
{
    static const char url[] = ROTOR_HOST "/play-audio";
    char body[1024];
    char track_enc[128];
    char play_enc[128];
    char ts_enc[128];
    char now_enc[128];
    NetHttpResponse resp;
    cJSON *root = NULL;
    cJSON *result_node = NULL;
    int body_len;
    int ok = -1;

    if (rotor_check_context(ctx) != 0 || uid <= 0 || !track_id ||
        !track_id[0] || !play_id || !play_id[0] || !timestamp_utc ||
        !client_now_utc) {
        return -1;
    }
    if (rotor_url_encode_component(track_id, track_enc, sizeof(track_enc)) != 0 ||
        rotor_url_encode_component(play_id, play_enc, sizeof(play_enc)) != 0 ||
        rotor_url_encode_component(timestamp_utc, ts_enc, sizeof(ts_enc)) != 0 ||
        rotor_url_encode_component(client_now_utc, now_enc, sizeof(now_enc)) != 0) {
        return -1;
    }
    body_len = snprintf(body, sizeof(body),
                        "track-id=%s&from-cache=%s&from=yandex-music-psp"
                        "&play-id=%s&uid=%d&timestamp=%s"
                        "&track-length-seconds=%.3f"
                        "&total-played-seconds=0&end-position-seconds=0"
                        "&album-id=%d&client-now=%s",
                        track_enc, from_cache ? "true" : "false",
                        play_enc, uid, ts_enc, track_len_s,
                        album_id, now_enc);
    if (body_len < 0 || (size_t)body_len >= sizeof(body)) {
        return -1;
    }

    memset(&resp, 0, sizeof(resp));
    {
        int req_rc = http_request(&(HttpRequest){
            .method = HTTP_POST,
            .url = url,
            .token = ctx->oauth_token,
            .content_type = "application/x-www-form-urlencoded",
            .payload = body,
            .payload_size = body_len,
            .initial_buffer = HTTP_TLS_BUFFER_SMALL,
        }, &resp);
        if (req_rc != 0) {
            logLine("ym_rotor: play-audio transport failed\n");
            net_http_response_free(&resp);
            return rotor_transport_verdict(req_rc);
        }
    }
    if (resp.status_code != 200 || !resp.body || resp.body_size <= 0) {
        int verdict = rotor_status_verdict(resp.status_code);
        logLine("ym_rotor: play-audio bad status=%d\n", resp.status_code);
        net_http_response_free(&resp);
        return verdict;
    }
    root = cJSON_ParseWithLength(resp.body, (size_t)resp.body_size);
    net_http_response_free(&resp);
    if (!root) {
        logLine("ym_rotor: play-audio parse failed\n");
        return -1;
    }
    result_node = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsString(result_node) && result_node->valuestring &&
        strcmp(result_node->valuestring, "ok") == 0) {
        ok = 0;
        logLine("ym_rotor: play-audio ok\n");
        logger_flush();
    } else {
        logLine("ym_rotor: play-audio rejected (result != ok)\n");
    }
    cJSON_Delete(root);
    return ok;
}

static int rotor_library_post(YmApiContext *ctx, int uid,
                              const char *collection, const char *operation,
                              const char *track_id)
{
    char url[256];
    char body[128];
    char track_enc[128];
    NetHttpResponse resp;
    int body_len;
    int req_rc;
    int rc = -1;

    if (rotor_check_context(ctx) != 0 || uid <= 0 || !track_id ||
        !track_id[0]) {
        return -1;
    }
    if (rotor_url_encode_component(track_id, track_enc, sizeof(track_enc)) != 0) {
        return -1;
    }
    snprintf(url, sizeof(url), ROTOR_HOST "/users/%d/%s/tracks/%s",
             uid, collection, operation);
    url[sizeof(url) - 1] = '\0';
    body_len = snprintf(body, sizeof(body), "track-ids=%s", track_enc);
    if (body_len < 0 || (size_t)body_len >= sizeof(body)) {
        return -1;
    }

    memset(&resp, 0, sizeof(resp));
    req_rc = http_request(&(HttpRequest){
        .method = HTTP_POST,
        .url = url,
        .token = ctx->oauth_token,
        .content_type = "application/x-www-form-urlencoded",
        .payload = body,
        .payload_size = body_len,
        .initial_buffer = HTTP_TLS_BUFFER_SMALL,
    }, &resp);
    if (req_rc != 0) {
        int verdict = rotor_transport_verdict(req_rc);
        logLine("ym_rotor: library %s/%s transport failed rc=%d\n",
                collection, operation, req_rc);
        net_http_response_free(&resp);
        return verdict;
    }
    logLine("ym_rotor: library %s/%s status=%d\n",
            collection, operation, resp.status_code);
    logger_flush();
    rc = (resp.status_code == 200) ? 0 : rotor_status_verdict(resp.status_code);
    net_http_response_free(&resp);
    return rc;
}

int ym_api_track_unlike(YmApiContext *ctx, int uid, const char *track_id)
{
    return rotor_library_post(ctx, uid, "likes", "remove", track_id);
}

int ym_api_track_dislike(YmApiContext *ctx, int uid, const char *track_id)
{
    return rotor_library_post(ctx, uid, "dislikes", "add-multiple", track_id);
}

int ym_api_track_undislike(YmApiContext *ctx, int uid, const char *track_id)
{
    return rotor_library_post(ctx, uid, "dislikes", "remove", track_id);
}

int ym_api_rotor_resolve_uid(YmApiContext *ctx, int *out_uid)
{
    static const char url[] = ROTOR_HOST "/account/status";
    NetHttpResponse resp;
    UserInfo info;
    int req_rc;

    if (out_uid) {
        *out_uid = 0;
    }
    if (rotor_check_context(ctx) != 0 || !out_uid) {
        return -1;
    }
    memset(&resp, 0, sizeof(resp));
    req_rc = http_request(&(HttpRequest){
        .method = HTTP_GET,
        .url = url,
        .token = ctx->oauth_token,
        .initial_buffer = HTTP_TLS_BUFFER_SMALL,
    }, &resp);
    if (req_rc != 0) {
        int verdict = rotor_transport_verdict(req_rc);
        logLine("ym_rotor: account/status transport failed rc=%d\n", req_rc);
        net_http_response_free(&resp);
        return verdict;
    }
    if (resp.status_code != 200 || !resp.body || resp.body_size <= 0) {
        int verdict = rotor_status_verdict(resp.status_code);
        logLine("ym_rotor: account/status bad status=%d\n", resp.status_code);
        net_http_response_free(&resp);
        return verdict;
    }
    memset(&info, 0, sizeof(info));
    if (api_parser_account_status(resp.body, (size_t)resp.body_size, &info) != 0 ||
        info.uid <= 0) {
        logLine("ym_rotor: account/status parse failed\n");
        net_http_response_free(&resp);
        return -1;
    }
    net_http_response_free(&resp);
    *out_uid = info.uid;
    logLine("ym_rotor: account uid resolved\n");
    logger_flush();
    return 0;
}

int ym_api_rotor_exchange_music_token(YmApiContext *ctx,
                                      char *out_music_token,
                                      size_t out_music_token_size,
                                      int *out_uid)
{
    int uid = 0;

    if (out_uid) {
        *out_uid = 0;
    }
    if (!out_music_token || out_music_token_size == 0) {
        return -1;
    }
    out_music_token[0] = '\0';
    if (ym_api_rotor_resolve_uid(ctx, &uid) != 0) {
        return -1;
    }
    if (ctx->oauth_token && strlen(ctx->oauth_token) < out_music_token_size) {
        snprintf(out_music_token, out_music_token_size, "%s", ctx->oauth_token);
    } else {
        return -1;
    }
    if (out_uid) {
        *out_uid = uid;
    }
    logLine("ym_rotor: music token exchanged\n");
    logger_flush();
    return 0;
}
