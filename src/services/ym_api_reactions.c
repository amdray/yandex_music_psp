#include "services/ym_api_reactions.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/json.h"
#include "core/libc_runtime.h"
#include "core/logger.h"
#include <sys/select.h>
#include <curl/curl.h>
#include "services/net_http.h"

#define API_HOST "https://api.music.yandex.net"

void ym_reactions_free(YmReactions *reactions)
{
    free(reactions->entries);
    memset(reactions, 0, sizeof(*reactions));
}

static size_t find_entry(const YmReactions *reactions, const char *id)
{
    size_t i;
    for (i = 0; i < reactions->count; ++i) {
        if (strcmp(reactions->entries[i].track_id, id) == 0) return i;
    }
    return reactions->count;
}

YmReaction ym_reactions_get(const YmReactions *reactions, const char *track_id)
{
    size_t i;
    if (!reactions->loaded || !track_id || !track_id[0])
        return YM_REACTION_UNKNOWN;
    i = find_entry(reactions, track_id);
    return i < reactions->count
        ? (YmReaction)reactions->entries[i].reaction : YM_REACTION_NONE;
}

static int valid_revision(const cJSON *node)
{
    return cJSON_IsNumber(node) && isfinite(node->valuedouble) &&
           node->valuedouble >= 0 && floor(node->valuedouble) == node->valuedouble;
}

int ym_reactions_parse(YmReactions *reactions, const char *json, size_t length)
{
    cJSON *root = ym_json_parse_len(json, length, NULL);
    cJSON *result, *values, *tracks, *info, *revision, *lists, *liked, *disliked;
    cJSON *node;
    YmReactions next = {0};
    size_t count;
    int rc = YM_ERR_PARSE;
    if (!root) return rc;
    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    values = cJSON_GetObjectItemCaseSensitive(result, "values");
    tracks = cJSON_GetObjectItemCaseSensitive(values, "likedTracks");
    info = cJSON_GetObjectItemCaseSensitive(tracks, "info");
    revision = cJSON_GetObjectItemCaseSensitive(info, "revision");
    if (!valid_revision(revision)) goto done;
    lists = cJSON_GetObjectItemCaseSensitive(tracks, "values");
    if (!lists) {
        /* Observed unchanged-revision response has info but no values. */
        if (reactions->loaded && reactions->revision == revision->valuedouble)
            rc = YM_OK;
        goto done;
    }
    liked = cJSON_GetObjectItemCaseSensitive(lists, "liked");
    disliked = cJSON_GetObjectItemCaseSensitive(lists, "disliked");
    if (!cJSON_IsArray(liked) || !cJSON_IsArray(disliked)) goto done;
    count = (size_t)cJSON_GetArraySize(liked) + (size_t)cJSON_GetArraySize(disliked);
    /* One spare entry permits a newly liked track without allocating after
     * the server has already accepted the mutation. */
    if (count >= SIZE_MAX / sizeof(*next.entries)) goto done;
    next.capacity = count + 1;
    next.entries = malloc(next.capacity * sizeof(*next.entries));
    if (!next.entries) { rc = YM_ERR_NO_MEMORY; goto done; }
    cJSON_ArrayForEach(node, liked) {
        YmReactionEntry *entry = &next.entries[next.count];
        if (ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(node, "trackId"),
                                entry->track_id, sizeof(entry->track_id)) < 0 ||
            !entry->track_id[0]) goto done;
        entry->reaction = YM_REACTION_LIKE;
        next.count++;
    }
    cJSON_ArrayForEach(node, disliked) {
        YmReactionEntry *entry = &next.entries[next.count];
        if (ym_json_copy_string(cJSON_GetObjectItemCaseSensitive(node, "trackId"),
                                entry->track_id, sizeof(entry->track_id)) < 0 ||
            !entry->track_id[0]) goto done;
        entry->reaction = YM_REACTION_DISLIKE;
        next.count++;
    }
    next.loaded = 1;
    next.revision = revision->valuedouble;
    ym_reactions_free(reactions);
    *reactions = next;
    memset(&next, 0, sizeof(next));
    rc = YM_OK;
done:
    ym_reactions_free(&next);
    cJSON_Delete(root);
    return rc;
}

static int request(YmApiContext *ctx, const char *url, const char *content_type,
                   const char *body, NetHttpResponse *response)
{
    int rc;
    memset(response, 0, sizeof(*response));
    if (!ctx || !ctx->oauth_token || !ctx->oauth_token[0]) return YM_ERR_AUTH;
    rc = http_request(&(HttpRequest){
        .method = HTTP_POST, .url = url, .token = ctx->oauth_token,
        .content_type = content_type, .payload = body, .payload_size = -1,
        .initial_buffer = HTTP_TLS_BUFFER_SMALL
    }, response);
    if (rc != 0) return net_http_error_is_network(rc) ? YM_ERR_NETWORK : YM_ERR_HTTP;
    if (response->status_code == 401 || response->status_code == 403) return YM_ERR_AUTH;
    return response->status_code == 200 ? YM_OK : YM_ERR_HTTP;
}

int ym_api_reactions_sync(YmApiContext *ctx, YmReactions *reactions)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *tracks;
    char *body = NULL;
    NetHttpResponse response = {0};
    int rc = YM_ERR_NO_MEMORY;
    if (!root) return rc;
    tracks = cJSON_AddObjectToObject(root, "likedTracks");
    if (!tracks) goto done;
    if (reactions->loaded) {
        if (!cJSON_AddNumberToObject(tracks, "revision", reactions->revision)) goto done;
    } else {
        if (!cJSON_AddBoolToObject(tracks, "allValuesRequired", 1)) goto done;
    }
    rc = ym_libc_lock();
    if (rc < 0) goto done;
    body = cJSON_PrintUnformatted(root);
    rc = ym_libc_unlock();
    if (rc < 0) goto done;
    rc = YM_ERR_NO_MEMORY;
    if (!body) goto done;
    rc = request(ctx, API_HOST "/collection/sync", "application/json", body, &response);
    if (rc == YM_OK)
        rc = ym_reactions_parse(reactions, response.body, (size_t)response.body_size);
    logLine("reactions: sync rc=%d http=%d entries=%u bytes=%d\n",
            rc, response.status_code, (unsigned)reactions->count, response.body_size);
done:
    net_http_response_free(&response);
    cJSON_free(body);
    cJSON_Delete(root);
    return rc;
}

int ym_api_reaction_set(YmApiContext *ctx, int uid, YmReactions *reactions,
                        const char *track_id, YmReactionAction action)
{
    /* UID is a positive int. sizeof(int)*CHAR_BIT bounds its decimal digits;
     * percent encoding needs at most three bytes per input byte. */
    char url[sizeof(API_HOST "/users//dislikes/tracks/add-multiple") + sizeof(int)*CHAR_BIT];
    char *encoded;
    char body[sizeof("track-ids=") + 3 * TRACK_ID_SIZE];
    const char *collection, *operation;
    int add, rc, written;
    size_t index;
    YmReaction value;
    NetHttpResponse response = {0};
    cJSON *root = NULL, *result;
    if (uid <= 0 || !reactions->loaded || !track_id || !track_id[0] ||
        strlen(track_id) >= TRACK_ID_SIZE) return YM_ERR_PARSE;
    switch (action) {
    case YM_REACTION_ADD_LIKE: collection = "likes"; add = 1; value = YM_REACTION_LIKE; break;
    case YM_REACTION_REMOVE_LIKE: collection = "likes"; add = 0; value = YM_REACTION_LIKE; break;
    case YM_REACTION_ADD_DISLIKE: collection = "dislikes"; add = 1; value = YM_REACTION_DISLIKE; break;
    case YM_REACTION_REMOVE_DISLIKE: collection = "dislikes"; add = 0; value = YM_REACTION_DISLIKE; break;
    default: return YM_ERR_PARSE;
    }
    operation = add ? "add-multiple" : "remove";
    written = snprintf(url, sizeof(url), API_HOST "/users/%d/%s/tracks/%s", uid, collection, operation);
    if (written < 0 || (size_t)written >= sizeof(url)) return YM_ERR_PARSE;
    encoded = curl_easy_escape(NULL, track_id, 0);
    if (!encoded) return YM_ERR_NO_MEMORY;
    written = snprintf(body, sizeof(body), "track-ids=%s", encoded);
    curl_free(encoded);
    if (written < 0 || (size_t)written >= sizeof(body)) return YM_ERR_PARSE;
    index = find_entry(reactions, track_id);
    if (add && index == reactions->count && reactions->count == reactions->capacity) {
        YmReactionEntry *entries;
        if (reactions->count >= SIZE_MAX / sizeof(*entries) - 1) return YM_ERR_NO_MEMORY;
        entries = realloc(reactions->entries, (reactions->count + 1) * sizeof(*entries));
        if (!entries) return YM_ERR_NO_MEMORY;
        reactions->entries = entries;
        reactions->capacity = reactions->count + 1;
    }
    rc = request(ctx, url, "application/x-www-form-urlencoded", body, &response);
    if (rc != YM_OK) goto done;
    root = ym_json_parse_len(response.body, (size_t)response.body_size, NULL);
    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!valid_revision(cJSON_GetObjectItemCaseSensitive(result, "revision"))) {
        rc = YM_ERR_PARSE;
        goto done;
    }
    if (add) {
        if (index == reactions->count) {
            strcpy(reactions->entries[index].track_id, track_id);
            reactions->count++;
        }
        reactions->entries[index].reaction = (unsigned char)value;
    } else if (index < reactions->count && reactions->entries[index].reaction == value) {
        reactions->entries[index] = reactions->entries[--reactions->count];
    }
done:
    logLine("reactions: set action=%d track='%s' rc=%d http=%d\n",
            action, track_id, rc, response.status_code);
    cJSON_Delete(root);
    net_http_response_free(&response);
    return rc;
}
