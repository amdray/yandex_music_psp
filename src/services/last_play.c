// Запоминание последнего воспроизведения. См. last_play.h.
#include "services/last_play.h"

#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/playback_controller.h"
#include "services/playback_queue.h"
#include "services/token_loader.h"

#define LAST_PLAY_PATH "config/lastplay.dat"
#define LAST_PLAY_MAGIC0 'Y'
#define LAST_PLAY_MAGIC1 'M'
#define LAST_PLAY_MAGIC2 'L'
#define LAST_PLAY_MAGIC3 'P'
#define LAST_PLAY_VER 1
#define LAST_PLAY_MAX_IDS 512

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
    char head[28];
    int n, i;

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
    fd = fs_open(LAST_PLAY_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        logLine("lastplay: save open failed 0x%08X\n", fd);
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
    if (write_all(fd, head, sizeof(head)) != 0) {
        logLine("lastplay: save header failed\n");
        fs_close(fd);
        return;
    }
    for (i = 0; i < n; i++) {
        if (write_all(fd, ids[i], sizeof(ListIndexId)) != 0) {
            logLine("lastplay: save id %d failed\n", i);
            fs_close(fd);
            return;
        }
    }
    fs_close(fd);
    logLine("lastplay: saved index=%d count=%d source=%d\n",
            info.current_index, info.count, (int)info.source);
}

int last_play_restore(AppState *state)
{
    PlaybackQueueInfo cur;
    ListIndexId ids[LAST_PLAY_MAX_IDS];
    SceUID fd;
    char head[28];
    int source, source_id, generation, index, count, i;
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
    if (read_all(fd, head, sizeof(head)) != 0) {
        fs_close(fd);
        return -1;
    }
    if (head[0] != LAST_PLAY_MAGIC0 || head[1] != LAST_PLAY_MAGIC1 ||
        head[2] != LAST_PLAY_MAGIC2 || head[3] != LAST_PLAY_MAGIC3 ||
        head[4] != LAST_PLAY_VER) {
        fs_close(fd);
        return -1;
    }
    source = read_i32(head + 8);
    source_id = read_i32(head + 12);
    generation = read_i32(head + 16);
    index = read_i32(head + 20);
    count = read_i32(head + 24);
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

    // Не поднимаем поверх живой очереди (например, уже играет).
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
    logLine("lastplay: restored index=%d count=%d source=%d\n",
            index, count, source);
    logger_flush();
    return 0;
}
