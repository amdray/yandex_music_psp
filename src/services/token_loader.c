#include "services/token_loader.h"

#include <string.h>

#include "core/fs.h"
#include "core/logger.h"

int token_loader_read(char *out_token, int out_size)
{
    static char s_cached_token[256];
    static int s_cached_len = 0;

    if (!out_token || out_size <= 0) {
        return -1;
    }

    /* Token is immutable for app runtime: read once from disk, then serve from RAM. */
    if (s_cached_len > 0) {
        if (s_cached_len >= out_size) {
            logLine("token: cached token does not fit buffer\n");
            return -1;
        }
        memcpy(out_token, s_cached_token, (size_t)s_cached_len + 1);
        return 0;
    }

    SceUID fd = fs_open("config/token.txt", PSP_O_RDONLY, 0666);
    if (fd < 0) {
        logLine("token: open failed\n");
        return -1;
    }

    char buf[256];
    int read_bytes = fs_read(fd, buf, sizeof(buf) - 1);
    fs_close(fd);
    
    if (read_bytes <= 0) {
        logLine("token: read failed\n");
        return -1;
    }
    buf[read_bytes] = '\0';
    
    // Найти первую строку (до \n или \r\n)
    buf[strcspn(buf, "\r\n")] = '\0';

    const char *start = strstr(buf, "YANDEX_TOKEN");
    if (!start) {
        logLine("token: missing key\n");
        return -1;
    }
    start = strchr(start, '"');
    if (!start) {
        logLine("token: missing quote\n");
        return -1;
    }
    start++;
    const char *end = strchr(start, '"');
    if (!end) {
        logLine("token: missing end quote\n");
        return -1;
    }

    int len = (int)(end - start);
    if (len <= 0 || len >= out_size) {
        logLine("token: invalid length\n");
        return -1;
    }

    if (len >= (int)sizeof(s_cached_token)) {
        logLine("token: too long for cache\n");
        return -1;
    }

    memcpy(s_cached_token, start, (size_t)len);
    s_cached_token[len] = '\0';
    s_cached_len = len;

    memcpy(out_token, s_cached_token, (size_t)s_cached_len + 1);
    logLine("token: ok len=%d\n", len);
    return 0;
}
