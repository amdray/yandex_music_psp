#include "services/track_meta_store.h"
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/fs.h"
#include "core/logger.h"
#include "services/ym_api.h"
#include "services/track_hydrator.h"

#define META_PATH "data/cache/meta/catalog.bin"
#define META_MAGIC 0x354D5954u
/* One disk batch matches the existing metadata request capacity. */
#define META_BATCH YM_API_HYDRATE_MAX

typedef struct {
    TrackRef ref;
    SceOff offset;
    TrackEntry *ram;
    unsigned int version;
    unsigned int view_generation;
    int dirty;
    int previous, next;
} MetaItem;
typedef struct { uint32_t magic, size; TrackRef ref; TrackEntry entry; } MetaRecord;
static MetaItem *s_items;
static int s_count, s_capacity;
static int s_ram_count, s_lru_head = -1, s_lru_tail = -1;
/* Committed backing records belong to this process; protected by s_io_mutex. */
static SceOff s_committed_end;
static SceLwMutexWorkarea s_mutex, s_io_mutex;
static SceUID s_thread = -1, s_sema = -1;
static volatile int s_running;
static int s_initialized, s_write_error;
/* Consumer demands and the executing job have independent lifetimes. */
static TrackRef s_needed[TRACK_HYDRATOR_CONSUMERS + 1][TRACK_HYDRATOR_JOB_MAX];
static int s_needed_count[TRACK_HYDRATOR_CONSUMERS + 1];
static void lock(void) { sceKernelLockLwMutex(&s_mutex, 1, NULL); }
static void unlock(void) { sceKernelUnlockLwMutex(&s_mutex, 1); }
static void lru_remove(int pos)
{
    MetaItem *item = &s_items[pos];
    if (item->previous >= 0) s_items[item->previous].next = item->next;
    else s_lru_head = item->next;
    if (item->next >= 0) s_items[item->next].previous = item->previous;
    else s_lru_tail = item->previous;
    item->previous = item->next = -1;
}
static void lru_touch(int pos, int added)
{
    if (added) ++s_ram_count;
    else lru_remove(pos);
    s_items[pos].previous = -1;
    s_items[pos].next = s_lru_head;
    if (s_lru_head >= 0) s_items[s_lru_head].previous = pos;
    else s_lru_tail = pos;
    s_lru_head = pos;
}
static void trim_ram(void)
{
    for (int i = s_lru_tail; i >= 0 && s_ram_count > TRACK_META_RAM_CAPACITY;) {
        MetaItem *item = &s_items[i];
        int previous = item->previous;
        if (item->dirty || item->offset < 0) { i = previous; continue; }
        int needed = 0;
        for (int c = 0; c <= TRACK_HYDRATOR_CONSUMERS && !needed; ++c)
            for (int j = 0; j < s_needed_count[c]; ++j)
                if (strcmp(item->ref.id, s_needed[c][j].id) == 0 &&
                    item->ref.album_id == s_needed[c][j].album_id) {
                    needed = 1; break;
                }
        if (!needed) {
            lru_remove(i);
            free(item->ram); item->ram = NULL;
            --s_ram_count;
        }
        i = previous;
    }
}
void track_meta_store_set_working_set(int consumer, const TrackRef *refs, int count)
{
    if (!s_initialized || consumer < 0 || consumer >= TRACK_HYDRATOR_CONSUMERS ||
        count < 0 || count > TRACK_HYDRATOR_JOB_MAX || (count && !refs)) return;
    lock();
    if (count) memcpy(s_needed[consumer], refs, (size_t)count * sizeof(*refs));
    s_needed_count[consumer] = count;
    trim_ram();
    unlock();
}
void track_meta_store_set_active_job(const TrackRef *refs, int count)
{
    if (!s_initialized || count < 0 || count > TRACK_HYDRATOR_JOB_MAX ||
        (count && !refs)) return;
    lock();
    if (count) memcpy(s_needed[TRACK_HYDRATOR_CONSUMERS], refs, (size_t)count * sizeof(*refs));
    s_needed_count[TRACK_HYDRATOR_CONSUMERS] = count;
    trim_ram();
    unlock();
}
static int find_item(const char *id, int album)
{
    for (int i = 0; i < s_count; ++i)
        if (strcmp(s_items[i].ref.id, id) == 0 &&
            s_items[i].ref.album_id == album) return i;
    return -1;
}
static int add_item(const TrackRef *ref)
{
    if (s_count == s_capacity) {
        int capacity = s_capacity ? s_capacity * 2 : META_BATCH;
        if (capacity <= s_capacity || (size_t)capacity > SIZE_MAX / sizeof(MetaItem)) return -1;
        MetaItem *items = realloc(s_items, (size_t)capacity * sizeof(*items));
        if (!items) return -1;
        s_items = items;
        s_capacity = capacity;
    }
    MetaItem *item = &s_items[s_count];
    memset(item, 0, sizeof(*item));
    item->ref = *ref;

    item->offset = -1;
    item->previous = item->next = -1;
    return s_count++;
}
static int transfer(SceUID fd, void *data, size_t size, int writing)
{
    unsigned char *p = data;
    while (size) {
        int n = writing ? fs_write(fd, p, size) : fs_read(fd, p, size);
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}
int track_meta_store_get_for(const char *id, int album, TrackEntry *out)
{
    if (!s_initialized || !id || !out) return TRACK_META_ERROR;
    lock();
    int pos = find_item(id, album);
    int rc = TRACK_META_MISSING;
    if (pos >= 0 && s_items[pos].ram) {
        *out = *s_items[pos].ram; lru_touch(pos, 0); rc = 0;
    }
    unlock();
    return rc;
}
int track_meta_store_load_for(const char *id, int album, TrackEntry *out,
                             unsigned int view_generation)
{
    if (!s_initialized || !id || !out) return TRACK_META_ERROR;
    int rc;
    sceKernelLockLwMutex(&s_io_mutex, 1, NULL);
    lock();
    int pos = find_item(id, album);
    if (view_generation && (pos < 0 || s_items[pos].view_generation != view_generation)) {
        unlock();
        sceKernelUnlockLwMutex(&s_io_mutex, 1);
        return TRACK_META_MISSING;
    }
    if (pos >= 0 && s_items[pos].ram) {
        *out = *s_items[pos].ram;
        lru_touch(pos, 0);
        unlock();
        sceKernelUnlockLwMutex(&s_io_mutex, 1);
        return 0;
    }
    SceOff offset = pos >= 0 ? s_items[pos].offset : -1;
    unlock();
    if (offset >= 0) {
        MetaRecord record;
        SceUID fd = fs_open(META_PATH, PSP_O_RDONLY, 0);
        rc = TRACK_META_ERROR;
        if (fd >= 0) {
            int ok = fs_lseek(fd, offset, PSP_SEEK_SET) == offset &&
                transfer(fd, &record, sizeof(record), 0) == 0;
            if (fs_close(fd) < 0) ok = 0;
            if (ok && record.magic == META_MAGIC && record.size == sizeof(record)) {
                TrackEntry *copy = malloc(sizeof(*copy));
                if (copy) {
                    *copy = record.entry;
                    lock();
                    int added = s_items[pos].ram == NULL;
                    if (added) { s_items[pos].ram = copy; copy = NULL; }
                    lru_touch(pos, added);
                    *out = *s_items[pos].ram;
                    trim_ram();
                    unlock();
                    free(copy);
                    rc = 0;
                }
            }
        }
    } else rc = TRACK_META_MISSING;
    sceKernelUnlockLwMutex(&s_io_mutex, 1);
    return rc;
}
int track_meta_store_put(const TrackRef *ref, const TrackEntry *entry,
                        unsigned int view_generation)
{
    if (!s_initialized || !ref || !ref->id[0] || !entry || !entry->id[0]) return -1;
    int rc = 0;
    lock();
    int pos = find_item(ref->id, ref->album_id);
    if (pos < 0) pos = add_item(ref);
    if (pos >= 0) {
        MetaItem *item = &s_items[pos];
        int added = item->ram == NULL;
        if (!item->ram) item->ram = malloc(sizeof(*item->ram));
        if (item->ram) {
            *item->ram = *entry;
            if (view_generation) item->view_generation = view_generation;
            ++item->version;
            item->dirty = 1;
            lru_touch(pos, added);
            trim_ram();
        } else rc = -1;
    } else rc = -1;
    unlock();
    if (rc == 0) sceKernelSignalSema(s_sema, 1);
    return rc;
}
static int flush_batch(void)
{
    MetaRecord *records = malloc(META_BATCH * sizeof(*records));
    int indices[META_BATCH];
    unsigned int versions[META_BATCH];
    int count = 0;
    if (!records) return -1;
    sceKernelLockLwMutex(&s_io_mutex, 1, NULL);
    lock();
    for (int i = 0; i < s_count && count < META_BATCH; ++i) {
        if (!s_items[i].dirty) continue;
        indices[count] = i;
        versions[count] = s_items[i].version;
        records[count].magic = META_MAGIC;
        records[count].size = sizeof(MetaRecord);
        records[count].ref = s_items[i].ref;
        records[count++].entry = *s_items[i].ram;
    }
    unlock();
    int rc = 0;
    SceOff offset = -1;
    if (count) {
        int flags = PSP_O_WRONLY;
        if (s_committed_end == 0) {
            fs_ensure_dir(META_PATH);
            flags |= PSP_O_CREAT | PSP_O_TRUNC;
        }
        SceUID fd = fs_open(META_PATH, flags, 0666);
        rc = -1;
        if (fd >= 0) {
            offset = fs_lseek(fd, s_committed_end, PSP_SEEK_SET);
            if (offset == s_committed_end)
                rc = transfer(fd, records, (size_t)count * sizeof(*records), 1);
            if (fs_close(fd) < 0) rc = -1;
        }
        if (rc == 0) {
            s_committed_end += (SceOff)count * sizeof(*records);
            lock();
            for (int i = 0; i < count; ++i) {
                MetaItem *item = &s_items[indices[i]];
                item->offset = offset + (SceOff)(i * sizeof(*records));
                if (item->version == versions[i]) item->dirty = 0;
            }
            trim_ram();
            unlock();
        }
    }
    sceKernelUnlockLwMutex(&s_io_mutex, 1);
    free(records);
    return rc < 0 ? -1 : count;
}
static int meta_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        if (sceKernelWaitSema(s_sema, 1, NULL) < 0) break;
        int rc;
        do { rc = flush_batch(); } while (rc > 0);
        s_write_error = rc < 0;
        if (rc < 0) logLine("meta: catalog write failed\n");
        if (!s_running) break;
    }
    return 0;
}
int track_meta_store_init(void)
{
    if (s_initialized) return 0;
    if (sceKernelCreateLwMutex(&s_mutex, "meta", 0, 0, NULL) < 0) return -1;
    if (sceKernelCreateLwMutex(&s_io_mutex, "meta_io", 0, 0, NULL) < 0) {
        sceKernelDeleteLwMutex(&s_mutex); return -1;
    }
    s_sema = sceKernelCreateSema("meta", 0, 0, 1, NULL);
    if (s_sema < 0) goto failed;
    s_running = 1;
    s_thread = sceKernelCreateThread("meta", meta_worker, 0x32, 0x4000, 0, NULL);
    if (s_thread < 0) goto failed;
    if (sceKernelStartThread(s_thread, 0, NULL) < 0) {
        sceKernelDeleteThread(s_thread); s_thread = -1; goto failed;
    }
    s_initialized = 1;
    return 0;
failed:
    s_running = 0;
    if (s_sema >= 0) sceKernelDeleteSema(s_sema);
    s_sema = -1;
    sceKernelDeleteLwMutex(&s_io_mutex);
    sceKernelDeleteLwMutex(&s_mutex);
    return -1;
}
int track_meta_store_quiesce(void)
{
    if (!s_initialized) return 0;
    s_running = 0;
    sceKernelSignalSema(s_sema, 1);
    SceUInt timeout = 20000000U;
    if (sceKernelWaitThreadEnd(s_thread, &timeout) < 0) return -1;
    return s_write_error ? -1 : 0;
}
int track_meta_store_shutdown(void)
{
    if (!s_initialized) return 0;
    if (track_meta_store_quiesce() < 0) return -1;
    sceKernelDeleteThread(s_thread);
    sceKernelDeleteSema(s_sema);
    for (int i = 0; i < s_count; ++i) free(s_items[i].ram);
    free(s_items); s_items = NULL; s_count = s_capacity = 0;
    s_ram_count = 0; s_lru_head = s_lru_tail = -1;
    sceKernelDeleteLwMutex(&s_io_mutex);
    sceKernelDeleteLwMutex(&s_mutex);
    s_initialized = s_write_error = 0;
    s_committed_end = 0;
    memset(s_needed_count, 0, sizeof(s_needed_count));
    return s_write_error ? -1 : 0;
}
