#include "services/playback_queue.h"

#include <stdlib.h>
#include <string.h>

#include "core/logger.h"
#include "services/track_meta_store.h"

typedef struct {
    ListIndexId *ids;   /* private copy of the source order */
    int *order;         /* playback order: indices into ids */
    int count;
    int current_index;
    int order_count;
    int order_pos;
    PlaybackQueueSource source;
    PlaybackOrderMode order_mode;
    int source_generation;
    int source_id;
    int initialized;
} PlaybackQueueState;

static PlaybackQueueState s_queue;

static unsigned int queue_seed(void)
{
    unsigned int seed = 0x6D2B79F5u;
    seed ^= (unsigned int)(s_queue.count * 2654435761u);
    seed ^= (unsigned int)(s_queue.source_generation * 2246822519u);
    seed ^= (unsigned int)(s_queue.source_id * 3266489917u);
    seed ^= (unsigned int)((s_queue.current_index + 1) * 668265263u);
    return seed;
}

static unsigned int queue_next_random(unsigned int *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static void queue_reset_order(void)
{
    int i;

    s_queue.order_count = s_queue.count;
    if (s_queue.count <= 0) {
        s_queue.order_pos = 0;
        return;
    }

    if (s_queue.order_mode == PLAYBACK_ORDER_SHUFFLE) {
        int write_pos = 0;
        unsigned int rng_state = queue_seed();

        s_queue.order[write_pos++] = s_queue.current_index;
        for (i = 0; i < s_queue.count; ++i) {
            if (i == s_queue.current_index) {
                continue;
            }
            s_queue.order[write_pos++] = i;
        }

        for (i = s_queue.count - 1; i > 1; --i) {
            unsigned int pick = queue_next_random(&rng_state);
            int swap_pos = 1 + (int)(pick % (unsigned int)i);
            int tmp = s_queue.order[i];
            s_queue.order[i] = s_queue.order[swap_pos];
            s_queue.order[swap_pos] = tmp;
        }

        s_queue.order_pos = 0;
        return;
    }

    for (i = 0; i < s_queue.count; ++i) {
        s_queue.order[i] = i;
    }
    s_queue.order_pos = s_queue.current_index;
}

static int queue_validate_index(int index)
{
    return index >= 0 && index < s_queue.count;
}

static void queue_free_storage(void)
{
    if (s_queue.ids) {
        free(s_queue.ids);
        s_queue.ids = NULL;
    }
    if (s_queue.order) {
        free(s_queue.order);
        s_queue.order = NULL;
    }
}

/* Resolve queue position -> full entry via the metadata store. On a store
   miss the caller gets the bare id (PLAYBACK_QUEUE_PENDING) to hand to the
   hydrator; metadata is immutable, so a later retry succeeds. */
static int queue_resolve(int index, TrackEntry *out)
{
    if (!out || !queue_validate_index(index)) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    if (track_meta_store_get(s_queue.ids[index], out) == 0) {
        return PLAYBACK_QUEUE_OK;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->id, s_queue.ids[index], sizeof(out->id) - 1);
    return PLAYBACK_QUEUE_PENDING;
}

int playback_queue_init(void)
{
    memset(&s_queue, 0, sizeof(s_queue));
    s_queue.initialized = 1;
    logLine("pq: init\n");
    return 0;
}

void playback_queue_clear(void)
{
    queue_free_storage();
    memset(&s_queue, 0, sizeof(s_queue));
    s_queue.initialized = 1;
    logLine("pq: clear\n");
}

int playback_queue_set_from_ids(const ListIndexId *ids,
                                 int count,
                                 int selected_index,
                                 PlaybackQueueSource source,
                                 int source_id,
                                 int generation)
{
    PlaybackQueueState next;
    ListIndexId *old_ids;
    int *old_order;
    int i;

    if (!s_queue.initialized) {
        playback_queue_init();
    }
    if (!ids || count <= 0) {
        return -1;
    }
    if (selected_index < 0 || selected_index >= count) {
        return -1;
    }
    if ((size_t)count > SIZE_MAX / sizeof(ListIndexId) ||
        (size_t)count > SIZE_MAX / sizeof(int)) {
        logLine("pq: set failed, size overflow for %d ids\n", count);
        return -1;
    }

    memset(&next, 0, sizeof(next));
    next.ids = (ListIndexId *)malloc((size_t)count * sizeof(ListIndexId));
    next.order = (int *)malloc((size_t)count * sizeof(int));
    if (!next.ids || !next.order) {
        free(next.ids);
        free(next.order);
        logLine("pq: set failed, no memory for %d ids\n", count);
        return -1;
    }

    memcpy(next.ids, ids, (size_t)count * sizeof(ListIndexId));
    for (i = 0; i < count; ++i) {
        next.order[i] = i;
    }
    next.count = count;
    next.current_index = selected_index;
    next.order_count = count;
    next.order_pos = selected_index;
    next.source = source;
    next.order_mode = PLAYBACK_ORDER_SEQUENTIAL;
    next.source_generation = generation;
    next.source_id = source_id;
    next.initialized = 1;

    old_ids = s_queue.ids;
    old_order = s_queue.order;
    s_queue = next;
    free(old_ids);
    free(old_order);

    logLine("pq: set source=%d source_id=%d generation=%d count=%d current=%d track_id='%s'\n",
            (int)s_queue.source,
            s_queue.source_id,
            s_queue.source_generation,
            s_queue.count,
            s_queue.current_index,
            s_queue.ids[s_queue.current_index]);
    return 0;
}

int playback_queue_set_order_mode(PlaybackOrderMode mode)
{
    if (!s_queue.initialized || s_queue.count <= 0) {
        return -1;
    }
    if (mode != PLAYBACK_ORDER_SEQUENTIAL && mode != PLAYBACK_ORDER_SHUFFLE) {
        return -1;
    }

    s_queue.order_mode = mode;
    queue_reset_order();
    logLine("pq: order mode=%d current=%d order_pos=%d\n",
            (int)s_queue.order_mode,
            s_queue.current_index,
            s_queue.order_pos);
    return 0;
}

int playback_queue_get_current(TrackEntry *out)
{
    return queue_resolve(s_queue.current_index, out);
}

int playback_queue_get_next(TrackEntry *out)
{
    int next_pos;

    if (!out || s_queue.order_count <= 0) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    next_pos = s_queue.order_pos + 1;
    if (next_pos < 0 || next_pos >= s_queue.order_count) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    return queue_resolve(s_queue.order[next_pos], out);
}

int playback_queue_get_previous(TrackEntry *out)
{
    int prev_pos;

    if (!out || s_queue.order_count <= 0) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    prev_pos = s_queue.order_pos - 1;
    if (prev_pos < 0 || prev_pos >= s_queue.order_count) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    return queue_resolve(s_queue.order[prev_pos], out);
}

int playback_queue_move_next(void)
{
    int next_pos = s_queue.order_pos + 1;

    if (next_pos < 0 || next_pos >= s_queue.order_count) {
        return -1;
    }

    s_queue.order_pos = next_pos;
    s_queue.current_index = s_queue.order[s_queue.order_pos];
    return 0;
}

int playback_queue_move_previous(void)
{
    int prev_pos = s_queue.order_pos - 1;

    if (prev_pos < 0 || prev_pos >= s_queue.order_count) {
        return -1;
    }

    s_queue.order_pos = prev_pos;
    s_queue.current_index = s_queue.order[s_queue.order_pos];
    return 0;
}

int playback_queue_get_info(PlaybackQueueInfo *out)
{
    if (!out) {
        return -1;
    }

    out->count = s_queue.count;
    out->current_index = s_queue.current_index;
    out->order_count = s_queue.order_count;
    out->order_pos = s_queue.order_pos;
    out->source = s_queue.source;
    out->order_mode = s_queue.order_mode;
    out->source_generation = s_queue.source_generation;
    out->source_id = s_queue.source_id;
    return 0;
}

int playback_queue_get_ids(ListIndexId *out, int max_count)
{
    int n, i;

    if (!out || max_count <= 0 || !s_queue.ids || s_queue.count <= 0) {
        return -1;
    }
    n = (s_queue.count < max_count) ? s_queue.count : max_count;
    for (i = 0; i < n; i++) {
        memcpy(out[i], s_queue.ids[i], sizeof(ListIndexId));
    }
    return n;
}

int playback_queue_append_ids(const ListIndexId *ids, int count)
{
    ListIndexId *grown_ids;
    int *grown_order;
    int new_count, i;

    if (!s_queue.initialized || !ids || count <= 0) {
        return -1;
    }
    if ((size_t)s_queue.count + (size_t)count > 512) {
        return -1;  // волна не должна расти бесконечно
    }
    new_count = s_queue.count + count;
    grown_ids = (ListIndexId *)realloc(s_queue.ids,
        (size_t)new_count * sizeof(ListIndexId));
    grown_order = (int *)realloc(s_queue.order,
        (size_t)new_count * sizeof(int));
    if (!grown_ids || !grown_order) {
        // realloc при неудаче оставляет старые блоки целыми.
        free(grown_ids);
        free(grown_order);
        return -1;
    }
    s_queue.ids = grown_ids;
    s_queue.order = grown_order;
    for (i = 0; i < count; i++) {
        memcpy(s_queue.ids[s_queue.count + i], ids[i], sizeof(ListIndexId));
        s_queue.order[s_queue.order_count + i] = s_queue.count + i;
    }
    s_queue.count = new_count;
    s_queue.order_count = new_count;
    logLine("pq: append %d ids, count=%d\n", count, new_count);
    return 0;
}
