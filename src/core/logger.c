#include "core/logger.h"
#include "core/fs.h"

#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <pspkernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_PATH_MAX 256
#define LOG_LINE_MAX 512
#define LOG_BUFFER_SIZE (64 * 1024)
#define LOG_AUTO_FLUSH_THRESHOLD (LOG_BUFFER_SIZE / 2)  // Auto-flush when half full
#define LOG_REOPEN_DELAY_US 5000000ULL

// Ring buffer for log messages
static char s_log_buffer[LOG_BUFFER_SIZE];
static size_t s_log_head = 0;
static size_t s_log_tail = 0;
static size_t s_log_used = 0;
static int s_log_dropped = 0;

// File handle and state
static SceUID s_log_fd = -1;
static int s_log_ready = 0;
static int s_logger_initialized = 0;
static int s_reopen_in_progress = 0;
static u64 s_reopen_at_us = 0;
static int s_last_sink_error = 0;
static unsigned int s_sink_failures = 0;
static volatile int s_realtime_mode = 0;
static char s_log_path[LOG_PATH_MAX];

// Thread-safety
static SceLwMutexWorkarea s_log_mutex;
static int s_log_mutex_initialized = 0;

#define LOG_MUTEX_LOCK() \
    do { \
        if (s_log_mutex_initialized) { \
            sceKernelLockLwMutex(&s_log_mutex, 1, NULL); \
        } \
    } while (0)
#define LOG_MUTEX_UNLOCK() \
    do { \
        if (s_log_mutex_initialized) { \
            sceKernelUnlockLwMutex(&s_log_mutex, 1); \
        } \
    } while (0)

static void ensure_log_dir(void)
{
    (void)fs_ensure_dir(s_log_path);
}

// Write data to ring buffer (thread-safe, called with mutex locked)
static void log_buffer_write_locked(const char *data, size_t len)
{
    if (len == 0) {
        return;
    }

    size_t available = LOG_BUFFER_SIZE - s_log_used;
    if (len > available) {
        // Buffer full - drop message
        s_log_dropped++;
        return;
    }

    // Write to ring buffer (handle wrap-around)
    size_t first = LOG_BUFFER_SIZE - s_log_head;
    if (len <= first) {
        // Single contiguous write
        memcpy(s_log_buffer + s_log_head, data, len);
        s_log_head = (s_log_head + len) % LOG_BUFFER_SIZE;
    } else {
        // Two-part write (wrap around)
        memcpy(s_log_buffer + s_log_head, data, first);
        memcpy(s_log_buffer, data + first, len - first);
        s_log_head = len - first;
    }
    s_log_used += len;
}

static void log_sink_failed_locked(int error)
{
    if (s_log_fd >= 0) fs_close(s_log_fd);
    s_log_fd = -1;
    s_log_ready = 0;
    s_last_sink_error = error;
    s_sink_failures++;
    s_reopen_at_us = sceKernelGetSystemTimeWide() + LOG_REOPEN_DELAY_US;
}

/* Called without the logger mutex: fs_open() itself emits an fs log line. */
static void log_try_reopen(void)
{
    SceUID fd;
    u64 now;
    int recovered = 0;
    int last_error = 0;
    unsigned int failures = 0;

    now = sceKernelGetSystemTimeWide();
    LOG_MUTEX_LOCK();
    if (!s_logger_initialized || s_log_ready || s_reopen_in_progress ||
        (s_reopen_at_us != 0 && now < s_reopen_at_us)) {
        LOG_MUTEX_UNLOCK();
        return;
    }
    s_reopen_in_progress = 1;
    LOG_MUTEX_UNLOCK();

    fd = fs_open(s_log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);

    LOG_MUTEX_LOCK();
    if (fd >= 0) {
        s_log_fd = fd;
        s_log_ready = 1;
        s_reopen_at_us = 0;
        recovered = (s_sink_failures != 0);
        last_error = s_last_sink_error;
        failures = s_sink_failures;
        s_last_sink_error = 0;
        s_sink_failures = 0;
    } else {
        s_reopen_at_us = sceKernelGetSystemTimeWide() + LOG_REOPEN_DELAY_US;
    }
    s_reopen_in_progress = 0;
    LOG_MUTEX_UNLOCK();
    if (recovered) {
        logLine("logger: sink recovered failures=%u last_error=%d\n",
                failures, last_error);
    }
}

/* Writes one contiguous span. The caller owns the logger mutex. */
static size_t log_write_span_locked(const char *data, size_t len)
{
    size_t done = 0;
    while (done < len && s_log_ready && s_log_fd >= 0) {
        int rc = fs_write(s_log_fd, data + done, len - done);
        if (rc <= 0) {
            log_sink_failed_locked(rc);
            break;
        }
        done += (size_t)rc;
    }
    return done;
}

// Flush buffered logs synchronously in the caller's thread.
static void log_flush_internal(void)
{
    log_try_reopen();
    if (!s_log_ready || s_log_fd < 0) return;

    // Lock mutex to read from buffer
    LOG_MUTEX_LOCK();

    if (s_log_dropped > 0) {
        char dropped_line[64];
        int len = snprintf(dropped_line, sizeof(dropped_line),
                          "[dropped %d log lines]\n", s_log_dropped);
        if (len > 0) {
            if (log_write_span_locked(dropped_line, (size_t)len) == (size_t)len)
                s_log_dropped = 0;
        }
    }

    while (s_log_used > 0 && s_log_ready) {
        size_t span = LOG_BUFFER_SIZE - s_log_tail;
        size_t written;
        if (span > s_log_used) span = s_log_used;
        written = log_write_span_locked(s_log_buffer + s_log_tail, span);
        s_log_tail = (s_log_tail + written) % LOG_BUFFER_SIZE;
        s_log_used -= written;
        if (written != span) break;
    }

    LOG_MUTEX_UNLOCK();
}

void logger_init(const char *path)
{
    if (!path) {
        return;
    }

    // Initialize path - convert to absolute if fs is initialized
    char abs_path[512];
    if (fs_make_abs_path(path, abs_path, sizeof(abs_path)) == 0) {
        strncpy(s_log_path, abs_path, sizeof(s_log_path) - 1);
    } else {
        strncpy(s_log_path, path, sizeof(s_log_path) - 1);
    }
    s_log_path[sizeof(s_log_path) - 1] = '\0';
    ensure_log_dir();

    // Use a plain lightweight mutex; logger state is not re-entered while locked.
    int ret = sceKernelCreateLwMutex(&s_log_mutex, "log_mutex",
                                     0, 0, NULL);
    if (ret >= 0) {
        s_log_mutex_initialized = 1;
    }

    // Initialize ring buffer
    s_log_head = 0;
    s_log_tail = 0;
    s_log_used = 0;
    s_log_dropped = 0;
    s_logger_initialized = 1;
    s_reopen_in_progress = 0;
    s_reopen_at_us = 0;
    s_last_sink_error = 0;
    s_sink_failures = 0;

    // Ensure directory exists - use relative paths, they will be resolved via CWD
    sceIoMkdir("data", 0777);
    sceIoMkdir("data/logs", 0777);

    // Open log file through fs layer - s_log_path already converted to absolute
    s_log_fd = fs_open(s_log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (s_log_fd >= 0) {
        s_log_ready = 1;
        // Write initial message directly to verify file is writable
        const char *init_msg = "logger: file opened successfully\n";
        if (fs_write(s_log_fd, init_msg, strlen(init_msg)) != (int)strlen(init_msg)) {
            LOG_MUTEX_LOCK();
            log_sink_failed_locked(-1);
            LOG_MUTEX_UNLOCK();
        }
    } else {
        // File open failed - write error to separate file (use relative path)
        sceIoMkdir("data/logs", 0777);
        SceUID err_fd = fs_open("data/logs/logger_error.txt",
                                PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
        if (err_fd >= 0) {
            char err_msg[256];
            int len = snprintf(err_msg, sizeof(err_msg), 
                             "logger: failed to open log file '%s', error=%d\n", s_log_path, s_log_fd);
            if (len > 0) {
                fs_write(err_fd, err_msg, len);
            }
            fs_close(err_fd);
        }
    }

}

void logger_shutdown(void)
{
    // Final flush of remaining data
    log_flush_internal();
    
    // Write shutdown message directly
    if (s_log_ready && s_log_fd >= 0) {
        const char *shutdown_msg = "logger: shutdown\n";
        fs_write(s_log_fd, shutdown_msg, strlen(shutdown_msg));
    }

    // Close file through fs layer
    if (s_log_fd >= 0) {
        fs_close(s_log_fd);
        s_log_fd = -1;
    }
    s_log_ready = 0;
    s_logger_initialized = 0;

    // Destroy mutex
    if (s_log_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_log_mutex);
        s_log_mutex_initialized = 0;
    }
}

void logger_flush(void)
{
    // Force immediate flush of buffered logs
    log_flush_internal();
}

void logger_set_realtime_mode(int enabled)
{
    s_realtime_mode = enabled ? 1 : 0;
}


void logLine(const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    va_list ap;
    u64 usec = sceKernelGetSystemTimeWide();
    unsigned int ms = (unsigned int)(usec / 1000ULL);

    // Format timestamp prefix
    int prefix = snprintf(line, sizeof(line), "[%u ms] ", ms);
    if (prefix < 0 || prefix >= (int)sizeof(line)) {
        return;
    }

    // Format message body
    va_start(ap, fmt);
    int body = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, fmt, ap);
    va_end(ap);

    if (body < 0) {
        return;
    }

    // Calculate total length
    size_t total_len = (size_t)(prefix + body);
    if (total_len >= sizeof(line)) {
        total_len = sizeof(line) - 1;
        line[total_len] = '\0';
    }

    for (;;) {
        int need_flush = 0;
        int buffer_full = 0;

        LOG_MUTEX_LOCK();
        if (total_len <= (LOG_BUFFER_SIZE - s_log_used)) {
            log_buffer_write_locked(line, total_len);
            if (!s_realtime_mode && s_log_used >= LOG_AUTO_FLUSH_THRESHOLD &&
                s_log_ready && s_log_fd >= 0) {
                need_flush = 1;
            }
            LOG_MUTEX_UNLOCK();

            if (need_flush) {
                log_flush_internal();
            }
            return;
        }

        buffer_full = (!s_realtime_mode && s_log_ready && s_log_fd >= 0 && s_log_used > 0);
        if (!buffer_full) {
            s_log_dropped++;
            LOG_MUTEX_UNLOCK();
            return;
        }
        LOG_MUTEX_UNLOCK();

        // Buffer is full: flush synchronously in the caller's thread, then retry the write.
        log_flush_internal();
    }
}
