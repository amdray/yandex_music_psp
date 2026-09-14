#include "services/playback_queue.h"

#include <stdlib.h>
#include <string.h>

#include "core/logger.h"
#include "services/track_meta_store.h"

typedef struct {
    ListIndexId *ids;   /* private copy of the source order */
    int *order;         /* playback order: indices into ids */
    int *flow_album;    /* per-position album_id (0 = unknown / non-FLOW) */
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
    if (s_queue.ids) {
        free(s_queue.ids);
        s_queue.ids = NULL;
    }
    if (s_queue.order) {
        free(s_queue.order);
        s_queue.order = NULL;
    }
    /* WAVE-REPORT: FLOW metadata travels with the positions. */
    if (s_queue.flow_album) {
        free(s_queue.flow_album);
        s_queue.flow_album = NULL;
    }
    if (s_queue.flow_batch) {
        free(s_queue.flow_batch);
        s_queue.flow_batch = NULL;
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
    int *old_flow_album;
    char *old_flow_batch;
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
    /* WAVE-REPORT: FLOW metadata arrays ride along; zeroed = non-FLOW. */
    next.flow_album = (int *)calloc((size_t)count, sizeof(int));
    next.flow_batch = (char *)calloc((size_t)count, PLAYBACK_FLOW_BATCH_SIZE);
    if (!next.ids || !next.order || !next.flow_album || !next.flow_batch) {
        free(next.ids);
        free(next.order);
        free(next.flow_album);
        free(next.flow_batch);
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
    old_flow_album = s_queue.flow_album;
    old_flow_batch = s_queue.flow_batch;
    s_queue = next;
    free(old_ids);
    free(old_order);
    free(old_flow_album);
    free(old_flow_batch);

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
    /* WAVE-REPORT: grow FLOW metadata via staging (malloc+memcpy, commit
     * only when both succeed) so a failure leaves the queue untouched.
     * The legacy ids/order path below is unchanged. */
    {
        int *staged_album = (int *)malloc((size_t)new_count * sizeof(int));
        char *staged_batch = (char *)malloc((size_t)new_count *
                                            PLAYBACK_FLOW_BATCH_SIZE);
        if (!staged_album || !staged_batch) {
            free(staged_album);
            free(staged_batch);
            return -1;
        }
        if (s_queue.count > 0) {
            if (s_queue.flow_album) {
                memcpy(staged_album, s_queue.flow_album,
                       (size_t)s_queue.count * sizeof(int));
            } else {
                memset(staged_album, 0, (size_t)s_queue.count * sizeof(int));
            }
            if (s_queue.flow_batch) {
                memcpy(staged_batch, s_queue.flow_batch,
                       (size_t)s_queue.count * PLAYBACK_FLOW_BATCH_SIZE);
            } else {
                memset(staged_batch, 0,
                       (size_t)s_queue.count * PLAYBACK_FLOW_BATCH_SIZE);
            }
        }
        memset(staged_album + s_queue.count, 0, (size_t)count * sizeof(int));
        memset(staged_batch + (size_t)s_queue.count * PLAYBACK_FLOW_BATCH_SIZE,
               0, (size_t)count * PLAYBACK_FLOW_BATCH_SIZE);
        free(s_queue.flow_album);
        free(s_queue.flow_batch);
        s_queue.flow_album = staged_album;
        s_queue.flow_batch = staged_batch;
    }
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

/* WAVE-REPORT: FLOW metadata accessors and atomic append (spec sections 7/9).
 * The staging commit below never mutates the live queue until all four
 * arrays are ready; any allocation failure returns -1 with the old queue,
 * current index and old batch ids intact. */

static int queue_contains_flow(const char *track_id, int album_id)
{
    int i;

    if (!track_id || !track_id[0] || !s_queue.ids) {
        return 0;
    }
    for (i = 0; i < s_queue.count; ++i) {
        int stored_album = s_queue.flow_album ? s_queue.flow_album[i] : 0;
        if (strcmp(s_queue.ids[i], track_id) == 0 && stored_album == album_id) {
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
    memcpy(out->track_id, s_queue.ids[index], sizeof(ListIndexId));
    if (s_queue.flow_album) {
        out->album_id = s_queue.flow_album[index];
    }
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
    if (!s_queue.flow_album || !s_queue.flow_batch) {
        return -1;
    }
    s_queue.flow_album[index] = album_id;
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

    if (!out || max_count <= 0 || !s_queue.ids || s_queue.count <= 0) {
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
    ListIndexId *new_ids;
    int *new_order;
    int *new_album;
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

    /* Staging allocation: nothing live is touched until all four arrays
     * are ready, then one commit replaces the storage. */
    new_count = s_queue.count + added;
    new_ids = (ListIndexId *)malloc((size_t)new_count * sizeof(ListIndexId));
    new_order = (int *)malloc((size_t)new_count * sizeof(int));
    new_album = (int *)malloc((size_t)new_count * sizeof(int));
    new_batch = (char *)malloc((size_t)new_count * PLAYBACK_FLOW_BATCH_SIZE);
    if (!new_ids || !new_order || !new_album || !new_batch) {
        free(new_ids);
        free(new_order);
        free(new_album);
        free(new_batch);
        logLine("pq: flow append failed, no memory for %d items\n", added);
        return -1;
    }

    if (s_queue.count > 0) {
        if (s_queue.ids) {
            memcpy(new_ids, s_queue.ids, (size_t)s_queue.count * sizeof(ListIndexId));
        }
        if (s_queue.order) {
            memcpy(new_order, s_queue.order, (size_t)s_queue.order_count * sizeof(int));
        }
        if (s_queue.flow_album) {
            memcpy(new_album, s_queue.flow_album, (size_t)s_queue.count * sizeof(int));
        } else {
            memset(new_album, 0, (size_t)s_queue.count * sizeof(int));
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
        memcpy(new_ids[dst], items[src].track_id, sizeof(ListIndexId));
        new_order[s_queue.order_count + tail] = dst;
        new_album[dst] = items[src].album_id;
        strncpy(new_batch + (size_t)dst * PLAYBACK_FLOW_BATCH_SIZE,
                items[src].batch_id, PLAYBACK_FLOW_BATCH_SIZE - 1);
        new_batch[(size_t)dst * PLAYBACK_FLOW_BATCH_SIZE + PLAYBACK_FLOW_BATCH_SIZE - 1] = '\0';
        tail++;
    }

    free(s_queue.ids);
    free(s_queue.order);
    free(s_queue.flow_album);
    free(s_queue.flow_batch);
    s_queue.ids = new_ids;
    s_queue.order = new_order;
    s_queue.flow_album = new_album;
    s_queue.flow_batch = new_batch;
    s_queue.count = new_count;
    s_queue.order_count = new_count;
    logLine("pq: flow append +%d, count=%d\n", tail, new_count);
    return 0;
}
