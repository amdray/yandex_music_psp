/* Хранилище метаданных треков: ОЗУ-первым, карта — отложенно и пачками.
 *
 * Раньше каждый put делал lseek+write 1,2 КБ прямо на Memory Stick (под
 * общим fs-мьютексом), а get — lseek+read. На медленной карте одна запись
 * стоит ~2 с, так что список из десятков/тысяч треков вешал загрузку на
 * минуты и блокировал любой другой файловый I/O приложения.
 *
 * Теперь:
 *  - все записи живут в ОЗУ (META_RAM_SLOTS слотов, LRU);
 *  - файл читается ОДИН раз (последовательно, крупными кусками) при первом
 *    обращении и больше в горячем пути не трогается;
 *  - изменённые слоты сбрасываются на карту фоновым потоком с низким
 *    приоритетом одной крупной записью, не чаще раза в META_FLUSH_DELAY_US,
 *    и целиком при выходе.
 * Формат файла прежний (версия 4), старые файлы читаются. */
#include "services/track_meta_store.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"

#define META_STORE_PATH "data/cache/meta/store.bin"
#define META_STORE_VERSION 4

/* 320 слотов x ~1,2 КБ ≈ 380 КБ ОЗУ: PSP-1000 имеет 32 МБ на всё, свободной
   системной памяти после старта остаётся ~1,5 МБ — не раздуваем. */
#define META_RAM_SLOTS 320

/* Максимум слотов в одной записи на карту (≈115 КБ временного буфера). */
#define META_FLUSH_MAX_SLOTS 96
#define META_FLUSH_DELAY_US 15000000u
#define META_FLUSH_RETRY_US 4000000u
#define META_FLUSH_PRIO 0x32
#define META_FLUSH_STACK 0x4000
#define META_LOAD_CHUNK (64 * 1024)

#define META_RECORD_MAGIC 0x52544D59u /* "YMTR" */

enum { SLOT_FREE = 0, SLOT_CLEAN = 1, SLOT_DIRTY = 2 };

typedef struct {
    char magic[4];        /* "YMMS" */
    uint16_t version;
    uint16_t record_size;
    uint32_t reserved[6];
} MetaStoreHeader;        /* 32 bytes */

typedef struct {
    uint32_t magic;       /* META_RECORD_MAGIC when the slot is valid */
    uint32_t stamp;       /* LRU clock; larger = fresher */
    TrackEntry entry;     /* entry.id is the key */
} MetaRecord;

static MetaRecord *s_ram = NULL;
static uint8_t s_state[META_RAM_SLOTS];
static uint32_t s_hash[META_RAM_SLOTS];
static int s_used_slots = 0;     /* верхняя граница занятых слотов */
static int s_dirty_count = 0;
static uint32_t s_clock = 0;
static int s_initialized = 0;
static int s_file_valid = 0;     /* заголовок на карте корректен */
static int s_io_failures = 0;
static uint64_t s_first_dirty_us = 0;

/* Rights availability can change between application runs. Keep terminal
   unavailable responses in RAM so they resolve every window/queue lookup in
   this run, but never make that state durable on the Memory Stick. */
static TrackEntry *s_unavailable = NULL;
static int s_unavailable_count = 0;
static int s_unavailable_capacity = 0;

static SceLwMutexWorkarea s_mutex;
static int s_mutex_initialized = 0;

static SceUID s_flush_tid = -1;
static SceUID s_flush_sema = -1;
static volatile int s_flush_stop = 0;

static void meta_lock(void)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_mutex, 1, NULL);
    }
}

static void meta_unlock(void)
{
    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_mutex, 1);
    }
}

static uint32_t meta_hash(const char *s)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t meta_hash_key(const char *track_id, int album_id)
{
    uint32_t h = meta_hash(track_id);
    unsigned int value = (unsigned int)album_id;
    int i;

    for (i = 0; i < 4; ++i) {
        h ^= (uint8_t)(value & 0xFFu);
        h *= 16777619u;
        value >>= 8;
    }
    return h;
}

static int meta_unavailable_find(const char *track_id, int album_id)
{
    int i;
    for (i = 0; i < s_unavailable_count; ++i) {
        if (strcmp(s_unavailable[i].id, track_id) == 0 &&
            (album_id == 0 || s_unavailable[i].album_id == album_id)) {
            return i;
        }
    }
    return -1;
}

static int meta_unavailable_put(const TrackEntry *entry)
{
    int pos = meta_unavailable_find(entry->id, entry->album_id);
    if (pos >= 0) {
        memcpy(&s_unavailable[pos], entry, sizeof(*entry));
        return 0;
    }
    if (s_unavailable_count == s_unavailable_capacity) {
        int new_capacity = s_unavailable_capacity > 0
            ? s_unavailable_capacity * 2
            : 4;
        TrackEntry *grown = (TrackEntry *)realloc(
            s_unavailable, (size_t)new_capacity * sizeof(*grown));
        if (!grown) {
            logLine("meta_store: unavailable session cache allocation failed\n");
            return -1;
        }
        s_unavailable = grown;
        s_unavailable_capacity = new_capacity;
    }
    memcpy(&s_unavailable[s_unavailable_count++], entry, sizeof(*entry));
    return 0;
}

static void meta_unavailable_remove(const char *track_id, int album_id)
{
    int pos = meta_unavailable_find(track_id, album_id);
    if (pos >= 0) {
        s_unavailable[pos] = s_unavailable[s_unavailable_count - 1];
        s_unavailable_count--;
    }
}

static SceOff meta_slot_offset(int slot)
{
    return (SceOff)sizeof(MetaStoreHeader) + (SceOff)slot * sizeof(MetaRecord);
}

static void meta_mark_dirty(int slot)
{
    if (s_state[slot] != SLOT_DIRTY) {
        s_state[slot] = SLOT_DIRTY;
        if (s_dirty_count == 0) {
            s_first_dirty_us = sceKernelGetSystemTimeWide();
        }
        s_dirty_count++;
    }
}

/* Точный поиск слота по (id, album). -1 = нет. */
static int meta_find(uint32_t hash, const char *track_id, int album_id)
{
    int i;
    for (i = 0; i < s_used_slots; i++) {
        if (s_state[i] == SLOT_FREE || s_hash[i] != hash) {
            continue;
        }
        if (strcmp(s_ram[i].entry.id, track_id) == 0 &&
            s_ram[i].entry.album_id == album_id) {
            return i;
        }
    }
    return -1;
}

/* Самая свежая запись для id в любом альбоме. -1 = нет. */
static int meta_find_any(const char *track_id)
{
    uint32_t newest = 0;
    int best = -1;
    int i;

    for (i = 0; i < s_used_slots; i++) {
        if (s_state[i] == SLOT_FREE ||
            strcmp(s_ram[i].entry.id, track_id) != 0) {
            continue;
        }
        if (best < 0 || s_ram[i].stamp > newest) {
            newest = s_ram[i].stamp;
            best = i;
        }
    }
    return best;
}

/* Один раз читает файл в ОЗУ крупными кусками. Любая неполадка = пустое
   хранилище (это кэш: потеря стоит повторной гидрации, не корректности). */
static void meta_load_file(void)
{
    MetaStoreHeader header;
    SceIoStat st;
    SceUID fd;
    int file_slots, n, slot = 0;
    size_t want;
    uint8_t *dst;

    s_file_valid = 0;
    if (fs_getstat(META_STORE_PATH, &st) != 0) {
        return;
    }
    fd = fs_open(META_STORE_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        return;
    }
    if (fs_read(fd, &header, sizeof(header)) != (int)sizeof(header) ||
        memcmp(header.magic, "YMMS", 4) != 0 ||
        header.version != META_STORE_VERSION ||
        header.record_size != sizeof(MetaRecord)) {
        logLine("meta_store: invalid store, will recreate\n");
        fs_close(fd);
        return;
    }
    s_file_valid = 1;
    file_slots = (int)(((SceOff)st.st_size - (SceOff)sizeof(header)) /
                       (SceOff)sizeof(MetaRecord));
    n = file_slots < META_RAM_SLOTS ? file_slots : META_RAM_SLOTS;
    if (n <= 0) {
        fs_close(fd);
        return;
    }

    want = (size_t)n * sizeof(MetaRecord);
    dst = (uint8_t *)s_ram;
    while (want > 0) {
        size_t chunk = want < META_LOAD_CHUNK ? want : META_LOAD_CHUNK;
        int rc = fs_read(fd, dst, chunk);
        if (rc <= 0) {
            /* Неполное чтение: берём, что целиком прочиталось. */
            n = (int)((size_t)(dst - (uint8_t *)s_ram) / sizeof(MetaRecord));
            break;
        }
        dst += rc;
        want -= (size_t)rc;
    }
    fs_close(fd);

    for (slot = 0; slot < n; slot++) {
        MetaRecord *rec = &s_ram[slot];
        if (rec->magic != META_RECORD_MAGIC || !rec->entry.id[0]) {
            memset(rec, 0, sizeof(*rec));
            continue;
        }
        s_state[slot] = SLOT_CLEAN;
        s_hash[slot] = meta_hash_key(rec->entry.id, rec->entry.album_id);
        if (rec->stamp > s_clock) {
            s_clock = rec->stamp;
        }
        s_used_slots = slot + 1;
    }
    logLine("meta_store: loaded %d slots into RAM\n", s_used_slots);
}

/* Ленивая загрузка на рабочем потоке. Вызывать под meta_lock. */
static int meta_ensure_loaded(void)
{
    if (s_initialized) {
        return s_ram ? 0 : -1;
    }
    s_initialized = 1;
    s_ram = (MetaRecord *)calloc(META_RAM_SLOTS, sizeof(MetaRecord));
    if (!s_ram) {
        logLine("meta_store: RAM cache allocation failed\n");
        return -1;
    }
    memset(s_state, 0, sizeof(s_state));
    s_used_slots = 0;
    s_dirty_count = 0;
    s_clock = 0;
    meta_load_file();
    return 0;
}

/* Записывает один непрерывный диапазон грязных слотов. Возвращает число
   записанных слотов (0 = нечего писать / ошибка). Берёт и отпускает
   meta_lock сам; запись на карту идёт БЕЗ него. */
static int meta_flush_once(void)
{
    uint8_t *stage = NULL;
    int first = -1, last = -1, count, i;
    size_t bytes;
    SceUID fd = -1;
    int ok = 0;

    meta_lock();
    if (!s_ram || s_dirty_count <= 0) {
        meta_unlock();
        return 0;
    }
    for (i = 0; i < s_used_slots; i++) {
        if (s_state[i] == SLOT_DIRTY) {
            first = i;
            break;
        }
    }
    if (first < 0) {
        s_dirty_count = 0;
        meta_unlock();
        return 0;
    }
    last = first;
    for (i = first + 1; i < s_used_slots && (i - first) < META_FLUSH_MAX_SLOTS;
         i++) {
        if (s_state[i] == SLOT_DIRTY) {
            last = i;
        }
    }
    count = last - first + 1;
    bytes = (size_t)count * sizeof(MetaRecord);
    stage = (uint8_t *)malloc(bytes);
    if (!stage) {
        meta_unlock();
        return 0;
    }
    memcpy(stage, &s_ram[first], bytes);
    for (i = first; i <= last; i++) {
        if (s_state[i] == SLOT_DIRTY) {
            s_state[i] = SLOT_CLEAN;
            s_dirty_count--;
        }
    }
    meta_unlock();

    fs_ensure_dir(META_STORE_PATH);
    if (!s_file_valid) {
        MetaStoreHeader header;
        fd = fs_open(META_STORE_PATH,
                     PSP_O_RDWR | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            memset(&header, 0, sizeof(header));
            memcpy(header.magic, "YMMS", 4);
            header.version = META_STORE_VERSION;
            header.record_size = sizeof(MetaRecord);
            if (fs_write(fd, &header, sizeof(header)) == (int)sizeof(header)) {
                s_file_valid = 1;
            }
        }
    } else {
        fd = fs_open(META_STORE_PATH, PSP_O_RDWR, 0777);
    }
    if (fd >= 0) {
        const uint8_t *p = stage;
        size_t left = bytes;
        SceOff off = meta_slot_offset(first);

        if (s_file_valid && fs_lseek(fd, off, PSP_SEEK_SET) == off) {
            ok = 1;
            while (left > 0) {
                int rc = fs_write(fd, p, left);
                if (rc <= 0) {
                    ok = 0;
                    break;
                }
                p += rc;
                left -= (size_t)rc;
            }
        }
        fs_close(fd);
    }
    free(stage);

    if (!ok) {
        /* Вернуть слоты в «грязные», но не зацикливаться на сбойной карте. */
        meta_lock();
        if (s_ram) {
            for (i = first; i <= last; i++) {
                if (s_state[i] == SLOT_CLEAN) {
                    meta_mark_dirty(i);
                }
            }
        }
        meta_unlock();
        s_io_failures++;
        return 0;
    }
    s_io_failures = 0;
    return count;
}

static int meta_flush_worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!s_flush_stop) {
        SceUInt timeout = 2000000;
        int dirty, due;
        uint64_t now;

        (void)sceKernelWaitSema(s_flush_sema, 1, &timeout);
        if (s_flush_stop) {
            break;
        }
        if (s_io_failures >= 3) {
            continue; /* карта не пишется — только RAM */
        }
        meta_lock();
        dirty = s_dirty_count;
        now = sceKernelGetSystemTimeWide();
        due = dirty > 0 && (now - s_first_dirty_us) >= META_FLUSH_DELAY_US;
        meta_unlock();
        if (!due) {
            continue;
        }
        if (meta_flush_once() > 0) {
            logLine("meta_store: flushed to card\n");
            sceKernelDelayThread(META_FLUSH_RETRY_US);
        }
    }
    return sceKernelExitDeleteThread(0);
}

int track_meta_store_init(void)
{
    if (s_mutex_initialized) {
        return 0;
    }
    if (sceKernelCreateLwMutex(&s_mutex, "meta_store", 0, 0, NULL) < 0) {
        return -1;
    }
    s_mutex_initialized = 1;

    s_flush_stop = 0;
    s_flush_sema = sceKernelCreateSema("meta_flush", 0, 0, 1, NULL);
    if (s_flush_sema >= 0) {
        s_flush_tid = sceKernelCreateThread("meta_flush", meta_flush_worker,
                                            META_FLUSH_PRIO, META_FLUSH_STACK,
                                            0, NULL);
        if (s_flush_tid < 0 || sceKernelStartThread(s_flush_tid, 0, NULL) < 0) {
            if (s_flush_tid >= 0) {
                sceKernelDeleteThread(s_flush_tid);
            }
            s_flush_tid = -1;
            sceKernelDeleteSema(s_flush_sema);
            s_flush_sema = -1;
        }
    }
    return 0;
}

void track_meta_store_shutdown(void)
{
    if (s_flush_tid >= 0) {
        SceUInt timeout = 5000000;
        s_flush_stop = 1;
        sceKernelSignalSema(s_flush_sema, 1);
        if (sceKernelWaitThreadEnd(s_flush_tid, &timeout) < 0) {
            sceKernelTerminateDeleteThread(s_flush_tid);
        }
        s_flush_tid = -1;
    }
    if (s_flush_sema >= 0) {
        sceKernelDeleteSema(s_flush_sema);
        s_flush_sema = -1;
    }

    /* Финальный сброс: крупными записями, пока есть что писать. */
    if (s_io_failures < 3) {
        int guard = 8;
        while (guard-- > 0 && meta_flush_once() > 0) {
        }
    }

    meta_lock();
    free(s_ram);
    s_ram = NULL;
    s_initialized = 0;
    s_used_slots = 0;
    s_dirty_count = 0;
    free(s_unavailable);
    s_unavailable = NULL;
    s_unavailable_count = 0;
    s_unavailable_capacity = 0;
    meta_unlock();

    if (s_mutex_initialized) {
        sceKernelDeleteLwMutex(&s_mutex);
        s_mutex_initialized = 0;
    }
}

int track_meta_store_get_for(const char *track_id, int album_id,
                             TrackEntry *out)
{
    int pos;
    int rc = -1;

    if (!track_id || !track_id[0] || !out) {
        return -1;
    }

    meta_lock();
    pos = meta_unavailable_find(track_id, album_id);
    if (pos >= 0) {
        memcpy(out, &s_unavailable[pos], sizeof(*out));
        rc = 0;
    } else if (meta_ensure_loaded() == 0) {
        pos = album_id != 0
            ? meta_find(meta_hash_key(track_id, album_id), track_id, album_id)
            : meta_find_any(track_id);
        if (pos >= 0) {
            memcpy(out, &s_ram[pos].entry, sizeof(*out));
            /* RAM-only: освежаем LRU, на карту ничего не пишем. */
            s_ram[pos].stamp = ++s_clock;
            rc = 0;
        }
    }
    meta_unlock();
    return rc;
}

int track_meta_store_get(const char *track_id, TrackEntry *out)
{
    return track_meta_store_get_for(track_id, 0, out);
}

int track_meta_store_put(const TrackEntry *entry)
{
    uint32_t hash;
    int slot;
    int rc = -1;

    if (!entry || !entry->id[0]) {
        return -1;
    }

    meta_lock();
    if (!entry->available) {
        rc = meta_unavailable_put(entry);
        meta_unlock();
        return rc;
    }
    meta_unavailable_remove(entry->id, entry->album_id);
    if (meta_ensure_loaded() != 0) {
        meta_unlock();
        return -1;
    }

    hash = meta_hash_key(entry->id, entry->album_id);
    slot = meta_find(hash, entry->id, entry->album_id);
    if (slot < 0) {
        int i;
        /* Свободный слот или вытеснение самого старого. */
        for (i = 0; i < s_used_slots; i++) {
            if (s_state[i] == SLOT_FREE) {
                slot = i;
                break;
            }
        }
        if (slot < 0 && s_used_slots < META_RAM_SLOTS) {
            slot = s_used_slots++;
        }
        if (slot < 0) {
            slot = 0;
            for (i = 1; i < s_used_slots; i++) {
                if (s_ram[i].stamp < s_ram[slot].stamp) {
                    slot = i;
                }
            }
        }
    }

    s_ram[slot].magic = META_RECORD_MAGIC;
    s_ram[slot].stamp = ++s_clock;
    memcpy(&s_ram[slot].entry, entry, sizeof(s_ram[slot].entry));
    s_hash[slot] = hash;
    if (s_state[slot] == SLOT_FREE) {
        s_state[slot] = SLOT_CLEAN;
    }
    meta_mark_dirty(slot);
    rc = 0;
    meta_unlock();
    return rc;
}
