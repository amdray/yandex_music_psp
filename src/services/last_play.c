// Запоминание последнего воспроизведения. См. last_play.h.
#include "services/last_play.h"

#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/audio_player.h"
#include "services/playback_controller.h"
#include "services/playback_queue.h"

#define LAST_PLAY_PATH "config/lastplay.dat"
#define PLAYER_SETTINGS_PATH "config/player.cfg"
#define LAST_PLAY_MAGIC0 'Y'
#define LAST_PLAY_MAGIC1 'M'
#define LAST_PLAY_MAGIC2 'L'
#define LAST_PLAY_MAGIC3 'P'
#define LAST_PLAY_VER 3
#define LAST_PLAY_PREV_VER 2
#define LAST_PLAY_MAX_IDS 512

static int s_autostart_loaded;
static int s_autostart_enabled;

static void load_autostart_setting(void)
{
    char buffer[64];
    SceUID fd;
    int count;
    int parsed;
    char trailing;

    if (s_autostart_loaded) return;
    s_autostart_loaded = 1;
    s_autostart_enabled = 0;
    fd = fs_open(PLAYER_SETTINGS_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        logLine("lastplay: autostart=0 source=default\n");
        return;
    }
    count = fs_read(fd, buffer, sizeof(buffer) - 1);
    fs_close(fd);
    if (count <= 0) {
        logLine("lastplay: autostart=0 source=empty_config\n");
        return;
    }
    buffer[count] = '\0';
    parsed = -1;
    if (sscanf(buffer, "autoplay=%d %c", &parsed, &trailing) == 1 &&
        (parsed == 0 || parsed == 1))
        s_autostart_enabled = parsed;
    logLine("lastplay: autostart=%d source=config\n", s_autostart_enabled);
}

int last_play_autostart_enabled(void)
{
    load_autostart_setting();
    return s_autostart_enabled;
}

void last_play_set_autostart(int enabled)
{
    char buffer[24];
    SceUID fd;
    int length;
    int written;

    load_autostart_setting();
    enabled = enabled ? 1 : 0;
    if (s_autostart_enabled == enabled) return;
    length = snprintf(buffer, sizeof(buffer), "autoplay=%d\n", enabled);
    if (length <= 0 || length >= (int)sizeof(buffer)) return;
    fs_ensure_dir(PLAYER_SETTINGS_PATH);
    fd = fs_open(PLAYER_SETTINGS_PATH,
                 PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        logLine("lastplay: autostart save open failed 0x%08X\n", fd);
        return;
    }
    written = fs_write(fd, buffer, (size_t)length);
    fs_close(fd);
    if (written != length) {
        logLine("lastplay: autostart save short %d/%d\n", written, length);
        return;
    }
    s_autostart_enabled = enabled;
    logLine("lastplay: autostart=%d saved\n", s_autostart_enabled);
    logger_flush();
}

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
    TrackEntry current;
    AudioPlayerSnapshot audio;
    TrackRef refs[LAST_PLAY_MAX_IDS];
    SceUID fd;
    char head[32];
    int position_ms = 0;
    int n, i;

    if (playback_queue_get_info(&info) != 0) {
        return;
    }
    if (info.count <= 0 || info.count > LAST_PLAY_MAX_IDS ||
        info.current_index < 0 || info.current_index >= info.count) {
        return;
    }
    n = playback_queue_get_refs(refs, LAST_PLAY_MAX_IDS);
    if (n != info.count) {
        return;
    }
    memset(&current, 0, sizeof(current));
    memset(&audio, 0, sizeof(audio));
    if (playback_queue_get_current(&current) != PLAYBACK_QUEUE_EMPTY &&
        audio_player_get_snapshot(&audio) &&
        current.id[0] && strcmp(current.id, audio.track_id) == 0) {
        position_ms = audio.position_ms;
        if (position_ms < 0) position_ms = 0;
        if (audio.duration_ms > 0 && position_ms > audio.duration_ms)
            position_ms = audio.duration_ms;
    } else if (current.id[0]) {
        (void)playback_controller_get_restored_pause(current.id,
                                                     &position_ms);
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
    write_i32(head + 28, position_ms);
    if (write_all(fd, head, sizeof(head)) != 0) {
        logLine("lastplay: save header failed\n");
        fs_close(fd);
        return;
    }
    for (i = 0; i < n; i++) {
        char record[TRACK_ID_SIZE + 4];
        memset(record, 0, sizeof(record));
        memcpy(record, refs[i].id, TRACK_ID_SIZE);
        write_i32(record + TRACK_ID_SIZE, refs[i].album_id);
        if (write_all(fd, record, sizeof(record)) != 0) {
            logLine("lastplay: save item %d failed\n", i);
            fs_close(fd);
            return;
        }
    }
    fs_close(fd);
    logLine("lastplay: saved index=%d count=%d source=%d track_id='%s' album_id=%d position_ms=%d\n",
            info.current_index, info.count, (int)info.source,
            refs[info.current_index].id, refs[info.current_index].album_id,
            position_ms);
}

int last_play_restore(AppState *state, int autoplay)
{
    PlaybackQueueInfo cur;
    TrackRef refs[LAST_PLAY_MAX_IDS];
    SceUID fd;
    char head[32];
    int source, source_id, generation, index, count, position_ms, i, version;

    if (!state) {
        return -1;
    }
    fd = fs_open(LAST_PLAY_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        return -1;
    }
    if (read_all(fd, head, sizeof(head)) != 0) {
        fs_close(fd);
        return -1;
    }
    if (head[0] != LAST_PLAY_MAGIC0 || head[1] != LAST_PLAY_MAGIC1 ||
        head[2] != LAST_PLAY_MAGIC2 || head[3] != LAST_PLAY_MAGIC3) {
        fs_close(fd);
        return -1;
    }
    version = (unsigned char)head[4];
    if (version != LAST_PLAY_VER && version != LAST_PLAY_PREV_VER) {
        fs_close(fd);
        return -1;
    }
    source = read_i32(head + 8);
    source_id = read_i32(head + 12);
    generation = read_i32(head + 16);
    index = read_i32(head + 20);
    count = read_i32(head + 24);
    position_ms = read_i32(head + 28);
    if (position_ms < 0) position_ms = 0;
    if (source < 0 || source > PLAYBACK_QUEUE_SOURCE_ARTIST ||
        count <= 0 || count > LAST_PLAY_MAX_IDS ||
        index < 0 || index >= count) {
        fs_close(fd);
        return -1;
    }
    for (i = 0; i < count; i++) {
        memset(&refs[i], 0, sizeof(refs[i]));
        if (version == LAST_PLAY_VER) {
            char record[TRACK_ID_SIZE + 4];
            if (read_all(fd, record, sizeof(record)) != 0) {
                fs_close(fd);
                return -1;
            }
            memcpy(refs[i].id, record, TRACK_ID_SIZE);
            refs[i].album_id = read_i32(record + TRACK_ID_SIZE);
        } else {
            if (read_all(fd, refs[i].id, TRACK_ID_SIZE) != 0) {
                fs_close(fd);
                return -1;
            }
            refs[i].album_id = source == PLAYBACK_QUEUE_SOURCE_ALBUM
                ? source_id : 0;
        }
        refs[i].id[TRACK_ID_SIZE - 1] = '\0';
        if (!refs[i].id[0] || refs[i].album_id < 0) {
            fs_close(fd);
            return -1;
        }
    }
    fs_close(fd);

    // Не поднимаем поверх живой очереди (например, уже играет).
    if (playback_queue_get_info(&cur) == 0 && cur.count > 0) {
        return -1;
    }
    if (playback_queue_set_from_refs(refs, count, index,
                                     (PlaybackQueueSource)source,
                                     source_id, generation) != 0) {
        return -1;
    }
    memset(&state->now_playing_track, 0, sizeof(state->now_playing_track));
    snprintf(state->now_playing_track.id, sizeof(state->now_playing_track.id),
             "%s", refs[index].id);
    if (autoplay) {
        playback_controller_request_resume_current(position_ms);
    } else {
        playback_controller_restore_paused_current(position_ms);
    }
    logLine("lastplay: restored index=%d count=%d source=%d track_id='%s' album_id=%d position_ms=%d autoplay=%d\n",
            index, count, source, refs[index].id, refs[index].album_id,
            position_ms, autoplay ? 1 : 0);
    logger_flush();
    return 0;
}
