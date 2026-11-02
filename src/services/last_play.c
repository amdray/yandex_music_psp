// Запоминание последнего воспроизведения. См. last_play.h.
#include "services/last_play.h"

#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pspthreadman.h>

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
#define LAST_PLAY_VER 5
#define LAST_PLAY_BASE_HEADER_SIZE 40
#define LAST_PLAY_HEADER_SIZE (LAST_PLAY_BASE_HEADER_SIZE + PLAYBACK_FLOW_SESSION_SIZE)


static int s_autostart_loaded;
static int s_autostart_enabled;

enum {
    READ_ERROR = -1,
    READ_COMPLETE = 0,
    READ_SHORT = 1
};

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
    (void)fd; (void)written;
    fs_write_deferred(PLAYER_SETTINGS_PATH, buffer, (size_t)length);  /* в фоне */
    s_autostart_enabled = enabled;
    logLine("lastplay: autostart=%d queued\n", s_autostart_enabled);
}

static int read_all(SceUID fd, void *buf, int len)
{
    char *p = (char *)buf;
    int done = 0;
    while (done < len) {
        int r = fs_read(fd, p + done, (size_t)(len - done));
        if (r < 0) return READ_ERROR;
        if (r == 0) return READ_SHORT;
        done += r;
    }
    return READ_COMPLETE;
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
    return (int)(((unsigned int)(unsigned char)src[0]) |
           (((unsigned int)(unsigned char)src[1]) << 8) |
           (((unsigned int)(unsigned char)src[2]) << 16) |
           (((unsigned int)(unsigned char)src[3]) << 24));
}

static SceLwMutexWorkarea s_save_mutex;
static SceUID s_save_thread = -1, s_save_sema = -1;
static unsigned char *s_pending_save;
static size_t s_pending_size;
static int s_save_stopping, s_save_error;

static int save_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        if (sceKernelWaitSema(s_save_sema, 1, NULL) < 0) return -1;
        for (;;) {
            sceKernelLockLwMutex(&s_save_mutex, 1, NULL);
            unsigned char *data = s_pending_save;
            size_t size = s_pending_size;
            int stopping = s_save_stopping;
            s_pending_save = NULL;
            s_pending_size = 0;
            sceKernelUnlockLwMutex(&s_save_mutex, 1);
            if (!data) {
                if (stopping) return 0;
                break;
            }
            s_save_error = fs_write_atomic(LAST_PLAY_PATH, data, size) < 0;
            logLine("lastplay: save bytes=%u result=%d\n", (unsigned int)size, s_save_error ? -1 : 0);
            free(data);
        }
    }
}
static int save_init(void)
{
    if (s_save_thread >= 0) return s_save_stopping ? -1 : 0;
    if (sceKernelCreateLwMutex(&s_save_mutex, "lastplay", 0, 0, NULL) < 0) return -1;
    s_save_sema = sceKernelCreateSema("lastplay", 0, 0, 1, NULL);
    if (s_save_sema < 0) goto failed;
    s_save_thread = sceKernelCreateThread("lastplay", save_worker, 0x32, 0x4000, 0, NULL);
    if (s_save_thread < 0) goto failed;
    if (sceKernelStartThread(s_save_thread, 0, NULL) < 0) {
        sceKernelDeleteThread(s_save_thread); s_save_thread = -1; goto failed;
    }
    return 0;
failed:
    if (s_save_sema >= 0) sceKernelDeleteSema(s_save_sema);
    s_save_sema = -1;
    sceKernelDeleteLwMutex(&s_save_mutex);
    return -1;
}
int last_play_quiesce(void)
{
    if (s_save_thread < 0) return 0;
    sceKernelLockLwMutex(&s_save_mutex, 1, NULL);
    s_save_stopping = 1;
    sceKernelUnlockLwMutex(&s_save_mutex, 1);
    sceKernelSignalSema(s_save_sema, 1);
    SceUInt timeout = 20000000U;
    if (sceKernelWaitThreadEnd(s_save_thread, &timeout) < 0) return -1;
    return s_save_error ? -1 : 0;
}
void last_play_save(void)
{
    PlaybackQueueInfo info;
    TrackEntry current;
    AudioPlayerSnapshot audio;
    int position_ms = 0;
    if (playback_queue_get_info(&info) < 0 || info.count <= 0) return;
    size_t record_size = sizeof(TrackRef) + sizeof(int);
    if ((size_t)info.count > (SIZE_MAX - LAST_PLAY_HEADER_SIZE) / record_size) return;
    size_t size = LAST_PLAY_HEADER_SIZE + (size_t)info.count * record_size;
    unsigned char *data = calloc(1, size);
    if (!data) { logLine("lastplay: snapshot allocation failed\n"); return; }
    TrackRef *refs = (TrackRef *)(data + LAST_PLAY_HEADER_SIZE);
    int *order = (int *)(refs + info.count);
    if (playback_queue_get_refs(refs, info.count) != info.count ||
        playback_queue_export_order(order, info.count) < 0) { free(data); return; }
    char *head = (char *)data;
    if (info.source == PLAYBACK_QUEUE_SOURCE_FLOW &&
        playback_queue_get_flow_session_id(head + LAST_PLAY_BASE_HEADER_SIZE, PLAYBACK_FLOW_SESSION_SIZE) < 0) {
        free(data); return;
    }
    memset(&current, 0, sizeof(current));
    memset(&audio, 0, sizeof(audio));
    playback_queue_get_current(&current);
    if (audio_player_get_snapshot(&audio) && current.id[0] && strcmp(current.id, audio.track_id) == 0) {
        position_ms = audio.position_ms;
        if (position_ms < 0) position_ms = 0;
        if (audio.duration_ms > 0 && position_ms > audio.duration_ms) position_ms = audio.duration_ms;
    } else if (current.id[0]) playback_controller_get_restored_pause(current.id, &position_ms);
    memcpy(head, "YMLP", 4);
    head[4] = LAST_PLAY_VER;
    write_i32(head + 8, info.source);
    write_i32(head + 12, info.source_id);
    write_i32(head + 16, info.source_generation);
    write_i32(head + 20, info.order_pos);
    write_i32(head + 24, info.count);
    write_i32(head + 28, position_ms);
    write_i32(head + 32, info.order_mode);
    write_i32(head + 36, sizeof(TrackRef));
    if (save_init() < 0) { free(data); logLine("lastplay: worker unavailable\n"); return; }
    sceKernelLockLwMutex(&s_save_mutex, 1, NULL);
    unsigned char *old = s_pending_save;
    s_pending_save = data;
    s_pending_size = size;
    sceKernelUnlockLwMutex(&s_save_mutex, 1);
    free(old);
    sceKernelSignalSema(s_save_sema, 1);
}
int last_play_restore(AppState *state, int autoplay)
{
    char head[LAST_PLAY_HEADER_SIZE];
    SceIoStat st;
    PlaybackQueueInfo current;
    if (!state || fs_getstat(LAST_PLAY_PATH, &st) < 0) return -1;
    SceUID fd = fs_open(LAST_PLAY_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    if (read_all(fd, head, sizeof(head)) != READ_COMPLETE ||
        memcmp(head, "YMLP", 4) != 0 || head[4] != LAST_PLAY_VER ||
        read_i32(head + 36) != sizeof(TrackRef)) { fs_close(fd); return -1; }
    int source = read_i32(head + 8), count = read_i32(head + 24);
    int pos = read_i32(head + 20), position_ms = read_i32(head + 28);
    int mode = read_i32(head + 32);
    size_t record_size = sizeof(TrackRef) + sizeof(int);
    if (count <= 0 || pos < 0 || pos >= count || position_ms < 0 ||
        source < PLAYBACK_QUEUE_SOURCE_NONE || source > PLAYBACK_QUEUE_SOURCE_ARTIST ||
        (size_t)count > (SIZE_MAX - sizeof(head)) / record_size ||
        (SceOff)st.st_size != (SceOff)sizeof(head) + (SceOff)count * (SceOff)record_size ||
        !memchr(head + LAST_PLAY_BASE_HEADER_SIZE, '\0', PLAYBACK_FLOW_SESSION_SIZE) ||
        ((source == PLAYBACK_QUEUE_SOURCE_FLOW) != (head[LAST_PLAY_BASE_HEADER_SIZE] != '\0'))) {
        fs_close(fd); return -1;
    }
    size_t bytes = (size_t)count * record_size;
    if (bytes > INT32_MAX) { fs_close(fd); return -1; }
    TrackRef *refs = malloc(bytes);
    if (!refs) { fs_close(fd); return -1; }
    int *order = (int *)(refs + count);
    int rc = read_all(fd, refs, (int)bytes);
    if (fs_close(fd) < 0) rc = READ_ERROR;
    for (int i = 0; rc == READ_COMPLETE && i < count; ++i)
        if (!refs[i].id[0] || !memchr(refs[i].id, '\0', sizeof(refs[i].id)) || refs[i].album_id < 0)
            rc = READ_ERROR;
    playback_queue_get_info(&current);
    if (rc != READ_COMPLETE || current.count > 0 ||
        playback_queue_restore(refs, order, count, pos, mode, source,
                               read_i32(head + 12), read_i32(head + 16)) < 0) {
        free(refs); return -1;
    }
    if (source == PLAYBACK_QUEUE_SOURCE_FLOW)
        playback_queue_set_flow_session_id(head + LAST_PLAY_BASE_HEADER_SIZE);
    memset(&state->now_playing_track, 0, sizeof(state->now_playing_track));
    snprintf(state->now_playing_track.id, sizeof(state->now_playing_track.id), "%s", refs[order[pos]].id);
    state->now_playing_track.album_id = refs[order[pos]].album_id;
    free(refs);
    if (autoplay && source != PLAYBACK_QUEUE_SOURCE_FLOW) playback_controller_request_resume_current(position_ms);
    else playback_controller_restore_paused_current(position_ms);
    logLine("lastplay: restored count=%d order_pos=%d shuffle=%d\n", count, pos, mode);
    return 0;
}