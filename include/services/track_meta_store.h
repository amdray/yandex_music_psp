#ifndef YM_SERVICES_TRACK_META_STORE_H
#define YM_SERVICES_TRACK_META_STORE_H

#include "app/track.h"

/* Global on-Memory-Stick store of full track metadata, keyed by track id and
   album id. A track may occur in multiple releases with different album
   metadata, so those contexts are stored separately and survive list revision
   changes. A revision bump invalidates only the cheap id-index.

   Fixed-size slots in one file + an in-RAM directory (built by a sequential
   scan on first use). LRU eviction; stamps are refreshed on write, and only
   in RAM on reads, so cross-session LRU is approximate by design.

   Thread-safe (single mutex). Callers are worker threads; the UI must read
   from its own hydrated RAM window, never from here (disk I/O inside). */

int track_meta_store_init(void);
void track_meta_store_shutdown(void);

/* 0 = hit, out filled; -1 = miss or I/O error. */
int track_meta_store_get(const char *track_id, TrackEntry *out);

/* Exact lookup by track and album context. album_id == 0 keeps the generic
   legacy lookup semantics and returns the freshest record for track_id. */
int track_meta_store_get_for(const char *track_id, int album_id,
                             TrackEntry *out);

/* Insert or refresh entry (key = entry->id + entry->album_id). Unavailable
   entries live only in the current run's RAM cache because rights may change
   between runs. */
int track_meta_store_put(const TrackEntry *entry);

#endif
