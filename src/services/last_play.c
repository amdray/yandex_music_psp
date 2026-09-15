// Запоминание последнего воспроизведения. См. last_play.h.
// v2: + FLOW-метаданные (album_id/batch_id) и radio_session_id,
// чтобы волна переживала ребут с живой сессией. v1 читается.
#include "services/last_play.h"

#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/playback_controller.h"
#include "services/playback_queue.h"
#include "services/token_loader.h"
#include "services/wave.h"

#define LAST_PLAY_PATH "config/lastplay.dat"
#define LAST_PLAY_MAGIC0 'Y'
#define LAST_PLAY_MAGIC1 'M'
#define LAST_PLAY_MAGIC2 'L'
#define LAST_PLAY_MAGIC3 'P'
#define LAST_PLAY_VER 2
#define LAST_PLAY_MAX_IDS 512
#define LAST_PLAY_HEAD_V2 32
#define LAST_PLAY_SESSID 64
#define LAST_PLAY_BATCH 64

static int write_all(SceUID fd, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int done = 0;
    while (done < len) {
        int w = fs_write(fd, p + done, (size_t)(len - done));
        if (w <= 0) {
            return -1;
        }
        done += w;
    }
    return 0;
}

static int read_all(SceUID fd, void *buf, int len)
{
    char *p = (char *)buf;
    int done = 0;
    while (done < len) {
        int r = fs_read(fd, p + done, (size_t)(len - done));
        if (r <= 0) {
            return -1;
        }
        done += r;
    }
    return 0;
}

static void write_i32(char *dst, int v)
{
    dst[0] = (char)(v & 0xFF);
    dst[1] = (char)((v >> 8) & 0xFF);
    dst[2] = (char)((v >> 16) & 0xFF);
    dst[3] = (char)((v >> 24) & 0xFF);
}

static int read_i32(const char *src)
{
    return ((unsigned char)src[0]) |
           (((unsigned char)src[1]) << 8) |
           (((unsigned char)src[2]) << 16) |
           (((unsigned char)src[3]) << 24);
}

void last_play_save(void)
{
    PlaybackQueueInfo info;
    ListIndexId ids[LAST_PLAY_MAX_IDS];
    SceUID fd;
    char head[LAST_PLAY_HEAD_V2];
    char sess_id[LAST_PLAY_SESSID];
    WaveQueueItem *flow = NULL;
    int flow_n = 0, has_flow = 0;
    int n, i, k;

    if (playback_queue_get_info(&info) != 0) {
        return;
    }
    if (info.count <= 0 || info.count > LAST_PLAY_MAX_IDS ||
        info.current_index < 0 || info.current_index >= info.count) {
        return;
    }
    n = playback_queue_get_ids(ids, LAST_PLAY_MAX_IDS);
    if (n != info.count) {
        return;
    }
    // FLOW-метаданные — только для волны с живой сессией.
    if (info.source == PLAYBACK_QUEUE_SOURCE_FLOW) {
        WaveSession ws;
        if (wave_get_session(&ws) == 0 && ws.radio_session_id[0]) {
            flow = (WaveQueueItem *)malloc(sizeof(WaveQueueItem) *
                                           (size_t)info.count);
            if (flow) {
                flow_n = playback_queue_get_flow_items(flow, info.count);
                if (flow_n > 0) {
                    has_flow = 1;
                    snprintf(sess_id, sizeof(sess_id), "%s",
                             ws.radio_session_id);
                } else {
                    free(flow);
                    flow = NULL;
                }
            }
        }
    }
    if (!has_flow) {
        memset(sess_id, 0, sizeof(sess_id));
    }
    fd = fs_open(LAST_PLAY_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        logLine("lastplay: save open failed 0x%08X\n", fd);
        free(flow);
        return;
    }
    head[0] = LAST_PLAY_MAGIC0;
    head[1] = LAST_PLAY_MAGIC1;
    head[2] = LAST_PLAY_MAGIC2;
    head[3] = LAST_PLAY_MAGIC3;
    head[4] = LAST_PLAY_VER;
    head[5] = 0;
    head[6] = 0;
    head[7] = 0;
    write_i32(head + 8, (int)info.source);
    write_i32(head + 12, info.source_id);
    write_i32(head + 16, info.source_generation);
    write_i32(head + 20, info.current_index);
    write_i32(head + 24, info.count);
    write_i32(head + 28, has_flow);
    if (write_all(fd, head, sizeof(head)) != 0) {
        logLine("lastplay: save header failed\n");
        fs_close(fd);
        free(flow);
        return;
    }
    for (i = 0; i < n; i++) {
        if (write_all(fd, ids[i], sizeof(ListIndexId)) != 0) {
            logLine("lastplay: save id %d failed\n", i);
            fs_close(fd);
            free(flow);
            return;
        }
    }
    if (write_all(fd, sess_id, sizeof(sess_id)) != 0) {
        fs_close(fd);
        free(flow);
        return;
    }
    // Метаданные в порядке очереди, поиск по track_id (порядок не важен).
    for (i = 0; i < n; i++) {
        char album_b[4] = { 0, 0, 0, 0 };
        char batch[LAST_PLAY_BATCH];
        memset(batch, 0, sizeof(batch));
        if (has_flow) {
            for (k = 0; k < flow_n; k++) {
                if (strcmp(flow[k].track_id, ids[i]) == 0) {
                    write_i32(album_b, flow[k].album_id);
                    snprintf(batch, sizeof(batch), "%s", flow[k].batch_id);
                    break;
                }
            }
        }
        if (write_all(fd, album_b, sizeof(album_b)) != 0 ||
            write_all(fd, batch, sizeof(batch)) != 0) {
            fs_close(fd);
            free(flow);
            return;
        }
    }
    fs_close(fd);
    free(flow);
    logLine("lastplay: saved index=%d count=%d source=%d flow=%d\n",
            info.current_index, info.count, (int)info.source, has_flow);
}

static int restore_v1(SceUID fd, AppState *state, const char *head28)
{
    PlaybackQueueInfo cur;
    ListIndexId ids[LAST_PLAY_MAX_IDS];
    int source, source_id, generation, index, count, i;

    source = read_i32(head28 + 8);
    source_id = read_i32(head28 + 12);
    generation = read_i32(head28 + 16);
    index = read_i32(head28 + 20);
    count = read_i32(head28 + 24);
    if (source < 0 || source > PLAYBACK_QUEUE_SOURCE_ARTIST ||
        count <= 0 || count > LAST_PLAY_MAX_IDS ||
        index < 0 || index >= count) {
        fs_close(fd);
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (read_all(fd, ids[i], sizeof(ListIndexId)) != 0) {
            fs_close(fd);
            return -1;
        }
        ids[i][sizeof(ListIndexId) - 1] = '\0';
    }
    fs_close(fd);
    if (playback_queue_get_info(&cur) == 0 && cur.count > 0) {
        return -1;
    }
    if (playback_queue_set_from_ids(ids, count, index,
                                    (PlaybackQueueSource)source,
                                    source_id, generation) != 0) {
        return -1;
    }
    memset(&state->now_playing_track, 0, sizeof(state->now_playing_track));
    snprintf(state->now_playing_track.id, sizeof(state->now_playing_track.id),
             "%s", ids[index]);
    playback_controller_request_play_current();
    logLine("lastplay: restored v1 index=%d count=%d\n", index, count);
    logger_flush();
    return 0;
}

int last_play_restore(AppState *state)
{
    PlaybackQueueInfo cur;
    ListIndexId ids[LAST_PLAY_MAX_IDS];
    SceUID fd;
    char head[LAST_PLAY_HEAD_V2];
    int source, source_id, generation, index, count, has_flow, i;
    char sess_id[LAST_PLAY_SESSID];
    char token[256];

    if (!state) {
        return -1;
    }
    // Без токена гидрация невозможна — молча в меню.
    if (token_loader_read(token, sizeof(token)) != 0) {
        return -1;
    }
    memset(token, 0, sizeof(token));
    fd = fs_open(LAST_PLAY_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        return -1;
    }
    // v1-файл короче: читаем минимум, остальное — по версии.
    if (read_all(fd, head, 28) != 0) {
        fs_close(fd);
        return -1;
    }
    if (head[0] != LAST_PLAY_MAGIC0 || head[1] != LAST_PLAY_MAGIC1 ||
        head[2] != LAST_PLAY_MAGIC2 || head[3] != LAST_PLAY_MAGIC3) {
        fs_close(fd);
        return -1;
    }
    if (head[4] == 1) {
        return restore_v1(fd, state, head);
    }
    if (head[4] != LAST_PLAY_VER) {
        fs_close(fd);
        return -1;
    }
    if (read_all(fd, head + 28, sizeof(head) - 28) != 0) {
        fs_close(fd);
        return -1;
    }
    source = read_i32(head + 8);
    source_id = read_i32(head + 12);
    generation = read_i32(head + 16);
    index = read_i32(head + 20);
    count = read_i32(head + 24);
    has_flow = read_i32(head + 28);
    if (source < 0 || source > PLAYBACK_QUEUE_SOURCE_ARTIST ||
        count <= 0 || count > LAST_PLAY_MAX_IDS ||
        index < 0 || index >= count || (has_flow != 0 && has_flow != 1)) {
        fs_close(fd);
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (read_all(fd, ids[i], sizeof(ListIndexId)) != 0) {
            fs_close(fd);
            return -1;
        }
        ids[i][sizeof(ListIndexId) - 1] = '\0';
    }
    if (read_all(fd, sess_id, sizeof(sess_id)) != 0) {
        fs_close(fd);
        return -1;
    }
    sess_id[sizeof(sess_id) - 1] = '\0';
    // Не поднимаем поверх живой очереди (например, уже играет).
    if (playback_queue_get_info(&cur) == 0 && cur.count > 0) {
        fs_close(fd);
        return -1;
    }
    if (playback_queue_set_from_ids(ids, count, index,
                                    (PlaybackQueueSource)source,
                                    source_id, generation) != 0) {
        fs_close(fd);
        return -1;
    }
    if (has_flow) {
        // Метаданные строго по совпадению id (порядок не важен).
        for (i = 0; i < count; i++) {
            char album_b[4];
            char batch[LAST_PLAY_BATCH];
            int album = 0;
            if (read_all(fd, album_b, sizeof(album_b)) != 0 ||
                read_all(fd, batch, sizeof(batch)) != 0) {
                fs_close(fd);
                return -1;
            }
            batch[sizeof(batch) - 1] = '\0';
            album = read_i32(album_b);
            if (batch[0] &&
                strcmp(ids[i], "") != 0) {
                playback_queue_set_flow_meta(i, album, batch);
            }
        }
        wave_restore_session(sess_id[0] ? sess_id : NULL, 0);
    } else {
        // Пропустить блок метаданных (нули): его может не быть в файле,
        // если писала старая v2 без flow — читаем аккуратно по размеру.
        // (Пишем всегда, так что здесь он есть; защита на всякий случай.)
        {
            char skip[4 + LAST_PLAY_BATCH];
            for (i = 0; i < count; i++) {
                if (read_all(fd, skip, sizeof(skip)) != 0) {
                    break;
                }
            }
        }
    }
    fs_close(fd);
    memset(&state->now_playing_track, 0, sizeof(state->now_playing_track));
    snprintf(state->now_playing_track.id, sizeof(state->now_playing_track.id),
             "%s", ids[index]);
    playback_controller_request_play_current();
    logLine("lastplay: restored v2 index=%d count=%d source=%d flow=%d\n",
            index, count, source, has_flow);
    logger_flush();
    return 0;
}
