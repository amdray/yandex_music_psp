#include "services/cover_cache.h"

#include <malloc.h>
#include <pspthreadman.h>
#include <psptypes.h>
#include <string.h>

#include "core/logger.h"
#include "services/image_loader.h"

#define MAX_CACHE_ENTRIES 50

static int evict_lru_internal(CoverCache *cache);

struct CoverCache {
    CoverCacheEntry entries[MAX_CACHE_ENTRIES];
    int entry_count;
    int max_size;
    CoverCacheEntry *lru_head;  // Most recently used
    CoverCacheEntry *lru_tail;  // Least recently used
    SceLwMutexWorkarea mutex;  // Protects all cache operations
    int mutex_initialized;
};

static void cache_lock(CoverCache *cache)
{
    if (cache && cache->mutex_initialized) {
        sceKernelLockLwMutex(&cache->mutex, 1, NULL);
    }
}

static void cache_unlock(CoverCache *cache)
{
    if (cache && cache->mutex_initialized) {
        sceKernelUnlockLwMutex(&cache->mutex, 1);
    }
}

static void lru_remove(CoverCache *cache, CoverCacheEntry *entry)
{
    if (!entry) {
        return;
    }
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        cache->lru_head = entry->next;
    }
    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cache->lru_tail = entry->prev;
    }
    entry->prev = NULL;
    entry->next = NULL;
}

static const char *normalize_size(const char *s)
{
    return (s && s[0]) ? s : "30x30";
}

static void lru_push_front(CoverCache *cache, CoverCacheEntry *entry)
{
    entry->prev = NULL;
    entry->next = cache->lru_head;
    if (cache->lru_head) {
        cache->lru_head->prev = entry;
    }
    cache->lru_head = entry;
    if (!cache->lru_tail) {
        cache->lru_tail = entry;
    }
}

void cover_cache_init(CoverCache **out_cache, int max_size)
{
    if (!out_cache || max_size <= 0 || max_size > MAX_CACHE_ENTRIES) {
        return;
    }

    CoverCache *cache = (CoverCache *)malloc(sizeof(CoverCache));
    if (!cache) {
        logLine("cover_cache: init alloc failed\n");
        return;
    }

    memset(cache, 0, sizeof(CoverCache));
    cache->max_size = max_size;
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    if (sceKernelCreateLwMutex(&cache->mutex, "cover_cache", 0, 0, NULL) < 0) {
        logLine("cover_cache: mutex create failed\n");
        free(cache);
        return;
    }
    cache->mutex_initialized = 1;

    *out_cache = cache;
    logLine("cover_cache: initialized max_size=%d\n", max_size);
}

void cover_cache_shutdown(CoverCache *cache)
{
    if (!cache) {
        return;
    }

    cache_lock(cache);

    for (int i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].rgba_data) {
            image_free_rgba8888(cache->entries[i].rgba_data);
            cache->entries[i].rgba_data = NULL;
        }
    }

    cache_unlock(cache);
    if (cache->mutex_initialized) {
        sceKernelDeleteLwMutex(&cache->mutex);
        cache->mutex_initialized = 0;
    }

    free(cache);
    logLine("cover_cache: shutdown\n");
}

const CoverCacheEntry *cover_cache_get(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                                        const char *cover_size)
{
    if (!cache || entity_id == 0) {
        return NULL;
    }

    const char *sz = normalize_size(cover_size);
    cache_lock(cache);

    CoverCacheEntry *entry = NULL;
    for (int i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].entity_type == entity_type &&
            cache->entries[i].entity_id == entity_id &&
            strcmp(cache->entries[i].cover_size, sz) == 0 &&
            cache->entries[i].rgba_data) {
            entry = &cache->entries[i];
            break;
        }
    }

    if (entry) {
        lru_remove(cache, entry);
        lru_push_front(cache, entry);
    }

    cache_unlock(cache);

    return entry;
}

int cover_cache_put(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                    const char *cover_size, void *rgba_data, int w, int h, int stride_bytes)
{
    if (!cache || entity_id == 0 || !rgba_data || w <= 0 || h <= 0) {
        return -1;
    }

    const char *sz = normalize_size(cover_size);
    cache_lock(cache);

    CoverCacheEntry *entry = NULL;
    for (int i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].entity_type == entity_type &&
            cache->entries[i].entity_id == entity_id &&
            strcmp(cache->entries[i].cover_size, sz) == 0) {
            entry = &cache->entries[i];
            break;
        }
    }

    const char *type_str = (entity_type == COVER_ENTITY_PLAYLIST) ? "playlist" :
                             (entity_type == COVER_ENTITY_ARTIST)   ? "artist" : "album";

    if (entry) {
        if (entry->rgba_data) {
            image_free_rgba8888(entry->rgba_data);
        }
        entry->rgba_data = rgba_data;
        entry->w = w;
        entry->h = h;
        entry->stride_bytes = stride_bytes;
        lru_remove(cache, entry);
        lru_push_front(cache, entry);
        cache_unlock(cache);
        logLine("cover_cache: replaced %s id=%d %dx%d\n", type_str, entity_id, w, h);
        return 0;
    }

    if (cache->entry_count < cache->max_size) {
        entry = &cache->entries[cache->entry_count++];
    } else {
        // Cache full - try to evict LRU (mutex already held)
        if (!evict_lru_internal(cache)) {
            // No eviction possible (all entries have ref_count > 0)
            cache_unlock(cache);
            logLine("cover_cache: put failed - cache full, all entries protected\n");
            return -1;
        }
        for (int i = 0; i < cache->entry_count; i++) {
            if (!cache->entries[i].rgba_data) {
                entry = &cache->entries[i];
                break;
            }
        }
        if (!entry) {
            cache_unlock(cache);
            return -1;
        }
    }

    entry->entity_type = entity_type;
    entry->entity_id = entity_id;
    strncpy(entry->cover_size, sz, sizeof(entry->cover_size) - 1);
    entry->cover_size[sizeof(entry->cover_size) - 1] = '\0';
    entry->rgba_data = rgba_data;
    entry->w = w;
    entry->h = h;
    entry->stride_bytes = stride_bytes;
    entry->ref_count = 0;
    lru_push_front(cache, entry);

    cache_unlock(cache);

    logLine("cover_cache: added %s id=%d %dx%d\n", type_str, entity_id, w, h);
    return 0;
}

void cover_cache_touch(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                       const char *cover_size)
{
    if (!cache || entity_id == 0) {
        return;
    }

    const char *sz = normalize_size(cover_size);
    cache_lock(cache);

    CoverCacheEntry *entry = NULL;
    for (int i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].entity_type == entity_type &&
            cache->entries[i].entity_id == entity_id &&
            strcmp(cache->entries[i].cover_size, sz) == 0) {
            entry = &cache->entries[i];
            break;
        }
    }

    if (entry) {
        lru_remove(cache, entry);
        lru_push_front(cache, entry);
    }

    cache_unlock(cache);
}

void cover_cache_ref(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                     const char *cover_size)
{
    if (!cache || entity_id == 0) {
        return;
    }

    const char *sz = normalize_size(cover_size);
    cache_lock(cache);

    for (int i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].entity_type == entity_type &&
            cache->entries[i].entity_id == entity_id &&
            strcmp(cache->entries[i].cover_size, sz) == 0) {
            cache->entries[i].ref_count++;
            break;
        }
    }

    cache_unlock(cache);
}

void cover_cache_unref(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                       const char *cover_size)
{
    if (!cache || entity_id == 0) {
        return;
    }

    const char *sz = normalize_size(cover_size);
    cache_lock(cache);

    for (int i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].entity_type == entity_type &&
            cache->entries[i].entity_id == entity_id &&
            strcmp(cache->entries[i].cover_size, sz) == 0) {
            if (cache->entries[i].ref_count > 0) {
                cache->entries[i].ref_count--;
            }
            break;
        }
    }

    cache_unlock(cache);
}

// Internal evict function (assumes mutex is already held)
// Evicts least recently used entry (respects ref_count protection)
static int evict_lru_internal(CoverCache *cache)
{
    if (!cache) {
        return 0;
    }

    // Walk from LRU tail to find first unprotected entry
    CoverCacheEntry *victim = cache->lru_tail;
    while (victim) {
        if (victim->ref_count == 0 && victim->rgba_data && victim->entity_id != 0) {
            // Found LRU entry that can be evicted
            const char *type_str = (victim->entity_type == COVER_ENTITY_PLAYLIST) ? "playlist" :
                                     (victim->entity_type == COVER_ENTITY_ARTIST)   ? "artist" : "album";
            int evicted_id = victim->entity_id;
            char evicted_size[16];
            strncpy(evicted_size, victim->cover_size, sizeof(evicted_size) - 1);
            evicted_size[sizeof(evicted_size) - 1] = '\0';
            if (victim->rgba_data) {
                image_free_rgba8888(victim->rgba_data);
                victim->rgba_data = NULL;
            }
            lru_remove(cache, victim);
            victim->entity_type = COVER_ENTITY_PLAYLIST;
            victim->entity_id = 0;
            victim->cover_size[0] = '\0';
            victim->w = 0;
            victim->h = 0;
            victim->stride_bytes = 0;
            logLine("cover_cache: evicted %s id=%d size=%s\n", type_str, evicted_id, evicted_size);
            return 1;
        }
        victim = victim->prev;
    }

    return 0;  // No eviction possible (all entries protected)
}

int cover_cache_evict_lru(CoverCache *cache)
{
    if (!cache) {
        return 0;
    }

    cache_lock(cache);

    int result = evict_lru_internal(cache);

    cache_unlock(cache);

    return result;
}

int cover_cache_remove(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                       const char *cover_size)
{
    if (!cache || entity_id == 0) {
        return 0;
    }

    const char *sz = normalize_size(cover_size);
    cache_lock(cache);

    const char *type_str = (entity_type == COVER_ENTITY_PLAYLIST) ? "playlist" :
                             (entity_type == COVER_ENTITY_ARTIST)   ? "artist" : "album";

    for (int i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].entity_type == entity_type &&
            cache->entries[i].entity_id == entity_id &&
            strcmp(cache->entries[i].cover_size, sz) == 0) {
            if (cache->entries[i].rgba_data) {
                image_free_rgba8888(cache->entries[i].rgba_data);
                cache->entries[i].rgba_data = NULL;
            }
            lru_remove(cache, &cache->entries[i]);
            cache->entries[i].entity_type = COVER_ENTITY_PLAYLIST;
            cache->entries[i].entity_id = 0;
            cache->entries[i].cover_size[0] = '\0';
            cache->entries[i].w = 0;
            cache->entries[i].h = 0;
            cache->entries[i].stride_bytes = 0;
            cache_unlock(cache);
            logLine("cover_cache: removed %s id=%d size=%s\n", type_str, entity_id, sz);
            return 1;
        }
    }

    cache_unlock(cache);

    return 0;
}
