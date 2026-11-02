#include "services/playback_queue.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "core/logger.h"
#include "services/track_meta_store.h"
#include "services/systemctrl_rng.h"

struct PlaybackQueueDraft {
    TrackRef *items;    /* private track+album identity per queue position */
    int *order;         /* playback order: indices into items */
    char *flow_batch;   /* per-position batch_id, count * PLAYBACK_FLOW_BATCH_SIZE */
    char flow_session_id[PLAYBACK_FLOW_SESSION_SIZE];
    int count;
    int current_index;
    int order_count;
    int order_pos;
    PlaybackQueueSource source;
    PlaybackOrderMode order_mode;
    int source_generation;
    int source_id;
    int initialized;
};
typedef struct PlaybackQueueDraft PlaybackQueueState;

static PlaybackQueueState s_queue;
static unsigned int s_revision;
static PlaybackOrderMode s_preferred_order;

static unsigned int queue_random_below(unsigned int bound)
{
    unsigned int value;
    unsigned int threshold = (0u - bound) % bound;
    do {
        systemctrl_rng(NULL, (unsigned char *)&value, sizeof(value));
    } while (value < threshold);
    return value % bound;
}
static void queue_reset_order(PlaybackQueueState *queue)
{
    int i;

    queue->order_count = queue->count;
    if (queue->count <= 0) {
        queue->order_pos = 0;
        return;
    }

    if (queue->order_mode == PLAYBACK_ORDER_SHUFFLE) {
        int write_pos = 0;


        queue->order[write_pos++] = queue->current_index;
        for (i = 0; i < queue->count; ++i) {
            if (i == queue->current_index) {
                continue;
            }
            queue->order[write_pos++] = i;
        }

        for (i = queue->count - 1; i > 1; --i) {
            int swap_pos = 1 + (int)queue_random_below((unsigned int)i);
            int tmp = queue->order[i];
            queue->order[i] = queue->order[swap_pos];
            queue->order[swap_pos] = tmp;
        }

        queue->order_pos = 0;
        return;
    }

    for (i = 0; i < queue->count; ++i) {
        queue->order[i] = i;
    }
    queue->order_pos = queue->current_index;
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
    ++s_revision;
    logLine("pq: clear\n");
}

PlaybackQueueDraft *playback_queue_prepare(const TrackRef *refs,
                     int count,
                     int selected_index,
                     PlaybackQueueSource source,
                     int source_id,
                     int generation)
{
    PlaybackQueueState next;
    int i;

    if (!s_queue.initialized) {
        playback_queue_init();
    }
    if (!refs || count <= 0 || source < PLAYBACK_QUEUE_SOURCE_NONE || source > PLAYBACK_QUEUE_SOURCE_ARTIST) {
        return NULL;
    }
    int shuffle_start = selected_index == -1;
    if (shuffle_start) selected_index = (int)queue_random_below((unsigned int)count);
    if (selected_index < 0 || selected_index >= count) {
        return NULL;
    }
    if ((size_t)count > SIZE_MAX / sizeof(TrackRef) ||
        (size_t)count > SIZE_MAX / sizeof(int) ||
        (source == PLAYBACK_QUEUE_SOURCE_FLOW && (size_t)count > SIZE_MAX / PLAYBACK_FLOW_BATCH_SIZE)) {
        logLine("pq: set failed, size overflow for %d ids\n", count);
        return NULL;
    }

    for (i = 0; i < count; ++i) {
        if (!refs[i].id[0] || !memchr(refs[i].id, '\0', sizeof(refs[i].id)) || refs[i].album_id < 0)
            return NULL;
    }
    memset(&next, 0, sizeof(next));
    next.items = (TrackRef *)calloc((size_t)count, sizeof(TrackRef));
    next.order = (int *)malloc((size_t)count * sizeof(int));
    if (source == PLAYBACK_QUEUE_SOURCE_FLOW)
        next.flow_batch = (char *)calloc((size_t)count, PLAYBACK_FLOW_BATCH_SIZE);
    if (!next.items || !next.order || (source == PLAYBACK_QUEUE_SOURCE_FLOW && !next.flow_batch)) {
        free(next.items);
        free(next.order);
        free(next.flow_batch);
        logLine("pq: set failed, no memory for %d ids\n", count);
        return NULL;
    }

    for (i = 0; i < count; ++i) {
        next.items[i] = refs[i];
        next.items[i].id[TRACK_ID_SIZE - 1] = '\0';
        next.order[i] = i;
    }
    next.count = count;
    next.current_index = selected_index;
    next.order_count = count;
    next.order_pos = selected_index;
    next.source = source;
    next.order_mode = source == PLAYBACK_QUEUE_SOURCE_FLOW
        ? PLAYBACK_ORDER_SEQUENTIAL : (shuffle_start ? PLAYBACK_ORDER_SHUFFLE : s_preferred_order);
    next.source_generation = generation;
    next.source_id = source_id;
    next.initialized = 1;

    queue_reset_order(&next);
    PlaybackQueueDraft *draft = malloc(sizeof(*draft));
    if (!draft) { free(next.items); free(next.order); free(next.flow_batch); return NULL; }
    *draft = next;
    return draft;
}

void playback_queue_discard(PlaybackQueueDraft *draft)
{
    if (!draft) return;
    free(draft->items);
    free(draft->order);
    free(draft->flow_batch);
    free(draft);
}

void playback_queue_commit(PlaybackQueueDraft *draft)
{
    if (!draft) return;
    queue_free_storage();
    s_queue = *draft;
    free(draft);
    if (s_queue.source != PLAYBACK_QUEUE_SOURCE_FLOW) s_preferred_order = s_queue.order_mode;
    ++s_revision;
    logLine("pq: commit source=%d count=%d index=%d shuffle=%d revision=%u\n",
        s_queue.source, s_queue.count, s_queue.current_index, s_queue.order_mode, s_revision);
}

const TrackRef *playback_queue_draft_current(const PlaybackQueueDraft *draft)
{
    if (!draft || draft->order_pos >= draft->count) return NULL;
    return &draft->items[draft->order[draft->order_pos]];
}

int playback_queue_draft_next(PlaybackQueueDraft *draft)
{
    if (!draft || draft->order_pos + 1 >= draft->count) return -1;
    ++draft->order_pos;
    draft->current_index = draft->order[draft->order_pos];
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

    if (s_queue.source == PLAYBACK_QUEUE_SOURCE_FLOW) return -1;
    s_preferred_order = mode;
    if (s_queue.order_mode == mode) return 0;
    s_queue.order_mode = mode;
    ++s_revision;
    queue_reset_order(&s_queue);
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

int playback_queue_get_adjacent(int direction, TrackEntry *out, int *order_pos)
{
    if (!out || (direction != 1 && direction != -1)) return PLAYBACK_QUEUE_EMPTY;
    for (int pos = s_queue.order_pos + direction;
         pos >= 0 && pos < s_queue.order_count; pos += direction) {
        int rc = queue_resolve(s_queue.order[pos], out);
        if (rc == PLAYBACK_QUEUE_PENDING || (rc == PLAYBACK_QUEUE_OK && out->available)) {
            if (order_pos) *order_pos = pos;
            return rc;
        }
    }
    return PLAYBACK_QUEUE_EMPTY;
}

int playback_queue_select_order_pos(int order_pos)
{
    if (order_pos < 0 || order_pos >= s_queue.order_count) return -1;
    s_queue.order_pos = order_pos;
    s_queue.current_index = s_queue.order[order_pos];
    return 0;
}

int playback_queue_get_info(PlaybackQueueInfo *out)
{
    if (!out) {
        return -1;
    }

    out->revision = s_revision;
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

static int queue_copy_refs(TrackRef *out, int max_count, int start, int count)
{
    int n;
    int i;

    if (!out || max_count <= 0 || !s_queue.items || s_queue.count <= 0) {
        return -1;
    }
    n = (count < max_count) ? count : max_count;
    for (i = 0; i < n; i++) {
        memcpy(&out[i], &s_queue.items[start + i], sizeof(TrackRef));
    }
    return n;
}

int playback_queue_get_refs(TrackRef *out, int max_count)
{
    return queue_copy_refs(out, max_count, 0, s_queue.count);
}

int playback_queue_get_history_refs(TrackRef *out, int max_count)
{
    if (max_count <= 0 || !queue_validate_index(s_queue.current_index)) return -1;
    int end = s_queue.current_index + 1;
    int count = end < max_count ? end : max_count;
    return queue_copy_refs(out, max_count, end - count, count);
}

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
    memcpy(out->track_id, s_queue.items[index].id, TRACK_ID_SIZE);
    out->album_id = s_queue.items[index].album_id;
    if (s_queue.flow_batch) {
        batch_src = s_queue.flow_batch + (size_t)index * PLAYBACK_FLOW_BATCH_SIZE;
        strncpy(out->batch_id, batch_src, sizeof(out->batch_id) - 1);
        out->batch_id[sizeof(out->batch_id) - 1] = '\0';
    }
    return PLAYBACK_QUEUE_OK;
}

int playback_queue_set_flow_session_id(const char *session_id)
{
    size_t length;
    if (!session_id || s_queue.count <= 0 ||
        s_queue.source != PLAYBACK_QUEUE_SOURCE_FLOW) return -1;
    length = strlen(session_id);
    if (length >= sizeof(s_queue.flow_session_id)) return -1;
    memset(s_queue.flow_session_id, 0, sizeof(s_queue.flow_session_id));
    memcpy(s_queue.flow_session_id, session_id, length);
    return 0;
}

int playback_queue_get_flow_session_id(char *out, size_t out_size)
{
    size_t length;
    if (!out || s_queue.count <= 0 ||
        s_queue.source != PLAYBACK_QUEUE_SOURCE_FLOW) return -1;
    length = strlen(s_queue.flow_session_id);
    if (length >= out_size) return -1;
    memcpy(out, s_queue.flow_session_id, length + 1);
    return 0;
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

int playback_queue_get_flow_at(int order_pos, WaveQueueItem *out)
{
    if (order_pos < 0 || order_pos >= s_queue.order_count) return -1;
    return queue_resolve_flow(s_queue.order[order_pos], out);
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
        memcpy(new_items[dst].id, items[src].track_id, TRACK_ID_SIZE);
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
    ++s_revision;
    logLine("pq: flow append +%d, count=%d\n", tail, new_count);
    return 0;
}

int playback_queue_get_window(TrackRef *refs, int capacity)
{
    int count = 0;
    if (!refs || capacity <= 0) return 0;
    for (int delta = 0; count < capacity && delta < s_queue.count; ++delta) {
        int pos = s_queue.order_pos + delta;
        if (pos < s_queue.order_count) refs[count++] = s_queue.items[s_queue.order[pos]];
        pos = s_queue.order_pos - delta - 1;
        if (pos >= 0 && count < capacity) refs[count++] = s_queue.items[s_queue.order[pos]];
    }
    return count;
}

int playback_queue_export_order(int *order, int count)
{
    if (!order || count != s_queue.count) return -1;
    memcpy(order, s_queue.order, (size_t)count * sizeof(*order));
    return 0;
}

int playback_queue_restore(const TrackRef *refs, const int *order, int count,
                           int pos, PlaybackOrderMode mode,
                           PlaybackQueueSource source, int source_id, int generation)
{
    unsigned char *seen;
    if (!refs || !order || count <= 0 || pos < 0 || pos >= count ||
        (mode != PLAYBACK_ORDER_SEQUENTIAL && mode != PLAYBACK_ORDER_SHUFFLE) ||
        (source == PLAYBACK_QUEUE_SOURCE_FLOW && mode != PLAYBACK_ORDER_SEQUENTIAL)) return -1;
    seen = calloc((size_t)count, 1);
    if (!seen) return -1;
    for (int i = 0; i < count; ++i) {
        if (order[i] < 0 || order[i] >= count || seen[order[i]] ||
            (mode == PLAYBACK_ORDER_SEQUENTIAL && order[i] != i)) {
            free(seen);
            return -1;
        }
        seen[order[i]] = 1;
    }
    free(seen);
    PlaybackQueueDraft *draft = playback_queue_prepare(refs, count, order[pos], source, source_id, generation);
    if (!draft) return -1;
    memcpy(draft->order, order, (size_t)count * sizeof(*order));
    draft->order_pos = pos;
    draft->order_mode = mode;
    playback_queue_commit(draft);
    return 0;
}
