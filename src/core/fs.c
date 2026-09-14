#include "core/fs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>

#include "core/logger.h"

// --- From old fs.c ---

/* Use Lightweight Mutex instead of Sema for better performance.
 * According to PSPSDK best practices, LwMutex is preferred for simple locks.
 * It's more efficient and has built-in priority inheritance protection.
 */
static SceLwMutexWorkarea fs_mutex;
static int fs_initialized = 0;

/* Initialize filesystem mutex.
 * Must be called from main() before any threads are created.
 * Returns 0 on success, < 0 on error.
 */
int fs_init(void)
{
    if (fs_initialized) {
        return 0;  // Already initialized
    }
    
    // Use a plain lightweight mutex; current fs operations do not re-enter fs while locked.
    int ret = sceKernelCreateLwMutex(&fs_mutex, "fs_mutex", 
                                     0, 0, NULL);
    if (ret < 0) {
        logLine("fs: failed to create mutex: %d\n", ret);
        return ret;
    }
    logLine("fs: mutex initialized\n");
    fs_initialized = 1;
    return 0;
}

/* Shutdown filesystem subsystem.
 * Must be called from main() during cleanup.
 */
void fs_shutdown(void)
{
    if (fs_initialized) {
        sceKernelDeleteLwMutex(&fs_mutex);
        logLine("fs: mutex destroyed\n");
        fs_initialized = 0;
    }
}

static int fs_lock_internal(void)
{
    if (!fs_initialized) {
        logLine("fs: ERROR - mutex not initialized! Call fs_init() first.\n");
        return -1;
    }
    sceKernelLockLwMutex(&fs_mutex, 1, NULL);
    return 0;
}

static void fs_unlock_internal(void)
{
    if (fs_initialized) {
        sceKernelUnlockLwMutex(&fs_mutex, 1);
    }
}

static int is_abs_path(const char *path)
{
    if (!path || !path[0]) {
        return 0;
    }
    if (strchr(path, ':')) {
        return 1;
    }
    if (path[0] == '/') {
        return 1;
    }
    return 0;
}

SceUID fs_open(const char *path, int flags, int mode)
{
    SceUID fd;
    char abs_path[512];
    const char *use_path = path;
    
    fs_log_open(path);

    if (fs_make_abs_path(path, abs_path, sizeof(abs_path)) == 0) {
        use_path = abs_path;
    }
    
    fs_lock_internal();
    fd = sceIoOpen(use_path, flags, mode);
    fs_unlock_internal();
    return fd;
}

int fs_close(SceUID fd)
{
    int rc;
    fs_lock_internal();
    rc = sceIoClose(fd);
    fs_unlock_internal();
    return rc;
}

int fs_read(SceUID fd, void *buf, size_t size)
{
    int rc;
    fs_lock_internal();
    rc = sceIoRead(fd, buf, size);
    fs_unlock_internal();
    return rc;
}

int fs_write(SceUID fd, const void *buf, size_t size)
{
    int rc;
    fs_lock_internal();
    rc = sceIoWrite(fd, buf, size);
    fs_unlock_internal();
    return rc;
}

SceOff fs_lseek(SceUID fd, SceOff offset, int whence)
{
    SceOff rc;
    fs_lock_internal();
    rc = sceIoLseek(fd, offset, whence);
    fs_unlock_internal();
    return rc;
}

int fs_getstat(const char *path, SceIoStat *st)
{
    int rc;
    char abs_path[512];
    const char *use_path = path;

    if (fs_make_abs_path(path, abs_path, sizeof(abs_path)) == 0) {
        use_path = abs_path;
    }

    fs_lock_internal();
    rc = sceIoGetstat(use_path, st);
    fs_unlock_internal();
    return rc;
}

int fs_remove(const char *path)
{
    int rc;
    fs_lock_internal();
    rc = sceIoRemove(path);
    fs_unlock_internal();
    return rc;
}

int fs_rename(const char *old_path, const char *new_path)
{
    int rc;
    char abs_old_path[512];
    char abs_new_path[512];
    const char *use_old_path = old_path;
    const char *use_new_path = new_path;

    if (fs_make_abs_path(old_path, abs_old_path, sizeof(abs_old_path)) == 0) {
        use_old_path = abs_old_path;
    }
    if (fs_make_abs_path(new_path, abs_new_path, sizeof(abs_new_path)) == 0) {
        use_new_path = abs_new_path;
    }

    fs_lock_internal();
    rc = sceIoRename(use_old_path, use_new_path);
    fs_unlock_internal();
    return rc;
}

/* Remove every entry in `dir` whose name starts with `prefix`, except `keep_name`.
   Names are collected before removal so deleting does not invalidate the open
   directory iterator. Returns the number removed, or -1 if the dir can't be read. */
int fs_remove_siblings(const char *dir, const char *prefix, const char *keep_name)
{
    if (!dir || !prefix) {
        return -1;
    }

    char abs_dir[512];
    const char *use_dir = dir;
    if (fs_make_abs_path(dir, abs_dir, sizeof(abs_dir)) == 0) {
        use_dir = abs_dir;
    }

    fs_lock_internal();
    SceUID dfd = sceIoDopen(use_dir);
    if (dfd < 0) {
        fs_unlock_internal();
        return -1;
    }

    char victims[16][256];   /* sized to SceIoDirent.d_name[256] — no truncation possible */
    int nv = 0;
    size_t plen = strlen(prefix);
    SceIoDirent ent;
    while (nv < 16) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(dfd, &ent) <= 0) {
            break;
        }
        if (strncmp(ent.d_name, prefix, plen) != 0) {
            continue;
        }
        if (keep_name && strcmp(ent.d_name, keep_name) == 0) {
            continue;
        }
        snprintf(victims[nv], sizeof(victims[nv]), "%s", ent.d_name);
        nv++;
    }
    sceIoDclose(dfd);

    int removed = 0;
    for (int i = 0; i < nv; i++) {
        char path[512];
        int n = snprintf(path, sizeof(path), "%s/%s", use_dir, victims[i]);
        if (n > 0 && (size_t)n < sizeof(path)) {
            if (sceIoRemove(path) >= 0) {
                removed++;
            }
        }
    }
    fs_unlock_internal();
    return removed;
}

int fs_mkdir(const char *path, int mode)
{
    int rc;
    fs_lock_internal();
    rc = sceIoMkdir(path, mode);
    fs_unlock_internal();
    return rc;
}

int fs_write_atomic(const char *path, const void *data, size_t size)
{
    const uint8_t *ptr = (const uint8_t *)data;
    size_t remaining = size;
    size_t path_len;
    SceUID fd;

    if (!path || !path[0] || !data) {
        return -1;
    }
    path_len = strlen(path);
    {
        char tmp_path[path_len + sizeof(".tmp")];
        char old_path[path_len + sizeof(".old")];

        if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) {
            return -1;
        }
        if (snprintf(old_path, sizeof(old_path), "%s.old", path) >= (int)sizeof(old_path)) {
            return -1;
        }

        fs_lock_internal();
        fd = sceIoOpen(tmp_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
        if (fd < 0) {
            fs_unlock_internal();
            return (int)fd;
        }

        while (remaining > 0) {
            int written = sceIoWrite(fd, ptr, remaining);
            if (written <= 0) {
                sceIoClose(fd);
                sceIoRemove(tmp_path);
                fs_unlock_internal();
                return -1;
            }
            ptr += written;
            remaining -= (size_t)written;
        }
        sceIoClose(fd);
        fs_unlock_internal();

        {
            SceIoStat st;
            int exists;
            int res;

            fs_lock_internal();
            exists = (sceIoGetstat(path, &st) >= 0);
            if (exists) {
                sceIoRename(path, old_path);
            }
            res = sceIoRename(tmp_path, path);
            if (res >= 0) {
                if (exists) {
                    sceIoRemove(old_path);
                }
            } else {
                if (exists) {
                    sceIoRename(old_path, path);
                }
            }
            fs_unlock_internal();
            return res;
        }
    }
}

// --- From new fs.c ---

int fs_get_cwd(char *out, size_t size)
{
    if (!out || size == 0) {
        return -1;
    }
    return getcwd(out, size) ? 0 : -1;
}

int fs_make_abs_path(const char *path, char *out, size_t size)
{
    if (!out || size == 0 || !path) {
        return -1;
    }
    if (is_abs_path(path)) {
        size_t len = strlen(path);
        if (len + 1 > size) {
            return -1;
        }
        memcpy(out, path, len + 1);
        return 0;
    }

    char cwd[256];
    if (fs_get_cwd(cwd, sizeof(cwd)) != 0) {
        return -1;
    }

    size_t cwd_len = strlen(cwd);
    const char *sep = "";
    if (cwd_len > 0 && cwd[cwd_len - 1] != '/' && path[0] != '/') {
        sep = "/";
    }

    int wrote = snprintf(out, size, "%s%s%s", cwd, sep, path);
    if (wrote < 0 || (size_t)wrote >= size) {
        return -1;
    }
    return 0;
}

void fs_log_open(const char *path)
{
    char full[512];
    if (fs_make_abs_path(path, full, sizeof(full)) == 0) {
        LOG_FS("open '%s'\n", full);
        return;
    }
    LOG_FS("open '%s'", path ? path : "(null)");
}

int fs_ensure_dir(const char *path)
{
    if (!path || !path[0]) {
        return -1;
    }

    char abs_path[512];
    char tmp[512];
    size_t len;

    if (fs_make_abs_path(path, abs_path, sizeof(abs_path)) != 0) {
        return -1;
    }

    len = strlen(abs_path);

    if (len >= sizeof(tmp)) {
        return -1;
    }

    memcpy(tmp, abs_path, len + 1);

    for (size_t i = 0; i < len; i++) {
        if (tmp[i] == '/') {
            if (i > 0 && tmp[i - 1] == ':') {
                continue;
            }
            tmp[i] = '\0';
            if (tmp[0] != '\0') {
                fs_mkdir(tmp, 0777);
            }
            tmp[i] = '/';
        }
    }

    return 0;
}
