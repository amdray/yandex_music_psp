#ifndef YM_PLAYBACK_QUEUE_H
#define YM_PLAYBACK_QUEUE_H

#include <stddef.h>
#include "app/track.h"


typedef enum {
    PLAYBACK_QUEUE_SOURCE_NONE = 0,
    PLAYBACK_QUEUE_SOURCE_ALBUM,
    PLAYBACK_QUEUE_SOURCE_PLAYLIST,
    PLAYBACK_QUEUE_SOURCE_FLOW,
    PLAYBACK_QUEUE_SOURCE_ARTIST
} PlaybackQueueSource;

typedef enum {
    PLAYBACK_ORDER_SEQUENTIAL = 0,
    PLAYBACK_ORDER_SHUFFLE
} PlaybackOrderMode;

typedef struct {
    unsigned int revision;
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

typedef struct PlaybackQueueDraft PlaybackQueueDraft;
/* selected_index == -1 requests a random starting position and shuffle. */
PlaybackQueueDraft *playback_queue_prepare(
    const TrackRef *refs,
    int count,
    int selected_index,
    PlaybackQueueSource source,
    int source_id,
    int generation);

void playback_queue_discard(PlaybackQueueDraft *draft);
void playback_queue_commit(PlaybackQueueDraft *draft);
const TrackRef *playback_queue_draft_current(const PlaybackQueueDraft *draft);
int playback_queue_draft_next(PlaybackQueueDraft *draft);
int playback_queue_set_order_mode(PlaybackOrderMode mode);

int playback_queue_get_current(TrackEntry *out);
/* Find the nearest available or not-yet-loaded position without moving the cursor. */
int playback_queue_get_adjacent(int direction, TrackEntry *out, int *order_pos);
/* Commit the selected position only after playback accepts it. */
int playback_queue_select_order_pos(int order_pos);

int playback_queue_get_info(PlaybackQueueInfo *out);

/* Copy queue identities including per-position album context. */
int playback_queue_get_refs(TrackRef *out, int max_count);
/* Last max_count identities up to and including the current queue position. */
int playback_queue_get_history_refs(TrackRef *out, int max_count);

/* FLOW per-item metadata (spec section 7/9). album_id/batch_id travel with
 * the queue position: refill appends must never rewrite the batch of items
 * that are already queued. Non-FLOW positions report album_id 0 and an
 * empty batch_id. */
#define PLAYBACK_FLOW_BATCH_SIZE 64
#define PLAYBACK_FLOW_SESSION_SIZE 64

/* Queue-owned session identity; reading it does not activate a server session.
 * Both accessors return 0 on success, -1 for invalid arguments or non-FLOW. */
int playback_queue_set_flow_session_id(const char *session_id);
int playback_queue_get_flow_session_id(char *out, size_t out_size);

typedef struct {
    char track_id[TRACK_ID_SIZE];
    int album_id;
    char batch_id[PLAYBACK_FLOW_BATCH_SIZE];
} WaveQueueItem;

/* Атомарный append FLOW-элементов через staging allocation: при allocation
 * failure текущая очередь остаётся неизменной, current index не двигается,
 * batch ID старых элементов сохраняются, дубликаты (ключ track-id:album-id)
 * отбрасываются, хвост order строится последовательно. Возврат 0 = ок
 * (0 также когда все элементы оказались дубликатами — делать нечего). */
int playback_queue_append_flow_items(const WaveQueueItem *items, int count);

/* Выставить FLOW-метаданные позиции (начальная пачка волны после
 * playback_queue_commit). Возврат 0 = ок. */
int playback_queue_set_flow_meta(int index, int album_id, const char *batch_id);

/* FLOW-метаданные текущего/следующего/предыдущего элемента в порядке
 * воспроизведения. Метаданные известны даже при PENDING (трек ещё не
 * гидрирован), поэтому возврат 0 = позиция валидна, -1 = границы очереди.
 * *out всегда заполняется целиком при возврате 0. */
int playback_queue_get_current_flow(WaveQueueItem *out);
int playback_queue_get_flow_at(int order_pos, WaveQueueItem *out);
int playback_queue_get_next_flow(WaveQueueItem *out);
int playback_queue_get_previous_flow(WaveQueueItem *out);

/* Скопировать FLOW-элементы в порядке сессии (id order), до max_count штук.
 * Возврат: число записанных, -1 при ошибке. Нужно refill-воркеру для
 * составных "track-id:album-id" идентификаторов очереди. */
int playback_queue_get_flow_items(WaveQueueItem *out, int max_count);

int playback_queue_get_window(TrackRef *refs, int capacity);
int playback_queue_export_order(int *order, int count);
int playback_queue_restore(const TrackRef *refs, const int *order, int count,
                           int pos, PlaybackOrderMode mode,
                           PlaybackQueueSource source, int source_id, int generation);
#endif
