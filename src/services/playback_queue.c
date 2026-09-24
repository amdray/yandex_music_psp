#include "services/playback_queue.h"

#include <stdlib.h>
#include <string.h>

#include "core/logger.h"
#include "services/track_meta_store.h"

typedef struct {
    TrackRef *items;    /* private track+album identity per queue position */
    int *order;         /* playback order: indices into items */
    char *flow_batch;   /* per-position batch_id, count * PLAYBACK_FLOW_BATCH_SIZE */
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
    if (s_queue.items) {
        free(s_queue.items);
        s_queue.items = NULL;
    }
    if (s_queue.order) {
        free(s_queue.order);
        s_queue.order = NULL;
    }
    if (s_queue.flow_batch) {
        free(s_queue.flow_batch);
        s_queue.flow_batch = NULL;
    }
}

/* Resolve queue position by the track+album identity. */
static int queue_resolve(int index, TrackEntry *out)
{
    if (!out || !queue_validate_index(index)) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    if (track_meta_store_get_for(s_queue.items[index].id,
                                 s_queue.items[index].album_id, out) == 0) {
        return PLAYBACK_QUEUE_OK;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->id, s_queue.items[index].id, sizeof(out->id));
    out->id[sizeof(out->id) - 1] = '\0';
    out->album_id = s_queue.items[index].album_id;
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

static int queue_set(const ListIndexId *ids,
                     const TrackRef *refs,
                     int count,
                     int selected_index,
                     PlaybackQueueSource source,
                     int source_id,
                     int generation)
{
    PlaybackQueueState next;
    TrackRef *old_items;
    int *old_order;
    char *old_flow_batch;
    int i;

    if (!s_queue.initialized) {
        playback_queue_init();
    }
    if ((!ids && !refs) || count <= 0) {
        return -1;
    }
    if (selected_index < 0 || selected_index >= count) {
        return -1;
    }
    if ((size_t)count > SIZE_MAX / sizeof(TrackRef) ||
        (size_t)count > SIZE_MAX / sizeof(int)) {
        logLine("pq: set failed, size overflow for %d ids\n", count);
        return -1;
    }

    memset(&next, 0, sizeof(next));
    next.items = (TrackRef *)calloc((size_t)count, sizeof(TrackRef));
    next.order = (int *)malloc((size_t)count * sizeof(int));
    next.flow_batch = (char *)calloc((size_t)count, PLAYBACK_FLOW_BATCH_SIZE);
    if (!next.items || !next.order || !next.flow_batch) {
        free(next.items);
        free(next.order);
        free(next.flow_batch);
        logLine("pq: set failed, no memory for %d ids\n", count);
        return -1;
    }

    for (i = 0; i < count; ++i) {
        if (refs) {
            memcpy(&next.items[i], &refs[i], sizeof(TrackRef));
            next.items[i].id[TRACK_ID_SIZE - 1] = '\0';
        } else {
            memcpy(next.items[i].id, ids[i], sizeof(ListIndexId));
            next.items[i].id[TRACK_ID_SIZE - 1] = '\0';
        }
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

    old_items = s_queue.items;
    old_order = s_queue.order;
    old_flow_batch = s_queue.flow_batch;
    s_queue = next;
    free(old_items);
    free(old_order);
    free(old_flow_batch);

    logLine("pq: set source=%d source_id=%d generation=%d count=%d current=%d track_id='%s' album_id=%d\n",
            (int)s_queue.source,
            s_queue.source_id,
            s_queue.source_generation,
            s_queue.count,
            s_queue.current_index,
            s_queue.items[s_queue.current_index].id,
            s_queue.items[s_queue.current_index].album_id);
    return 0;
}

int playback_queue_set_from_ids(const ListIndexId *ids,
                                 int count,
                                 int selected_index,
                                 PlaybackQueueSource source,
                                 int source_id,
                                 int generation)
{
    return queue_set(ids, NULL, count, selected_index,
                     source, source_id, generation);
}

int playback_queue_set_from_refs(const TrackRef *refs,
                                  int count,
                                  int selected_index,
                                  PlaybackQueueSource source,
                                  int source_id,
                                  int generation)
{
    return queue_set(NULL, refs, count, selected_index,
                     source, source_id, generation);
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

    if (!out || max_count <= 0 || !s_queue.items || s_queue.count <= 0) {
        return -1;
    }
    n = (s_queue.count < max_count) ? s_queue.count : max_count;
    for (i = 0; i < n; i++) {
        memcpy(out[i], s_queue.items[i].id, sizeof(ListIndexId));
    }
    return n;
}

int playback_queue_get_refs(TrackRef *out, int max_count)
{
    int n;
    int i;

    if (!out || max_count <= 0 || !s_queue.items || s_queue.count <= 0) {
        return -1;
    }
    n = (s_queue.count < max_count) ? s_queue.count : max_count;
    for (i = 0; i < n; i++) {
        memcpy(&out[i], &s_queue.items[i], sizeof(TrackRef));
    }
    return n;
}

int playback_queue_append_ids(const ListIndexId *ids, int count)
{
    TrackRef *new_items;
    int *new_order;
    char *new_batch;
    int new_count, i;

    if (!s_queue.initialized || !ids || count <= 0) {
        return -1;
    }
    if ((size_t)s_queue.count + (size_t)count > 512) {
        return -1;  // волна не должна расти бесконечно
    }
    new_count = s_queue.count + count;
    new_items = (TrackRef *)calloc((size_t)new_count, sizeof(TrackRef));
    new_order = (int *)malloc((size_t)new_count * sizeof(int));
    new_batch = (char *)calloc((size_t)new_count, PLAYBACK_FLOW_BATCH_SIZE);
    if (!new_items || !new_order || !new_batch) {
        free(new_items);
        free(new_order);
        free(new_batch);
        return -1;
    }
    if (s_queue.count > 0) {
        memcpy(new_items, s_queue.items,
               (size_t)s_queue.count * sizeof(TrackRef));
        memcpy(new_order, s_queue.order,
               (size_t)s_queue.order_count * sizeof(int));
        if (s_queue.flow_batch) {
            memcpy(new_batch, s_queue.flow_batch,
                   (size_t)s_queue.count * PLAYBACK_FLOW_BATCH_SIZE);
        }
    }
    for (i = 0; i < count; i++) {
        memcpy(new_items[s_queue.count + i].id, ids[i], sizeof(ListIndexId));
        new_items[s_queue.count + i].id[TRACK_ID_SIZE - 1] = '\0';
        new_order[s_queue.order_count + i] = s_queue.count + i;
    }
    free(s_queue.items);
    free(s_queue.order);
    free(s_queue.flow_batch);
    s_queue.items = new_items;
    s_queue.order = new_order;
    s_queue.flow_batch = new_batch;
    s_queue.count = new_count;
    s_queue.order_count = new_count;
    logLine("pq: append %d ids, count=%d\n", count, new_count);
    return 0;
}

/* WAVE-REPORT: FLOW metadata accessors and atomic append (spec sections 7/9).
 * The staging commit below never mutates the live queue until all four
 * arrays are ready; any allocation failure returns -1 with the old queue,
 * current index and old batch ids intact. */

static int queue_contains_flow(const char *track_id, int album_id)
{
    int i;

    if (!track_id || !track_id[0] || !s_queue.items) {
        return 0;
    }
    for (i = 0; i < s_queue.count; ++i) {
        if (strcmp(s_queue.items[i].id, track_id) == 0 &&
            s_queue.items[i].album_id == album_id) {
            return 1;
        }
    }
    return 0;
}

static int queue_resolve_flow(int index, WaveQueueItem *out)
{
    char *batch_src;

    if (!out || !queue_validate_index(index)) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->track_id, s_queue.items[index].id, sizeof(ListIndexId));
    out->album_id = s_queue.items[index].album_id;
    if (s_queue.flow_batch) {
        batch_src = s_queue.flow_batch + (size_t)index * PLAYBACK_FLOW_BATCH_SIZE;
        strncpy(out->batch_id, batch_src, sizeof(out->batch_id) - 1);
        out->batch_id[sizeof(out->batch_id) - 1] = '\0';
    }
    return PLAYBACK_QUEUE_OK;
}

int playback_queue_set_flow_meta(int index, int album_id, const char *batch_id)
{
    char *batch_dst;

    if (!s_queue.initialized || !queue_validate_index(index)) {
        return -1;
    }
    if (!s_queue.items || !s_queue.flow_batch) {
        return -1;
    }
    s_queue.items[index].album_id = album_id;
    batch_dst = s_queue.flow_batch + (size_t)index * PLAYBACK_FLOW_BATCH_SIZE;
    if (batch_id && batch_id[0]) {
        strncpy(batch_dst, batch_id, PLAYBACK_FLOW_BATCH_SIZE - 1);
        batch_dst[PLAYBACK_FLOW_BATCH_SIZE - 1] = '\0';
    } else {
        batch_dst[0] = '\0';
    }
    return 0;
}

int playback_queue_get_current_flow(WaveQueueItem *out)
{
    return queue_resolve_flow(s_queue.current_index, out);
}

int playback_queue_get_next_flow(WaveQueueItem *out)
{
    int next_pos;

    if (!out || s_queue.order_count <= 0) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    next_pos = s_queue.order_pos + 1;
    if (next_pos < 0 || next_pos >= s_queue.order_count) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    return queue_resolve_flow(s_queue.order[next_pos], out);
}

int playback_queue_get_previous_flow(WaveQueueItem *out)
{
    int prev_pos;

    if (!out || s_queue.order_count <= 0) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    prev_pos = s_queue.order_pos - 1;
    if (prev_pos < 0 || prev_pos >= s_queue.order_count) {
        return PLAYBACK_QUEUE_EMPTY;
    }
    return queue_resolve_flow(s_queue.order[prev_pos], out);
}

int playback_queue_get_flow_items(WaveQueueItem *out, int max_count)
{
    int n;
    int i;

    if (!out || max_count <= 0 || !s_queue.items || s_queue.count <= 0) {
        return -1;
    }
    n = (s_queue.count < max_count) ? s_queue.count : max_count;
    for (i = 0; i < n; i++) {
        queue_resolve_flow(i, &out[i]);
    }
    return n;
}

int playback_queue_append_flow_items(const WaveQueueItem *items, int count)
{
    TrackRef *new_items;
    int *new_order;
    char *new_batch;
    int new_count;
    int src;
    int prev;
    int added;
    int tail;

    if (!s_queue.initialized || !items || count <= 0) {
        return -1;
    }

    /* Count genuinely new items: drop empties, queue dupes and dupes inside
     * the incoming batch (key track-id:album-id). */
    added = 0;
    for (src = 0; src < count; ++src) {
        int dup = 0;

        if (!items[src].track_id[0]) {
            continue;
        }
        if (queue_contains_flow(items[src].track_id, items[src].album_id)) {
            continue;
        }
        for (prev = 0; prev < src; ++prev) {
            if (strcmp(items[prev].track_id, items[src].track_id) == 0 &&
                items[prev].album_id == items[src].album_id) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            added++;
        }
    }
    if (added == 0) {
        return 0;
    }
    if ((size_t)s_queue.count + (size_t)added > 512) {
        logLine("pq: flow append refused, would exceed cap\n");
        return -1;
    }

    /* Staging allocation: nothing live is touched until all three arrays
     * are ready, then one commit replaces the storage. */
    new_count = s_queue.count + added;
    new_items = (TrackRef *)malloc((size_t)new_count * sizeof(TrackRef));
    new_order = (int *)malloc((size_t)new_count * sizeof(int));
    new_batch = (char *)malloc((size_t)new_count * PLAYBACK_FLOW_BATCH_SIZE);
    if (!new_items || !new_order || !new_batch) {
        free(new_items);
        free(new_order);
        free(new_batch);
        logLine("pq: flow append failed, no memory for %d items\n", added);
        return -1;
    }

    if (s_queue.count > 0) {
        if (s_queue.items) {
            memcpy(new_items, s_queue.items,
                   (size_t)s_queue.count * sizeof(TrackRef));
        }
        if (s_queue.order) {
            memcpy(new_order, s_queue.order, (size_t)s_queue.order_count * sizeof(int));
        }
        if (s_queue.flow_batch) {
            memcpy(new_batch, s_queue.flow_batch,
                   (size_t)s_queue.count * PLAYBACK_FLOW_BATCH_SIZE);
        } else {
            memset(new_batch, 0, (size_t)s_queue.count * PLAYBACK_FLOW_BATCH_SIZE);
        }
    }

    /* Sequential order tail (FLOW order is server-defined; shuffle unused). */
    tail = 0;
    for (src = 0; src < count; ++src) {
        int dup = 0;
        int dst;

        if (!items[src].track_id[0]) {
            continue;
        }
        if (queue_contains_flow(items[src].track_id, items[src].album_id)) {
            continue;
        }
        for (prev = 0; prev < src; ++prev) {
            if (strcmp(items[prev].track_id, items[src].track_id) == 0 &&
                items[prev].album_id == items[src].album_id) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        dst = s_queue.count + tail;
        memset(&new_items[dst], 0, sizeof(new_items[dst]));
        memcpy(new_items[dst].id, items[src].track_id, sizeof(ListIndexId));
        new_items[dst].id[TRACK_ID_SIZE - 1] = '\0';
        new_items[dst].album_id = items[src].album_id;
        new_order[s_queue.order_count + tail] = dst;
        strncpy(new_batch + (size_t)dst * PLAYBACK_FLOW_BATCH_SIZE,
                items[src].batch_id, PLAYBACK_FLOW_BATCH_SIZE - 1);
        new_batch[(size_t)dst * PLAYBACK_FLOW_BATCH_SIZE + PLAYBACK_FLOW_BATCH_SIZE - 1] = '\0';
        tail++;
    }

    free(s_queue.items);
    free(s_queue.order);
    free(s_queue.flow_batch);
    s_queue.items = new_items;
    s_queue.order = new_order;
    s_queue.flow_batch = new_batch;
    s_queue.count = new_count;
    s_queue.order_count = new_count;
    logLine("pq: flow append +%d, count=%d\n", tail, new_count);
    return 0;
}
