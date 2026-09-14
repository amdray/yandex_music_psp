#ifndef YM_SERVICES_COVER_CACHE_H
#define YM_SERVICES_COVER_CACHE_H

#include <psptypes.h>
#include "cover_storage.h"  // For CoverEntityType

// Cover cache entry - stores decoded image data in RAM
typedef struct CoverCacheEntry {
    CoverEntityType entity_type;  // Тип сущности (плейлист или альбом)
    int entity_id;                 // ID плейлиста или альбома
    char cover_size[16];           // Размер обложки ("30x30", "200x200"), часть ключа кэша
    void *rgba_data;               // RAM buffer with decoded RGBA8888 image
    int w, h;                      // Image dimensions
    int stride_bytes;              // Row stride in bytes (aligned)
    int ref_count;                 // Reference count (protects from eviction when > 0)
    struct CoverCacheEntry *prev;  // LRU list previous
    struct CoverCacheEntry *next;  // LRU list next
} CoverCacheEntry;

// Cover cache - thread-safe LRU cache
typedef struct CoverCache CoverCache;

// Initialize cache with max entries
void cover_cache_init(CoverCache **out_cache, int max_size);

// Shutdown and free all resources
void cover_cache_shutdown(CoverCache *cache);

// Thread-safe: Get cover by (entity_type, entity_id, cover_size) — returns NULL if not found.
// cover_size NULL or "" treated as "30x30". Moves entry to front of LRU.
const CoverCacheEntry *cover_cache_get(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                                       const char *cover_size);

// Thread-safe: Put cover into cache (takes ownership of rgba_data).
// cover_size NULL or "" treated as "30x30". If cache is full, evicts LRU (respects ref_count).
// Returns 0 on success, -1 on error.
int cover_cache_put(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                    const char *cover_size, void *rgba_data, int w, int h, int stride_bytes);

// Thread-safe: Touch entry (move to front of LRU)
void cover_cache_touch(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                       const char *cover_size);

// Thread-safe: Increment reference count (protects from eviction)
void cover_cache_ref(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                     const char *cover_size);

// Thread-safe: Decrement reference count (allows eviction when reaches 0)
void cover_cache_unref(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                       const char *cover_size);

// Thread-safe: Evict least recently used entry (if ref_count == 0)
// Returns 1 if evicted, 0 if no eviction possible
int cover_cache_evict_lru(CoverCache *cache);

// Thread-safe: Remove specific entry by (entity_type, entity_id, cover_size)
// Returns 1 if removed, 0 if not found
int cover_cache_remove(CoverCache *cache, CoverEntityType entity_type, int entity_id,
                       const char *cover_size);

#endif
