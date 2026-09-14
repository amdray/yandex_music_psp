#ifndef YM_PLAYBACK_QUEUE_H
#define YM_PLAYBACK_QUEUE_H

#include "app/track.h"
#include "services/list_index.h"

typedef enum {
    PLAYBACK_QUEUE_SOURCE_NONE = 0,
    PLAYBACK_QUEUE_SOURCE_ALBUM,
    PLAYBACK_QUEUE_SOURCE_PLAYLIST,
    PLAYBACK_QUEUE_SOURCE_FLOW
} PlaybackQueueSource;

typedef enum {
    PLAYBACK_ORDER_SEQUENTIAL = 0,
    PLAYBACK_ORDER_SHUFFLE
} PlaybackOrderMode;

typedef struct {
    int count;
    int current_index;
    int order_count;
    int order_pos;
    PlaybackQueueSource source;
    PlaybackOrderMode order_mode;
    int source_generation;
    int source_id;
} PlaybackQueueInfo;

/* Resolution results for get_current/get_next/get_previous. */
#define PLAYBACK_QUEUE_OK 0
#define PLAYBACK_QUEUE_EMPTY (-1)
/* The id is known but its metadata is not in the store yet: out->id is filled,
   the rest of *out is zeroed. Ask the hydrator for out->id and retry. */
#define PLAYBACK_QUEUE_PENDING (-2)

int playback_queue_init(void);
void playback_queue_clear(void);

/* The queue owns a private copy of the id order; metadata is resolved from
   the track metadata store at access time, so the queue spans the whole list
   regardless of what is hydrated in the UI window. */
int playback_queue_set_from_ids(
    const ListIndexId *ids,
    int count,
    int selected_index,
    PlaybackQueueSource source,
    int source_id,
    int generation);

int playback_queue_set_order_mode(PlaybackOrderMode mode);

int playback_queue_get_current(TrackEntry *out);
int playback_queue_get_next(TrackEntry *out);
int playback_queue_get_previous(TrackEntry *out);

int playback_queue_move_next(void);
int playback_queue_move_previous(void);

int playback_queue_get_info(PlaybackQueueInfo *out);

#endif
