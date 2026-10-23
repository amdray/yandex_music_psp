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
/* Ленивый режим (по умолчанию): лог копится в RAM и пишется на карту редко —
 * при накоплении LOG_AUTO_FLUSH_THRESHOLD, раз в LOG_WRITER_PERIOD_US или по
 * logger_flush_now(). На реальной PSP каждая запись + sceIoSync держит
 * Memory Stick секундами, и все чтения (шрифты, токен, конфиги) стоят в
 * очереди за ними; плюс индикатор карты мигает постоянно. logger_flush()
 * в ленивом режиме ничего не пишет (его зовут сотни мест, главный цикл —
 * каждые 250 мс). YM_LOG_EAGER=1 — старое поведение для отладки падений. */
#ifdef YM_LOG_EAGER
#define LOG_AUTO_FLUSH_THRESHOLD (LOG_BUFFER_SIZE / 2)
#else
#define LOG_AUTO_FLUSH_THRESHOLD (LOG_BUFFER_SIZE / 4)
#endif
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
#define LOG_CFG_PATH "config/logs.cfg"
static volatile int s_log_enabled = 1;

// Фоновый писатель (async). Запись на Memory Stick на реальной PSP с
// почти заполненной/фрагментированной FAT32-картой занимает ~2 с на операцию;
// синхронный flush держал мьютекс логгера и fs, и UI замирал на каждом
// logLine/logger_flush (меню «листало через раз», старт 100+ с).
// Теперь на карту пишет только поток log_writer (низкий приоритет),
// logLine/logger_flush никогда не ждут Memory Stick.
#define LOG_WRITER_PRIO   0x30
#ifdef YM_LOG_EAGER
#define LOG_WRITER_PERIOD_US 2000000
#define LOG_SYNC_MIN_US   4000000ULL
#else
#define LOG_WRITER_PERIOD_US 60000000
#define LOG_SYNC_MIN_US   300000000ULL
#endif
#define LOG_WRITE_CHUNK   (16 * 1024)
static SceUID s_writer_tid = -1;
static SceUID s_writer_sema = -1;
static volatile int s_writer_stop = 0;
static char s_writer_buf[LOG_WRITE_CHUNK];

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
        int rc = sceIoWrite(s_log_fd, data + done, len - done);
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

/* Фоновый сброс: копия куска под мьютексом, запись на MS — БЕЗ мьютекса
 * логгера (logLine других потоков не ждут). Хвост двигает только писатель. */
/* Синхронизация FAT после записи: без неё размер файла в каталоге
 * обновляется только при закрытии, и при внезапном выключении/падении
 * консоли всё записанное после открытия теряется (лог «обрывался» за
 * минуты до реального места падения). Устройство берём из пути лога. */
static u64 s_last_sync_us = 0;
static volatile int s_force_flush = 0;
static void log_sync_device(void)
{
    char dev[16];
    const char *colon = strchr(s_log_path, ':');
    size_t n;
    if (!colon) return;
    n = (size_t)(colon - s_log_path) + 1;
    if (n >= sizeof(dev)) return;
    memcpy(dev, s_log_path, n);
    dev[n] = '\0';
    (void)sceIoSync(dev, 0);
}

static void log_flush_background(void)
{
    int wrote = 0;

    log_try_reopen();
    if (!s_log_ready || s_log_fd < 0) return;

    for (;;) {
        size_t span;
        int dropped = 0;
        size_t done = 0;

        LOG_MUTEX_LOCK();
        if (s_log_dropped > 0) {
            dropped = s_log_dropped;
            s_log_dropped = 0;
        }
        span = LOG_BUFFER_SIZE - s_log_tail;
        if (span > s_log_used) span = s_log_used;
        if (span > LOG_WRITE_CHUNK) span = LOG_WRITE_CHUNK;
        if (span > 0) memcpy(s_writer_buf, s_log_buffer + s_log_tail, span);
        LOG_MUTEX_UNLOCK();

        if (dropped > 0) {
            char dl[64];
            int len = snprintf(dl, sizeof(dl), "[dropped %d log lines]\n", dropped);
            if (len > 0 && s_log_fd >= 0) (void)sceIoWrite(s_log_fd, dl, (size_t)len);
        }
        if (span == 0) break;

        while (done < span && s_log_fd >= 0) {
            int rc = sceIoWrite(s_log_fd, s_writer_buf + done, span - done);
            if (rc <= 0) {
                LOG_MUTEX_LOCK();
                log_sink_failed_locked(rc);
                LOG_MUTEX_UNLOCK();
                break;
            }
            done += (size_t)rc;
        }

        if (done > 0) wrote = 1;
        LOG_MUTEX_LOCK();
        /* Даже при ошибке записи кусок снимаем — иначе вечный повтор. */
        s_log_tail = (s_log_tail + span) % LOG_BUFFER_SIZE;
        s_log_used -= span;
        LOG_MUTEX_UNLOCK();
        if (done != span) break;
    }
    if (wrote) {
        /* sceIoSync на медленной карте дорогой: не чаще раза в LOG_SYNC_MIN_US. */
        u64 now = sceKernelGetSystemTimeWide();
        if (s_force_flush || now - s_last_sync_us >= LOG_SYNC_MIN_US) {
            s_force_flush = 0;
            log_sync_device();
            s_last_sync_us = sceKernelGetSystemTimeWide();
        }
    }
}

static int log_writer_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!s_writer_stop) {
        SceUInt timeout = LOG_WRITER_PERIOD_US;
        int busy;
        (void)sceKernelWaitSema(s_writer_sema, 1, &timeout);
        if (s_writer_stop) break;
        /* В реальном времени (играет звук) пишем только когда накопилось
         * 8 КБ (раньше 3/4 буфера — при падении терялись десятки КБ хвоста) — меньше конкуренции за MS с аудио. */
        LOG_MUTEX_LOCK();
        busy = s_realtime_mode && !s_force_flush &&
               s_log_used < (LOG_BUFFER_SIZE / 8);
        LOG_MUTEX_UNLOCK();
        if (!busy) log_flush_background();
    }
    return sceKernelExitDeleteThread(0);
}

static void log_writer_start(void)
{
    if (s_writer_tid >= 0) return;
    s_writer_sema = sceKernelCreateSema("log_wr", 0, 0, 1, NULL);
    if (s_writer_sema < 0) return;
    s_writer_stop = 0;
    s_writer_tid = sceKernelCreateThread("log_writer", log_writer_main,
                                         LOG_WRITER_PRIO, 0x2000,
                                         PSP_THREAD_ATTR_USER, NULL);
    if (s_writer_tid < 0 || sceKernelStartThread(s_writer_tid, 0, NULL) < 0) {
        if (s_writer_tid >= 0) sceKernelDeleteThread(s_writer_tid);
        s_writer_tid = -1;
        sceKernelDeleteSema(s_writer_sema);
        s_writer_sema = -1;
    }
}

static void log_writer_signal(void)
{
    if (s_writer_sema >= 0) sceKernelSignalSema(s_writer_sema, 1);
}

static void log_writer_stop(void)
{
    SceUInt timeout = 3000000;
    if (s_writer_tid < 0) return;
    s_writer_stop = 1;
    log_writer_signal();
    if (sceKernelWaitThreadEnd(s_writer_tid, &timeout) < 0) {
        sceKernelTerminateDeleteThread(s_writer_tid);
    }
    s_writer_tid = -1;
    if (s_writer_sema >= 0) {
        sceKernelDeleteSema(s_writer_sema);
        s_writer_sema = -1;
    }
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

    log_writer_start();

    /* Настройка «Логи»: читаем после открытия файла (fs_open сам пишет в лог). */
    {
        char cfg[16];
        SceUID cfd = fs_open(LOG_CFG_PATH, PSP_O_RDONLY, 0777);
        if (cfd >= 0) {
            int n = fs_read(cfd, cfg, sizeof(cfg) - 1);
            fs_close(cfd);
            if (n > 0) {
                cfg[n] = '\0';
                if (strncmp(cfg, "logs=0", 6) == 0) {
                    logLine("logger: disabled by settings (logs=0)\n");
                    s_log_enabled = 0;
                }
            }
        }
    }
}

int logger_enabled(void)
{
    return s_log_enabled;
}

void logger_set_enabled(int enabled)
{
    const char *line;
    enabled = enabled ? 1 : 0;
    if (s_log_enabled == enabled) return;
    if (enabled) {
        s_log_enabled = 1;
        logLine("logger: enabled by settings\n");
        line = "logs=1\n";
    } else {
        logLine("logger: disabled by settings\n");
        logger_flush_now();   /* дописать хвост: после этого записи прекращаются */
        s_log_enabled = 0;
        line = "logs=0\n";
    }
    (void)fs_write_deferred(LOG_CFG_PATH, line, strlen(line));
}

void logger_shutdown(void)
{
    // Остановить фонового писателя, затем финальный синхронный сброс.
    log_writer_stop();
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
#ifdef YM_LOG_EAGER
    // С фоновым писателем — только разбудить его (не ждём Memory Stick).
    if (s_writer_tid >= 0) {
        log_writer_signal();
        return;
    }
    log_flush_internal();
#endif
    // Ленивый режим: запись идёт по порогу/таймеру, см. комментарий выше.
}

void logger_flush_now(void)
{
    // Сон/питание: записать накопленное и синхронизировать FAT.
    s_force_flush = 1;
    if (s_writer_tid >= 0) {
        log_writer_signal();
        return;
    }
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
    u64 usec;

    if (!s_log_enabled) return;
    usec = sceKernelGetSystemTimeWide();
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
            /* Не даём буферу переполниться и в realtime-режиме (аудио). */
            if (s_realtime_mode && s_log_used >= (LOG_BUFFER_SIZE * 5 / 8) &&
                s_log_ready && s_log_fd >= 0) {
                need_flush = 1;
            }
            LOG_MUTEX_UNLOCK();

            if (need_flush) {
                if (s_writer_tid >= 0) log_writer_signal();
                else log_flush_internal();
            }
            return;
        }

        if (s_writer_tid >= 0) {
            /* Буфер полон, писатель отстаёт (медленная карта): не ждём —
             * строку теряем, считаем в dropped, будим писателя. */
            s_log_dropped++;
            LOG_MUTEX_UNLOCK();
            log_writer_signal();
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

void logger_checkpoint(const char *fmt, ...)
{
    char msg[LOG_LINE_MAX];
    va_list ap;

    if (!s_log_enabled) return;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    logLine("checkpoint: %s\n", msg);
    logger_flush_now();
}
