#ifndef YM_CORE_JSON_H
#define YM_CORE_JSON_H

#include <stddef.h>
#include <cjson/cJSON.h>

typedef enum {
    YM_JSON_OK,
    YM_JSON_ARGUMENT_ERROR,
    YM_JSON_SYNC_ERROR,
    YM_JSON_PARSE_ERROR
} YmJsonStatus;

typedef struct {
    YmJsonStatus status;
    int sync_error;
    size_t offset;
} YmJsonError;

/* Caller owns the returned tree (cJSON_Delete). error may be NULL.
 * Parse exactly one JSON value, allowing only JSON whitespace after it.
 * len counts data bytes, excluding any terminating NUL.
 * offset is the parse failure byte offset, not a pointer into the input.
 * cJSON cannot distinguish invalid input from allocation failure.
 * All parsing must use this module; do not use cJSON_GetErrorPtr. */
cJSON *ym_json_parse(const char *text, YmJsonError *error);
cJSON *ym_json_parse_len(const char *text, size_t len, YmJsonError *error);

/* Copy a complete JSON string; return -1 for wrong type or insufficient space. */
int ym_json_copy_string(const cJSON *node, char *out, size_t size);

#endif
