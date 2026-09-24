#include "services/ym_api_genres.h"

#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "core/logger.h"
#include "services/net_http.h"

#define YM_GENRES_URL "https://api.music.yandex.net/genres"
#define YM_GENRE_MAX_ENTRIES 512
#define YM_GENRE_MAX_DEPTH 16
#define YM_GENRE_ID_SIZE 48
#define YM_GENRE_TITLE_SIZE 96

typedef struct YmGenreEntry {
    char id[YM_GENRE_ID_SIZE];
    char title[YM_GENRE_TITLE_SIZE];
} YmGenreEntry;

static YmGenreEntry *s_entries;
static int s_entry_count;

static const char *json_string(const cJSON *object, const char *key)
{
    const cJSON *node;
    if (!cJSON_IsObject(object)) return NULL;
    node = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(node) && node->valuestring && node->valuestring[0]
               ? node->valuestring : NULL;
}

static const char *genre_title(const cJSON *item, const char *language)
{
    const cJSON *titles;
    const cJSON *localized;

    titles = cJSON_GetObjectItemCaseSensitive(item, "titles");
    localized = cJSON_IsObject(titles)
                    ? cJSON_GetObjectItemCaseSensitive(titles, language)
                    : NULL;
    return json_string(localized, "title");
}

static int collect_genres(const cJSON *items, const char *language,
                          YmGenreEntry *entries, int *count, int depth)
{
    const cJSON *item;

    if (!cJSON_IsArray(items) || !entries || !count || depth > YM_GENRE_MAX_DEPTH)
        return -1;

    cJSON_ArrayForEach(item, items) {
        const char *id;
        const char *title;
        const cJSON *children;
        size_t id_len;
        size_t title_len;

        if (!cJSON_IsObject(item)) continue;
        id = json_string(item, "id");
        title = genre_title(item, language);
        if (id && title) {
            id_len = strlen(id);
            title_len = strlen(title);
            if (id_len < YM_GENRE_ID_SIZE && title_len < YM_GENRE_TITLE_SIZE) {
                if (*count >= YM_GENRE_MAX_ENTRIES) return -1;
                memcpy(entries[*count].id, id, id_len + 1);
                memcpy(entries[*count].title, title, title_len + 1);
                (*count)++;
            } else {
                logLine("ym_genres: skipped oversized entry id_len=%u title_len=%u\n",
                        (unsigned int)id_len, (unsigned int)title_len);
            }
        }

        children = cJSON_GetObjectItemCaseSensitive(item, "subGenres");
        if (!cJSON_IsArray(children)) {
            children = cJSON_GetObjectItemCaseSensitive(item, "sub_genres");
        }
        if (cJSON_IsArray(children) &&
            collect_genres(children, language, entries, count, depth + 1) != 0)
            return -1;
    }
    return 0;
}

static int compare_genres(const void *left, const void *right)
{
    const YmGenreEntry *a = (const YmGenreEntry *)left;
    const YmGenreEntry *b = (const YmGenreEntry *)right;
    return strcmp(a->id, b->id);
}

int ym_api_genres_load(const char *token, const char *language)
{
    NetHttpResponse response;
    cJSON *root = NULL;
    cJSON *result;
    YmGenreEntry *entries = NULL;
    int count = 0;
    int rc = -1;

    if (!token || !token[0] || !language || !language[0]) return -1;
    memset(&response, 0, sizeof(response));
    if (http_request(&(HttpRequest){
            .method = HTTP_GET,
            .url = YM_GENRES_URL,
            .token = token,
            .initial_buffer = HTTP_TLS_BUFFER_MEDIUM,
            .identity_encoding = 1
        }, &response) != 0) {
        logLine("ym_genres: request failed\n");
        return -1;
    }
    if (response.status_code != 200 || !response.body || response.body_size <= 0) {
        logLine("ym_genres: invalid response status=%d body=%d\n",
                response.status_code, response.body_size);
        goto done;
    }

    root = cJSON_ParseWithLength(response.body, (size_t)response.body_size);
    result = root ? cJSON_GetObjectItemCaseSensitive(root, "result") : NULL;
    if (!cJSON_IsArray(result)) {
        logLine("ym_genres: result is not an array\n");
        goto done;
    }

    entries = (YmGenreEntry *)calloc(YM_GENRE_MAX_ENTRIES, sizeof(*entries));
    if (!entries) {
        logLine("ym_genres: catalog allocation failed\n");
        goto done;
    }
    if (collect_genres(result, language, entries, &count, 0) != 0 || count <= 0) {
        logLine("ym_genres: catalog parse failed count=%d\n", count);
        goto done;
    }
    qsort(entries, (size_t)count, sizeof(*entries), compare_genres);
    {
        YmGenreEntry *shrunk = (YmGenreEntry *)realloc(
            entries, (size_t)count * sizeof(*entries));
        if (shrunk) entries = shrunk;
    }

    free(s_entries);
    s_entries = entries;
    s_entry_count = count;
    entries = NULL;
    logLine("ym_genres: loaded entries=%d body=%d lang=%s\n",
            count, response.body_size, language);
    rc = 0;

done:
    free(entries);
    cJSON_Delete(root);
    net_http_response_free(&response);
    return rc;
}

int ym_api_genres_resolve(const char *id, char *out, size_t out_size)
{
    int low = 0;
    int high = s_entry_count - 1;

    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    if (!id || !id[0]) return -1;

    while (low <= high) {
        int middle = low + (high - low) / 2;
        int order = strcmp(id, s_entries[middle].id);
        if (order == 0) {
            size_t length = strlen(s_entries[middle].title);
            if (length >= out_size) return -1;
            memcpy(out, s_entries[middle].title, length + 1);
            return 0;
        }
        if (order < 0) high = middle - 1;
        else low = middle + 1;
    }
    return -1;
}
