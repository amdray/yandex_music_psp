#include "services/token_loader.h"

#include <string.h>
#include <pspthreadman.h>
#include <mbedtls/platform_util.h>

#include "core/fs.h"
#include "core/logger.h"

static const char s_path[] = "config/token.txt";
static const char s_prefix[] = "YANDEX_TOKEN = \"";
static const char s_suffix[] = "\"\n";
enum {
    TOKEN_RECORD_MAX = sizeof(s_prefix) - 1 + TOKEN_LOADER_CAPACITY - 1
                     + sizeof(s_suffix) - 1
};
static SceLwMutexWorkarea s_mutex;
static int s_initialized;
static char s_cached_token[TOKEN_LOADER_CAPACITY];
static size_t s_cached_len;

int token_loader_init(void)
{
    int rc;
    if (s_initialized) return 0;
    rc = sceKernelCreateLwMutex(&s_mutex, "token_store", 0, 0, NULL);
    if (rc != 0) return rc;
    s_initialized = 1;
    return 0;
}

static int token_lock(void)
{
    int rc;
    if (!s_initialized) return -1;
    rc = sceKernelLockLwMutex(&s_mutex, 1, NULL);
    if (rc != 0) logLine("token: lock failed 0x%08X\n", (unsigned int)rc);
    return rc;
}

static int token_unlock(int result)
{
    int rc = sceKernelUnlockLwMutex(&s_mutex, 1);
    if (rc != 0) {
        logLine("token: unlock failed 0x%08X\n", (unsigned int)rc);
        if (result == 0) result = rc;
    }
    return result;
}

static int token_close(SceUID fd, int result)
{
    int rc = fs_close(fd);
    if (rc < 0) {
        logLine("token: close failed 0x%08X\n", (unsigned int)rc);
        if (result == 0) result = rc;
    }
    return result;
}

static void token_cache_clear(void)
{
    mbedtls_platform_zeroize(s_cached_token, sizeof(s_cached_token));
    s_cached_len = 0;
}

static void token_cache_set(const char *token, size_t len)
{
    token_cache_clear();
    memcpy(s_cached_token, token, len);
    s_cached_len = len;
}

static int token_bytes_valid(const char *token, size_t len)
{
    size_t i;
    if (len == 0 || len >= TOKEN_LOADER_CAPACITY) return 0;
    for (i = 0; i < len; ++i) {
        /* The file stores a single quoted line without escape sequences. */
        if (token[i] == '\0' || token[i] == '"' ||
            token[i] == '\r' || token[i] == '\n') return 0;
    }
    return 1;
}

static int token_read_file(char *out_token, int out_size)
{
    /* One extra byte distinguishes a complete maximum record from overflow. */
    char record[TOKEN_RECORD_MAX + 1];
    size_t used = 0, len;
    int result = 0;
    SceUID fd = fs_open(s_path, PSP_O_RDONLY, 0666);
    if (fd < 0) return (int)fd;
    while (used < sizeof(record)) {
        int count = fs_read(fd, record + used, sizeof(record) - used);
        if (count < 0) {
            result = count;
            break;
        }
        if (count == 0) break;
        used += (size_t)count;
    }
    result = token_close(fd, result);
    if (result != 0) goto done;
    if (used > TOKEN_RECORD_MAX) {
        result = -1;
        goto done;
    }
    if (used > 0 && record[used - 1] == '\n') --used;
    if (used <= sizeof(s_prefix) ||
        memcmp(record, s_prefix, sizeof(s_prefix) - 1) != 0 ||
        record[used - 1] != '"') {
        result = -1;
        goto done;
    }
    len = used - (sizeof(s_prefix) - 1) - 1;
    if (!token_bytes_valid(record + sizeof(s_prefix) - 1, len) ||
        len >= (size_t)out_size) {
        result = -1;
        goto done;
    }
    token_cache_set(record + sizeof(s_prefix) - 1, len);
    memcpy(out_token, s_cached_token, len + 1);
done:
    mbedtls_platform_zeroize(record, sizeof(record));
    return result;
}

int token_loader_read(char *out_token, int out_size)
{
    int result;
    if (!out_token || out_size <= 0) return -1;
    out_token[0] = '\0';
    result = token_lock();
    if (result != 0) return result;
    if (s_cached_len > 0) {
        if (s_cached_len >= (size_t)out_size) result = -1;
        else memcpy(out_token, s_cached_token, s_cached_len + 1);
    } else {
        result = token_read_file(out_token, out_size);
    }
    result = token_unlock(result);
    if (result != 0) {
        out_token[0] = '\0';
        logLine("token: read failed 0x%08X\n", (unsigned int)result);
    }
    return result;
}

int token_loader_save(const char *token)
{
    char record[TOKEN_RECORD_MAX];
    size_t len = 0, size, written = 0;
    int result;
    SceUID fd;
    if (!token) return -1;
    while (len < TOKEN_LOADER_CAPACITY && token[len]) ++len;
    if (!token_bytes_valid(token, len)) return -1;
    result = token_lock();
    if (result != 0) return result;
    memcpy(record, s_prefix, sizeof(s_prefix) - 1);
    memcpy(record + sizeof(s_prefix) - 1, token, len);
    memcpy(record + sizeof(s_prefix) - 1 + len, s_suffix, sizeof(s_suffix) - 1);
    size = sizeof(s_prefix) - 1 + len + sizeof(s_suffix) - 1;
    fd = fs_open(s_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) result = (int)fd;
    else {
        while (written < size) {
            int count = fs_write(fd, record + written, size - written);
            if (count <= 0) {
                result = count < 0 ? count : -1;
                break;
            }
            written += (size_t)count;
        }
        result = token_close(fd, result);
    }
    if (result == 0) token_cache_set(token, len);
    else token_cache_clear();
    mbedtls_platform_zeroize(record, sizeof(record));
    result = token_unlock(result);
    if (result != 0) {
        logLine("token: save failed 0x%08X\n", (unsigned int)result);
    }
    return result;
}

int token_loader_remove(void)
{
    int result = token_lock();
    if (result != 0) return result;
    result = fs_remove(s_path);
    token_cache_clear();
    result = token_unlock(result < 0 ? result : 0);
    if (result != 0) {
        logLine("token: remove failed 0x%08X\n", (unsigned int)result);
    }
    return result;
}
