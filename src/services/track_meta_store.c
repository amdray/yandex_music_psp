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

/* 4096 slots x ~780 B ≈ 3.1 MB on the Memory Stick when full — comfortably
   covers the whole liked collection (2511 tracks) plus open playlists. */
#define META_STORE_SLOTS 4096

#define META_RECORD_MAGIC 0x52544D59u /* "YMTR" */

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

typedef struct {
    uint32_t hash;
    uint16_t slot;
} MetaDirEntry;

static SceUID s_fd = -1;
static int s_slot_count = 0;             /* slots physically present in file */
static uint32_t s_stamps[META_STORE_SLOTS];
static MetaDirEntry s_dir[META_STORE_SLOTS];
static int s_dir_count = 0;
static uint32_t s_clock = 0;
static int s_initialized = 0;

/* Rights availability can change between application runs. Keep terminal
   unavailable responses in RAM so they resolve every window/queue lookup in
   this run, but never make that state durable on the Memory Stick. */
static TrackEntry *s_unavailable = NULL;
static int s_unavailable_count = 0;
static int s_unavailable_capacity = 0;

static SceLwMutexWorkarea s_mutex;
static int s_mutex_initialized = 0;

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

static int meta_read_record(int slot, MetaRecord *rec)
{
    SceOff offset = meta_slot_offset(slot);
    if (fs_lseek(s_fd, offset, PSP_SEEK_SET) != offset) {
        return -1;
    }
    if (fs_read(s_fd, rec, sizeof(*rec)) != (int)sizeof(*rec)) {
        return -1;
    }
    return 0;
}

static int meta_write_record(int slot, const MetaRecord *rec)
{
    SceOff offset = meta_slot_offset(slot);
    const char *p = (const char *)rec;
    int remaining = (int)sizeof(*rec);

    if (fs_lseek(s_fd, offset, PSP_SEEK_SET) != offset) {
        return -1;
    }
    while (remaining > 0) {
        int rc = fs_write(s_fd, p, (size_t)remaining);
        if (rc <= 0) {
            return -1;
        }
        p += rc;
        remaining -= rc;
    }
    return 0;
}

static int meta_dir_find(uint32_t hash, const char *track_id, int album_id,
                         MetaRecord *rec)
{
    int i;
    for (i = 0; i < s_dir_count; i++) {
        if (s_dir[i].hash != hash) {
            continue;
        }
        if (meta_read_record(s_dir[i].slot, rec) != 0) {
            continue;
        }
        if (rec->magic == META_RECORD_MAGIC &&
            strcmp(rec->entry.id, track_id) == 0 &&
            rec->entry.album_id == album_id) {
            return i;
        }
    }
    return -1;
}

static int meta_dir_find_any(const char *track_id, MetaRecord *rec)
{
    MetaRecord candidate;
    uint32_t newest = 0;
    int newest_pos = -1;
    int i;

    for (i = 0; i < s_dir_count; ++i) {
        if (meta_read_record(s_dir[i].slot, &candidate) != 0 ||
            candidate.magic != META_RECORD_MAGIC ||
            strcmp(candidate.entry.id, track_id) != 0) {
            continue;
        }
        if (newest_pos < 0 || s_stamps[s_dir[i].slot] > newest) {
            newest = s_stamps[s_dir[i].slot];
            newest_pos = i;
            memcpy(rec, &candidate, sizeof(*rec));
        }
    }
    return newest_pos;
}

static void meta_dir_remove_slot(int slot)
{
    int i;
    for (i = 0; i < s_dir_count; i++) {
        if (s_dir[i].slot == slot) {
            s_dir[i] = s_dir[s_dir_count - 1];
            s_dir_count--;
            return;
        }
    }
}

/* Fresh empty store: truncate + header. */
static int meta_store_create(void)
{
    MetaStoreHeader header;

    if (s_fd >= 0) {
        fs_close(s_fd);
    }
    s_fd = fs_open(META_STORE_PATH,
                   PSP_O_RDWR | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (s_fd < 0) {
        return -1;
    }

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "YMMS", 4);
    header.version = META_STORE_VERSION;
    header.record_size = sizeof(MetaRecord);
    if (fs_write(s_fd, &header, sizeof(header)) != (int)sizeof(header)) {
        return -1;
    }

    s_slot_count = 0;
    s_dir_count = 0;
    s_clock = 0;
    memset(s_stamps, 0, sizeof(s_stamps));
    return 0;
}

/* Build the RAM directory from the file. Any inconsistency resets the store —
   it is a cache; losing it costs re-hydration, never correctness. */
static int meta_store_load(void)
{
    MetaStoreHeader header;
    SceIoStat st;
    int slot;

    fs_ensure_dir(META_STORE_PATH);
    if (fs_getstat(META_STORE_PATH, &st) != 0) {
        return meta_store_create();
    }

    s_fd = fs_open(META_STORE_PATH, PSP_O_RDWR, 0777);
    if (s_fd < 0) {
        return meta_store_create();
    }

    if (fs_read(s_fd, &header, sizeof(header)) != (int)sizeof(header) ||
        memcmp(header.magic, "YMMS", 4) != 0 ||
        header.version != META_STORE_VERSION ||
        header.record_size != sizeof(MetaRecord) ||
        ((SceOff)st.st_size - (SceOff)sizeof(header)) % sizeof(MetaRecord) != 0) {
        logLine("meta_store: invalid store, resetting\n");
        return meta_store_create();
    }

    s_slot_count = (int)(((SceOff)st.st_size - (SceOff)sizeof(header)) /
                         sizeof(MetaRecord));
    if (s_slot_count > META_STORE_SLOTS) {
        logLine("meta_store: slot count %d exceeds cap, resetting\n", s_slot_count);
        return meta_store_create();
    }

    s_dir_count = 0;
    s_clock = 0;
    memset(s_stamps, 0, sizeof(s_stamps));

    for (slot = 0; slot < s_slot_count; slot++) {
        MetaRecord rec;
        if (meta_read_record(slot, &rec) != 0) {
            logLine("meta_store: scan failed at slot %d, resetting\n", slot);
            return meta_store_create();
        }
        if (rec.magic != META_RECORD_MAGIC || !rec.entry.id[0]) {
            continue; /* free slot */
        }
        s_stamps[slot] = rec.stamp;
        if (rec.stamp > s_clock) {
            s_clock = rec.stamp;
        }
        s_dir[s_dir_count].hash = meta_hash_key(rec.entry.id,
                                                rec.entry.album_id);
        s_dir[s_dir_count].slot = (uint16_t)slot;
        s_dir_count++;
    }

    logLine("meta_store: loaded %d records in %d slots\n", s_dir_count, s_slot_count);
    return 0;
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
    return 0;
}

void track_meta_store_shutdown(void)
{
    meta_lock();
    if (s_fd >= 0) {
        fs_close(s_fd);
        s_fd = -1;
    }
    s_initialized = 0;
    s_dir_count = 0;
    s_slot_count = 0;
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

/* Lazily open/scan on first use, on the calling worker thread. */
static int meta_ensure_loaded(void)
{
    if (s_initialized) {
        return s_fd >= 0 ? 0 : -1;
    }
    s_initialized = 1;
    if (meta_store_load() != 0) {
        logLine("meta_store: unavailable (open/create failed)\n");
        if (s_fd >= 0) {
            fs_close(s_fd);
            s_fd = -1;
        }
        return -1;
    }
    return 0;
}

int track_meta_store_get_for(const char *track_id, int album_id,
                             TrackEntry *out)
{
    MetaRecord rec;
    int dir_pos;
    int rc = -1;

    if (!track_id || !track_id[0] || !out) {
        return -1;
    }

    meta_lock();
    dir_pos = meta_unavailable_find(track_id, album_id);
    if (dir_pos >= 0) {
        memcpy(out, &s_unavailable[dir_pos], sizeof(*out));
        rc = 0;
    } else if (meta_ensure_loaded() == 0) {
        dir_pos = album_id != 0
            ? meta_dir_find(meta_hash_key(track_id, album_id), track_id,
                            album_id, &rec)
            : meta_dir_find_any(track_id, &rec);
        if (dir_pos >= 0) {
            memcpy(out, &rec.entry, sizeof(*out));
            /* RAM-only stamp refresh: keeps this session's LRU honest without
               a disk write per lookup. */
            s_stamps[s_dir[dir_pos].slot] = ++s_clock;
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
    MetaRecord rec;
    int dir_pos;
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

    dir_pos = meta_dir_find(meta_hash_key(entry->id, entry->album_id),
                            entry->id, entry->album_id, &rec);
    if (dir_pos >= 0) {
        slot = s_dir[dir_pos].slot;
    } else if (s_slot_count < META_STORE_SLOTS) {
        slot = s_slot_count;
    } else {
        /* Evict the least recently used slot. */
        int i;
        slot = 0;
        for (i = 1; i < META_STORE_SLOTS; i++) {
            if (s_stamps[i] < s_stamps[slot]) {
                slot = i;
            }
        }
        meta_dir_remove_slot(slot);
    }

    memset(&rec, 0, sizeof(rec));
    rec.magic = META_RECORD_MAGIC;
    rec.stamp = ++s_clock;
    memcpy(&rec.entry, entry, sizeof(rec.entry));

    if (meta_write_record(slot, &rec) == 0) {
        if (slot == s_slot_count) {
            s_slot_count++;
        }
        s_stamps[slot] = rec.stamp;
        if (dir_pos < 0) {
            s_dir[s_dir_count].hash = meta_hash_key(entry->id,
                                                    entry->album_id);
            s_dir[s_dir_count].slot = (uint16_t)slot;
            s_dir_count++;
        }
        rc = 0;
    }
    meta_unlock();
    return rc;
}
