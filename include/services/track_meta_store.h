#ifndef YM_SERVICES_TRACK_META_STORE_H
#define YM_SERVICES_TRACK_META_STORE_H
#include "app/track.h"
#define TRACK_META_MISSING (-1)
#define TRACK_META_ERROR (-2)
/* Retention target; active and unsaved records cannot be evicted. */
#define TRACK_META_RAM_CAPACITY 4096
int track_meta_store_init(void);
int track_meta_store_shutdown(void);
int track_meta_store_quiesce(void);
void track_meta_store_set_working_set(int consumer, const TrackRef *refs, int count);
/* The single hydrator worker owns this set until its active job finishes. */
void track_meta_store_set_active_job(const TrackRef *refs, int count);
/* RAM-only lookup, safe on the main thread. */
int track_meta_store_get_for(const char *id, int album_id, TrackEntry *out);
/* Worker-only lookup. view_generation=0 accepts shared metadata; a screen
 * requires a successful refresh for its own generation, including disk hits. */
int track_meta_store_load_for(const char *id, int album_id, TrackEntry *out,
                             unsigned int view_generation);
/* A nonzero view generation marks a successful screen refresh. Shared
 * publications preserve that mark; metadata may name a different album. */
int track_meta_store_put(const TrackRef *ref, const TrackEntry *entry,
                        unsigned int view_generation);
#endif
