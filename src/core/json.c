/* Статически проверен и зафиксирован: назначение, границы ответственности,
 * контракты и числовые значения обёртки cJSON. */
#include "core/json.h"
#include "core/libc_runtime.h"

#include <string.h>

cJSON *ym_json_parse_len(const char *text, size_t len, YmJsonError *error)
{
    YmJsonError result = {YM_JSON_OK, 0, 0};
    const char *end = NULL;
    cJSON *root = NULL;
    int rc;

    if (!text || len == 0) {
        result.status = YM_JSON_ARGUMENT_ERROR;
    } else if ((rc = ym_libc_lock()) < 0) {
        result.status = YM_JSON_SYNC_ERROR;
        result.sync_error = rc;
    } else {
        root = cJSON_ParseWithLengthOpts(text, len, &end, 0);
        if (!root) {
            result.status = YM_JSON_PARSE_ERROR;
            if (end) result.offset = (size_t)(end - text);
        } else {
            const char *limit = text + len;
            while (end < limit &&
                   (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
                ++end;
            }
            if (end != limit) {
                result.status = YM_JSON_PARSE_ERROR;
                result.offset = (size_t)(end - text);
                cJSON_Delete(root);
                root = NULL;
            }
        }
        rc = ym_libc_unlock();
        if (rc < 0) {
            cJSON_Delete(root);
            root = NULL;
            result.status = YM_JSON_SYNC_ERROR;
            result.sync_error = rc;
        }
    }
    if (error) *error = result;
    return root;
}

cJSON *ym_json_parse(const char *text, YmJsonError *error)
{
    return ym_json_parse_len(text, text ? strlen(text) : 0, error);
}

int ym_json_copy_string(const cJSON *node, char *out, size_t size)
{
    size_t length;
    if (!out || !cJSON_IsString(node) || !node->valuestring) return -1;
    length = strlen(node->valuestring);
    if (length >= size) return -1;
    memcpy(out, node->valuestring, length + 1);
    return 0;
}
