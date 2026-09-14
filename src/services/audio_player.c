#include "services/audio_player.h"

#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspaudio.h>
#include <pspkernel.h>
#include <pspmp3.h>
#include <psputility_modules.h>

#include <string.h>
#include <stdint.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/audio_cache.h"
#include "services/eq.h"
#include "services/audio_stream_buf.h"
#include "services/net_http.h"
#include "services/net_stack.h"
#include "services/playback_controller.h"
#include "services/resource_policy.h"

#define AUDIO_PLAYER_WORKER_STACK_SIZE 0x10000
#define AUDIO_PLAYER_WORKER_PRIORITY   0x10
#define AUDIO_PLAYER_WAIT_US           (20 * 1000)
#define AUDIO_PLAYER_DECODE_EOS        ((int)0x80671402u)
#define AUDIO_PLAYER_ERR_STOP          (-1001)
#define AUDIO_PLAYER_ERR_TRACK_MISMATCH (-1002)
#define AUDIO_PLAYER_ERR_CACHE         (-1003)
#define AUDIO_PLAYER_ERR_SOURCE        (-1004)
#define AUDIO_PLAYER_EOF               (-1005)
#define AUDIO_PLAYER_ERR_TIMEOUT       (-1006)
/* Watchdog: любое ожидание кэша ограничено сверху. 45 с покрывает худший
 * легальный случай (контеншн handshake-семафора до 30 с + connect/handshake
 * + первые 256 КБ на реальном Wi-Fi ~125 КБ/с); вечный OPENING/BUFFERING
 * при умершем кэше переходит в ERROR вместо зависания навсегда. */
#define AUDIO_PLAYER_WAIT_DEADLINE_US  (45ULL * 1000000ULL)
#define AUDIO_PLAYER_MP3_BUF_SIZE      (16 * 1024)
#define AUDIO_PLAYER_PCM_BUF_SIZE      (16 * (1152 / 2))

static const char *audio_player_state_name(AudioPlayerState state)
{
    switch (state) {
        case AUDIO_PLAYER_OPENING:   return "OPENING";
        case AUDIO_PLAYER_PLAYING:   return "PLAYING";
        case AUDIO_PLAYER_PAUSED:    return "PAUSED";
        case AUDIO_PLAYER_BUFFERING: return "BUFFERING";
        case AUDIO_PLAYER_STOPPING:  return "STOPPING";
        case AUDIO_PLAYER_STOPPED:   return "STOPPED";
        case AUDIO_PLAYER_FINISHED:  return "FINISHED";
        case AUDIO_PLAYER_ERROR:     return "ERROR";
        default:                     return "IDLE";
    }
}

static volatile int s_state = AUDIO_PLAYER_IDLE;
static AudioPlayerSnapshot s_status;
static SceLwMutexWorkarea s_status_mutex;
static int s_mutex_initialized = 0;

static SceUID s_worker_thread = -1;
static volatile int s_worker_running = 0;
static volatile int s_stop_requested = 0;
static volatile int s_pause_requested = 0;
static volatile int s_resume_state = AUDIO_PLAYER_PLAYING;
static int s_backend_ready = 0;
static int s_backend_error = 0;
static int s_mp3_modules_loaded = 0;
static int s_mp3_resource_initialized = 0;
static unsigned char s_mp3_buf[AUDIO_PLAYER_MP3_BUF_SIZE] __attribute__((aligned(64)));
static unsigned char s_pcm_buf[AUDIO_PLAYER_PCM_BUF_SIZE] __attribute__((aligned(64)));

static int release_audio_src_channel(void)
{
    int rc;

    do {
        rc = sceAudioSRCChRelease();
        if (rc < 0) {
            sceKernelDelayThread(100);
        }
    } while (rc < 0);

    return rc;
}

static void set_track_snapshot(const TrackEntry *track)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    }

    s_status.position_ms = 0;
    s_status.duration_ms = 0;
    s_status.sample_rate = 0;
    s_status.channels = 0;
    s_status.bitrate_kbps = 0;

    if (track) {
        s_status.duration_ms = track->duration_ms;
        strncpy(s_status.track_id, track->id, sizeof(s_status.track_id) - 1);
        s_status.track_id[sizeof(s_status.track_id) - 1] = '\0';
        strncpy(s_status.title, track->title, sizeof(s_status.title) - 1);
        s_status.title[sizeof(s_status.title) - 1] = '\0';
        strncpy(s_status.artist, track->artist, sizeof(s_status.artist) - 1);
        s_status.artist[sizeof(s_status.artist) - 1] = '\0';
    } else {
        s_status.track_id[0] = '\0';
        s_status.title[0] = '\0';
        s_status.artist[0] = '\0';
    }

    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
    }
}

static void set_bitrate_snapshot(int bitrate_kbps)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    }
    s_status.bitrate_kbps = bitrate_kbps;
    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
    }
}

static void set_format_snapshot(int sample_rate, int channels)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    }
    s_status.sample_rate = sample_rate;
    s_status.channels = channels;
    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
    }
}

static void set_position_snapshot(int position_ms)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    }
    s_status.position_ms = position_ms;
    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
    }
}

static void set_status(AudioPlayerState state, const char *track_id, int error_code)
{
    AudioPlayerState prev_state = s_status.state;

    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    }

    s_status.state = state;
    s_status.error_code = error_code;
    if (track_id) {
        strncpy(s_status.track_id, track_id, sizeof(s_status.track_id) - 1);
        s_status.track_id[sizeof(s_status.track_id) - 1] = '\0';
    } else {
        s_status.track_id[0] = '\0';
    }

    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
    }

    s_state = state;
    logger_set_realtime_mode(state == AUDIO_PLAYER_OPENING ||
                             state == AUDIO_PLAYER_PLAYING ||
                             state == AUDIO_PLAYER_PAUSED ||
                             state == AUDIO_PLAYER_BUFFERING ||
                             state == AUDIO_PLAYER_STOPPING);

    switch (state) {
        case AUDIO_PLAYER_OPENING:
        case AUDIO_PLAYER_PLAYING:
        case AUDIO_PLAYER_PAUSED:
        case AUDIO_PLAYER_BUFFERING:
            resource_policy_set_audio_active(1);
            break;
        case AUDIO_PLAYER_STOPPED:
        case AUDIO_PLAYER_FINISHED:
        case AUDIO_PLAYER_ERROR:
        case AUDIO_PLAYER_IDLE:
            resource_policy_set_audio_active(0);
            break;
        default:
            break;
    }

    if (prev_state != state || error_code != 0) {
        logLine("ap: state -> %s track_id='%s' err=0x%08X\n",
                audio_player_state_name(state),
                track_id ? track_id : "",
                error_code);

        /* Persist the completed decoder transition before the first blocking
         * audio output. During playback logs stay in RAM so Memory Stick
         * latency cannot disturb the real-time audio path. */
        if (prev_state == AUDIO_PLAYER_OPENING &&
            state == AUDIO_PLAYER_PLAYING) {
            logger_flush();
        }
    }
}

static void set_active_status(AudioPlayerState state, const char *track_id)
{
    if (s_pause_requested) {
        s_resume_state = state;
        set_status(AUDIO_PLAYER_PAUSED, track_id, 0);
    } else {
        set_status(state, track_id, 0);
    }
}

static int wait_while_paused(const char *track_id)
{
    if (!s_pause_requested) {
        return 0;
    }

    if (s_state != AUDIO_PLAYER_PAUSED) {
        s_resume_state = s_state;
        set_status(AUDIO_PLAYER_PAUSED, track_id, 0);
    }
    while (s_pause_requested && !s_stop_requested) {
        sceKernelDelayThread(AUDIO_PLAYER_WAIT_US);
    }
    return s_stop_requested ? AUDIO_PLAYER_ERR_STOP : 0;
}

static void reap_worker_thread(void)
{
    if (s_worker_thread >= 0 && !s_worker_running) {
        sceKernelWaitThreadEnd(s_worker_thread, NULL);
        sceKernelDeleteThread(s_worker_thread);
        s_worker_thread = -1;
    }
}

static void log_worker_priority(SceUID tid)
{
    SceKernelThreadRunStatus status;

    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    if (sceKernelReferThreadRunStatus(tid, &status) == 0) {
        logLine("ap: worker priority tid=%d requested=0x%02X current=0x%02X\n",
                tid,
            AUDIO_PLAYER_WORKER_PRIORITY,
                status.currentPriority);
    } else {
        logLine("ap: worker priority tid=%d requested=0x%02X current=?\n",
                tid,
                AUDIO_PLAYER_WORKER_PRIORITY);
    }
}

static int load_mp3_modules(void)
{
    int rc = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    if (rc < 0 && (unsigned int)rc != SCE_ERROR_MODULE_ALREADY_LOADED) {
        return rc;
    }

    rc = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    if (rc < 0 && (unsigned int)rc != SCE_ERROR_MODULE_ALREADY_LOADED) {
        return rc;
    }

    return 0;
}

static int ensure_backend_ready(void)
{
    int rc;

    if (s_backend_ready) {
        return 0;
    }

    rc = load_mp3_modules();
    if (rc < 0) {
        s_backend_error = rc;
        return rc;
    }
    s_mp3_modules_loaded = 1;

    rc = sceMp3InitResource();
    if (rc < 0) {
        s_backend_error = rc;
        return rc;
    }

    s_mp3_resource_initialized = 1;
    s_backend_ready = 1;
    s_backend_error = 0;
    logLine("ap: backend ready\n");
    return 0;
}

static int wait_for_source(const char *track_id, AudioSourceSnapshot *source)
{
    u64 deadline = sceKernelGetSystemTimeWide() + AUDIO_PLAYER_WAIT_DEADLINE_US;

    while (!s_stop_requested) {
        AudioCacheStatus status;

        audio_cache_get_status(&status);
        if (strcmp(status.track_id, track_id) == 0) {
            if (status.state == AUDIO_CACHE_PROGRESSIVE_READY ||
                status.state == AUDIO_CACHE_READY) {
                if (audio_cache_get_source(source) == 0 &&
                    strcmp(source->track_id, track_id) == 0) {
                    logLine("ap: source acquired track_id='%s' cache_state=%d path='%s' bytes=%d/%d complete=%d\n",
                            source->track_id,
                            source->state,
                            source->path,
                            (int)source->downloaded_bytes,
                            (int)source->content_length,
                            source->complete);
                    return 0;
                }
            }

            if (status.state == AUDIO_CACHE_ERROR) {
                return AUDIO_PLAYER_ERR_CACHE;
            }
            if (status.state == AUDIO_CACHE_CANCELLED) {
                /* Наше задание отменили и замену не поставили — ждать нечего. */
                logLine("ap: wait_for_source cancelled track_id='%s'\n", track_id);
                return AUDIO_PLAYER_ERR_CACHE;
            }
        }

        {
            u64 now = sceKernelGetSystemTimeWide();
            if (strcmp(status.track_id, track_id) == 0 &&
                status.state == AUDIO_CACHE_DOWNLOADING &&
                (!net_stack_is_ready() ||
                 net_http_error_is_network(status.error_code))) {
                deadline = now + AUDIO_PLAYER_WAIT_DEADLINE_US;
            } else if (now >= deadline) {
                logLine("ap: wait_for_source TIMEOUT track_id='%s' cache_state=%d cache_track='%s'\n",
                        track_id, status.state, status.track_id);
                return AUDIO_PLAYER_ERR_TIMEOUT;
            }
        }
        sceKernelDelayThread(AUDIO_PLAYER_WAIT_US);
    }

    return AUDIO_PLAYER_ERR_STOP;
}

static int wait_for_more_data(const char *track_id, int64_t required_bytes)
{
    u64 stall_deadline = sceKernelGetSystemTimeWide() + AUDIO_PLAYER_WAIT_DEADLINE_US;
    int64_t last_bytes = -1;

    set_active_status(AUDIO_PLAYER_BUFFERING, track_id);
    logLine("ap: buffering wait track_id='%s' required=%d\n",
            track_id, (int)required_bytes);

    while (!s_stop_requested) {
        AudioCacheStatus status;

        audio_cache_get_status(&status);
        if (strcmp(status.track_id, track_id) != 0) {
            return AUDIO_PLAYER_ERR_TRACK_MISMATCH;
        }
        if (status.state == AUDIO_CACHE_ERROR) {
            return AUDIO_PLAYER_ERR_CACHE;
        }
        if (status.state == AUDIO_CACHE_CANCELLED) {
            logLine("ap: buffering cancelled track_id='%s'\n", track_id);
            return AUDIO_PLAYER_ERR_CACHE;
        }
        {
            u64 now = sceKernelGetSystemTimeWide();
            if (status.downloaded_bytes != last_bytes) {
                /* Прогресс есть — дедлайн отсчитывается от последнего сдвига байтов. */
                last_bytes = status.downloaded_bytes;
                stall_deadline = now + AUDIO_PLAYER_WAIT_DEADLINE_US;
            } else if (!net_stack_is_ready() ||
                       net_http_error_is_network(status.error_code)) {
                /* Network recovery is not a dead cache. Keep the same decoder
                 * and pause the watchdog until the resumable producer moves. */
                stall_deadline = now + AUDIO_PLAYER_WAIT_DEADLINE_US;
            } else if (now >= stall_deadline) {
                logLine("ap: buffering STALL TIMEOUT track_id='%s' bytes=%d\n",
                        track_id, (int)status.downloaded_bytes);
                return AUDIO_PLAYER_ERR_TIMEOUT;
            }
        }
        if (status.downloaded_bytes > required_bytes) {
            logLine("ap: buffering resume track_id='%s' have=%d need_gt=%d complete=%d\n",
                    track_id,
                    (int)status.downloaded_bytes,
                    (int)required_bytes,
                    status.complete);
            return 0;
        }
        if (status.complete) {
            logLine("ap: buffering eof track_id='%s' have=%d required=%d\n",
                    track_id,
                    (int)status.downloaded_bytes,
                    (int)required_bytes);
            return AUDIO_PLAYER_EOF;
        }

        sceKernelDelayThread(AUDIO_PLAYER_WAIT_US);
    }

    return AUDIO_PLAYER_ERR_STOP;
}

static int feed_mp3_stream_ram(int handle, const char *track_id,
                               int64_t *required_bytes,
                               AudioStreamBuf *sbuf)
{
    unsigned char *dst = NULL;
    SceInt32 write_size = 0;
    SceInt32 source_pos = 0;
    AudioCacheStatus status;
    int rc;
    int got;

    rc = sceMp3GetInfoToAddStreamData(handle, &dst, &write_size, &source_pos);
    if (rc < 0) return rc;

    audio_cache_get_status(&status);
    if (strcmp(status.track_id, track_id) != 0) {
        return AUDIO_PLAYER_ERR_TRACK_MISMATCH;
    }
    if (status.state == AUDIO_CACHE_ERROR || asb_is_error(sbuf)) {
        return AUDIO_PLAYER_ERR_CACHE;
    }

    if (source_pos >= asb_write_pos(sbuf)) {
        *required_bytes = asb_write_pos(sbuf);
        return asb_is_complete(sbuf) ? AUDIO_PLAYER_EOF : 0;
    }

    got = asb_read_at(sbuf, (int64_t)source_pos, dst, (int)write_size);
    if (got < 0) {
        logLine("ap: feed_ram underrun source_pos=%d track_id='%s'\n",
                (int)source_pos, track_id);
        return AUDIO_PLAYER_ERR_SOURCE;
    }
    if (got == 0) {
        *required_bytes = asb_write_pos(sbuf);
        return 0;
    }

    sceKernelDcacheWritebackRange(dst, got);

    rc = sceMp3NotifyAddStreamData(handle, got);
    if (rc < 0) return rc;

    *required_bytes = (int64_t)source_pos + got;
    return got;
}

static int feed_mp3_stream(int *fd_ptr, int handle, const char *track_id,
                           int64_t *required_bytes)
{
    unsigned char *dst = NULL;
    SceInt32 write_size = 0;
    SceInt32 source_pos = 0;
    AudioCacheStatus status;
    int rc;
    int to_read;
    int read_now;

    rc = sceMp3GetInfoToAddStreamData(handle, &dst, &write_size, &source_pos);
    if (rc < 0) {
        return rc;
    }

    audio_cache_get_status(&status);
    if (strcmp(status.track_id, track_id) != 0) {
        return AUDIO_PLAYER_ERR_TRACK_MISMATCH;
    }
    if (status.state == AUDIO_CACHE_ERROR) {
        return AUDIO_PLAYER_ERR_CACHE;
    }

    if (source_pos >= status.content_length && status.content_length > 0) {
        *required_bytes = status.content_length;
        return AUDIO_PLAYER_EOF;
    }

    to_read = write_size;
    if (status.content_length > 0 &&
        source_pos + to_read > status.content_length) {
        to_read = (int)(status.content_length - source_pos);
    }

    rc = (int)fs_lseek(*fd_ptr, source_pos, PSP_SEEK_SET);
    if (rc < 0) {
        return rc;
    }

    read_now = fs_read(*fd_ptr, dst, to_read);
    if (read_now <= 0) {
        return read_now < 0 ? read_now : AUDIO_PLAYER_EOF;
    }

    sceKernelDcacheWritebackRange(dst, read_now);

    rc = sceMp3NotifyAddStreamData(handle, read_now);
    if (rc < 0) {
        return rc;
    }

    *required_bytes = source_pos + read_now;
    return read_now;
}

static int audio_player_worker(SceSize args, void *argp)
{
    TrackEntry track;
    AudioSourceSnapshot source;
    int fd = -1;
    int handle = -1;
    int audio_reserved = 0;
    int reserved_bytes = 0;
    int sampling_rate = 0;
    int num_channels = 0;
    int error_code = 0;
    int source_acquired = 0;
    int stop_state = 0;
    int stream_end = 0;
    int64_t played_samples = 0;
    int loop_num = 0;
    AudioStreamBuf *sbuf = NULL;  /* non-NULL = live RAM stream mode */

    (void)args;
    (void)argp;

    memset(&track, 0, sizeof(track));
    memset(&source, 0, sizeof(source));
    memcpy(&track, &g_playback.current_track, sizeof(track));

    if (track.id[0] == '\0') {
        error_code = AUDIO_PLAYER_ERR_SOURCE;
        goto cleanup;
    }

    set_track_snapshot(&track);
    set_status(AUDIO_PLAYER_OPENING, track.id, 0);

    error_code = wait_for_source(track.id, &source);
    if (error_code != 0) {
        goto cleanup;
    }
    source_acquired = 1;
    set_bitrate_snapshot(source.bitrate_kbps);

    if (!s_backend_ready) {
        error_code = s_backend_error != 0 ? s_backend_error : AUDIO_PLAYER_ERR_SOURCE;
        goto cleanup;
    }

    if (source.use_stream_buf) {
        /* Live streaming: read from the per-source RAM buffer. */
        sbuf = source.stream_buf;
        if (!sbuf) {
            error_code = AUDIO_PLAYER_ERR_SOURCE;
            goto cleanup;
        }
        fd = -1;
        logLine("ap: live ram stream mode track_id='%s' slot=%d\n",
                track.id, source.slot_id);
    } else {
        /* Cached playback — final .mp3 already on disk. */
        fd = fs_open(source.path, PSP_O_RDONLY, 0777);
        if (fd < 0) {
            error_code = fd;
            goto cleanup;
        }
        logLine("ap: cached file mode track_id='%s' path='%s'\n",
                track.id, source.path);
    }

    {
        SceMp3InitArg init_arg;
        memset(&init_arg, 0, sizeof(init_arg));
        init_arg.mp3StreamStart = 0;
        init_arg.mp3StreamEnd = source.content_length > 0 ? source.content_length : source.downloaded_bytes;
        stream_end = (int)init_arg.mp3StreamEnd;
        memset(s_mp3_buf, 0, sizeof(s_mp3_buf));
        memset(s_pcm_buf, 0, sizeof(s_pcm_buf));
        sceKernelDcacheWritebackRange(s_mp3_buf, sizeof(s_mp3_buf));
        sceKernelDcacheWritebackRange(s_pcm_buf, sizeof(s_pcm_buf));
        init_arg.mp3Buf = s_mp3_buf;
        init_arg.mp3BufSize = sizeof(s_mp3_buf);
        init_arg.pcmBuf = s_pcm_buf;
        init_arg.pcmBufSize = sizeof(s_pcm_buf);

        handle = sceMp3ReserveMp3Handle(&init_arg);
    }
    if (handle < 0) {
        error_code = handle;
        handle = -1;
        goto cleanup;
    }
    logLine("ap: sceMp3ReserveMp3Handle ok track_id='%s' handle=%d stream_end=%d\n",
            track.id, handle, stream_end);
    eq_reset();  // новый трек: старые хвосты фильтров не щёлкают

    {
        int64_t required_bytes = 0;
        error_code = sbuf ? feed_mp3_stream_ram(handle, track.id, &required_bytes, sbuf)
                          : feed_mp3_stream(&fd, handle, track.id, &required_bytes);
        if (error_code == 0) {
            error_code = wait_for_more_data(track.id, required_bytes);
            if (error_code == AUDIO_PLAYER_EOF) {
                stop_state = 1;
                goto cleanup;
            }
            if (error_code != 0) {
                goto cleanup;
            }
            error_code = sbuf ? feed_mp3_stream_ram(handle, track.id, &required_bytes, sbuf)
                              : feed_mp3_stream(&fd, handle, track.id, &required_bytes);
        }
        if (error_code == AUDIO_PLAYER_EOF) {
            stop_state = 1;
            goto cleanup;
        }
        if (error_code < 0) {
            goto cleanup;
        }
    }

    error_code = sceMp3Init(handle);
    if (error_code < 0) {
        goto cleanup;
    }
    logLine("ap: sceMp3Init ok track_id='%s' handle=%d\n", track.id, handle);

    /* The API bitrate is available for a fresh network stream, but a cache hit
     * intentionally skips get-file-info and therefore carries zero here.
     * Once sceMp3Init has parsed the stream header, the decoder is the common
     * source of truth for both RAM streams and files from the Memory Stick. */
    {
        int decoded_bitrate_kbps = sceMp3GetBitRate(handle);
        if (decoded_bitrate_kbps > 0) {
            set_bitrate_snapshot(decoded_bitrate_kbps);
            logLine("ap: decoder bitrate track_id='%s' bitrate=%d kbps source=%s\n",
                    track.id, decoded_bitrate_kbps,
                    sbuf ? "ram" : "disk");
        } else {
            logLine("ap: decoder bitrate unavailable track_id='%s' rc=0x%08X fallback=%d\n",
                    track.id, decoded_bitrate_kbps, source.bitrate_kbps);
        }
    }

    error_code = sceMp3SetLoopNum(handle, 0);
    if (error_code < 0) {
        goto cleanup;
    }

    loop_num = sceMp3GetLoopNum(handle);
    logLine("ap: loop mode track_id='%s' loop=%d\n", track.id, loop_num);

    sampling_rate = sceMp3GetSamplingRate(handle);
    if (sampling_rate < 0) {
        error_code = sampling_rate;
        goto cleanup;
    }

    num_channels = sceMp3GetMp3ChannelNum(handle);
    if (num_channels < 0) {
        error_code = num_channels;
        goto cleanup;
    }
    logLine("ap: format ok track_id='%s' rate=%d channels=%d\n",
            track.id, sampling_rate, num_channels);
        set_format_snapshot(sampling_rate, num_channels);

    set_active_status(AUDIO_PLAYER_PLAYING, track.id);

    while (!s_stop_requested) {
        if (wait_while_paused(track.id) != 0) {
            stop_state = 1;
            break;
        }
        int need_data = sceMp3CheckStreamDataNeeded(handle);
        if (need_data < 0) {
            error_code = need_data;
            goto cleanup;
        }

        if (need_data > 0) {
            int64_t required_bytes = 0;
            int feed_rc = sbuf ? feed_mp3_stream_ram(handle, track.id, &required_bytes, sbuf)
                               : feed_mp3_stream(&fd, handle, track.id, &required_bytes);
            if (feed_rc == 0) {
                feed_rc = wait_for_more_data(track.id, required_bytes);
                if (feed_rc == AUDIO_PLAYER_EOF) {
                    logLine("ap: eof from wait_for_more_data track_id='%s' required=%d played_ms=%d loop=%d\n",
                            track.id,
                            (int)required_bytes,
                            sampling_rate > 0 ? (int)((played_samples * 1000) / sampling_rate) : 0,
                            loop_num);
                    logger_flush();
                    stop_state = 1;
                    break;
                }
                if (feed_rc != 0) {
                    error_code = feed_rc;
                    goto cleanup;
                }
                feed_rc = sbuf ? feed_mp3_stream_ram(handle, track.id, &required_bytes, sbuf)
                               : feed_mp3_stream(&fd, handle, track.id, &required_bytes);
            }
            if (feed_rc == AUDIO_PLAYER_EOF) {
                logLine("ap: eof from feed track_id='%s' required=%d played_ms=%d loop=%d\n",
                        track.id,
                        (int)required_bytes,
                        sampling_rate > 0 ? (int)((played_samples * 1000) / sampling_rate) : 0,
                        loop_num);
                logger_flush();
                stop_state = 1;
                break;
            }
            if (feed_rc < 0) {
                error_code = feed_rc;
                goto cleanup;
            }
        }

        {
            short *pcm_ptr = NULL;
            int bytes_decoded = sceMp3Decode(handle, &pcm_ptr);
            if (bytes_decoded == AUDIO_PLAYER_DECODE_EOS || bytes_decoded == 0) {
                AudioCacheStatus status;
                audio_cache_get_status(&status);
                if (strcmp(status.track_id, track.id) != 0) {
                    error_code = AUDIO_PLAYER_ERR_TRACK_MISMATCH;
                    goto cleanup;
                }
                if (!status.complete) {
                    error_code = wait_for_more_data(track.id, status.downloaded_bytes);
                    if (error_code == AUDIO_PLAYER_EOF) {
                        logLine("ap: eof after decode wait track_id='%s' bytes_decoded=%d played_ms=%d loop=%d\n",
                                track.id,
                                bytes_decoded,
                                sampling_rate > 0 ? (int)((played_samples * 1000) / sampling_rate) : 0,
                                loop_num);
                        logger_flush();
                        stop_state = 1;
                        break;
                    }
                    if (error_code != 0) {
                        goto cleanup;
                    }
                    set_active_status(AUDIO_PLAYER_PLAYING, track.id);
                    continue;
                }
                logLine("ap: eof decode terminal track_id='%s' bytes_decoded=%d played_ms=%d loop=%d\n",
                        track.id,
                        bytes_decoded,
                        sampling_rate > 0 ? (int)((played_samples * 1000) / sampling_rate) : 0,
                        loop_num);
                logger_flush();
                stop_state = 1;
                break;
            }
            if (bytes_decoded < 0) {
                error_code = bytes_decoded;
                goto cleanup;
            }

            sceKernelDcacheInvalidateRange(pcm_ptr, bytes_decoded);

            /* Эквалайзер: правим PCM после декодера, до вывода.
             * Изменённые строки отдаём обратно через writeback,
             * иначе SRC-канал прочитает старые данные из RAM. */
            if (eq_is_active()) {
                eq_process(pcm_ptr, bytes_decoded / (2 * num_channels),
                           num_channels, sampling_rate);
                sceKernelDcacheWritebackInvalidateRange(pcm_ptr, bytes_decoded);
            }

            if (!audio_reserved || reserved_bytes != bytes_decoded) {
                int sample_count;
                if (audio_reserved) {
                    release_audio_src_channel();
                    audio_reserved = 0;
                }

                sample_count = bytes_decoded / (2 * num_channels);
                if (sample_count < 17 || sample_count > 4111) {
                    error_code = AUDIO_PLAYER_ERR_SOURCE;
                    goto cleanup;
                }

                error_code = sceAudioSRCChReserve(sample_count, sampling_rate, num_channels);
                if (error_code < 0) {
                    goto cleanup;
                }

                audio_reserved = 1;
                reserved_bytes = bytes_decoded;
                logLine("ap: sceAudioSRCChReserve ok track_id='%s' samples=%d rate=%d channels=%d bytes_decoded=%d\n",
                        track.id, sample_count, sampling_rate, num_channels, bytes_decoded);
            }

            if (wait_while_paused(track.id) != 0) {
                stop_state = 1;
                break;
            }

            /* sceAudioSRCOutputBlocking returns the sample count on success
             * (e.g. 1152); keep that out of error_code so a normal stop does
             * not log a bogus "error=0x480". Only a negative value is a fault. */
            int out_rc = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, pcm_ptr);
            if (out_rc < 0) {
                error_code = out_rc;
                goto cleanup;
            }

            played_samples += bytes_decoded / (2 * num_channels);
            if (sampling_rate > 0) {
                set_position_snapshot((int)((played_samples * 1000) / sampling_rate));
            }

            if (s_state != AUDIO_PLAYER_PLAYING) {
                set_active_status(AUDIO_PLAYER_PLAYING, track.id);
            }
        }
    }

    if (s_stop_requested) {
        stop_state = 1;
    }

cleanup:
    if (s_stop_requested && s_state != AUDIO_PLAYER_STOPPING) {
        set_status(AUDIO_PLAYER_STOPPING, track.id[0] ? track.id : NULL, 0);
    }

    if (audio_reserved) {
        release_audio_src_channel();
    }
    if (handle >= 0) {
        sceMp3ReleaseMp3Handle(handle);
    }
    if (fd >= 0) {
        fs_close(fd);
    }
    if (source_acquired) {
        logLine("ap: releasing source track_id='%s'\n", source.track_id);
        audio_cache_release_source(source.track_id);
    }

    if (s_stop_requested || stop_state) {
        logLine("ap: cleanup stop track_id='%s' stop_requested=%d stop_state=%d position_ms=%d error=0x%08X\n",
                track.id[0] ? track.id : "",
                s_stop_requested,
                stop_state,
                sampling_rate > 0 ? (int)((played_samples * 1000) / sampling_rate) : 0,
                error_code);
        logger_flush();
        set_status((stop_state && !s_stop_requested) ? AUDIO_PLAYER_FINISHED : AUDIO_PLAYER_STOPPED,
                   track.id[0] ? track.id : NULL, 0);
    } else if (error_code != 0) {
        set_status(AUDIO_PLAYER_ERROR, track.id[0] ? track.id : NULL, error_code);
        logLine("ap: worker error track_id='%s' rc=0x%08X\n",
                track.id, error_code);
        logger_flush();
    }

    s_worker_running = 0;
    return 0;
}

void audio_player_init(void)
{
    int backend_rc;

    if (!s_mutex_initialized) {
        memset(&s_status_mutex, 0, sizeof(s_status_mutex));
        if (sceKernelCreateLwMutex(&s_status_mutex, "audio_player_status", 0, 0, NULL) >= 0) {
            s_mutex_initialized = 1;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    s_worker_thread = -1;
    s_worker_running = 0;
    s_stop_requested = 0;
    s_pause_requested = 0;
    s_resume_state = AUDIO_PLAYER_PLAYING;
    s_backend_ready = 0;
    s_backend_error = 0;

    backend_rc = ensure_backend_ready();
    set_status(AUDIO_PLAYER_IDLE, NULL, 0);
    if (backend_rc < 0) {
        set_status(AUDIO_PLAYER_ERROR, NULL, backend_rc);
        logLine("ap: init failed backend rc=0x%08X\n", backend_rc);
    } else {
        logLine("ap: init\n");
    }
}

void audio_player_shutdown(void)
{
    audio_player_stop();
    if (s_mp3_resource_initialized) {
        sceMp3TermResource();
        s_mp3_resource_initialized = 0;
    }
    s_backend_ready = 0;
    set_status(AUDIO_PLAYER_IDLE, NULL, 0);
    logLine("ap: shutdown\n");
}

int audio_player_start_current(void)
{
    AudioPlayerStatus status;

    reap_worker_thread();
    audio_player_get_status(&status);

    if (g_playback.current_track.id[0] == '\0') {
        return -1;
    }

    if (!s_backend_ready) {
        int rc = s_backend_error != 0 ? s_backend_error : AUDIO_PLAYER_ERR_SOURCE;
        set_status(AUDIO_PLAYER_ERROR, g_playback.current_track.id, rc);
        return rc;
    }

    if (s_worker_running && strcmp(status.track_id, g_playback.current_track.id) == 0) {
        return 0;
    }

    if (s_worker_running) {
        audio_player_stop();
        reap_worker_thread();
    }

    s_stop_requested = 0;
    s_pause_requested = 0;
    s_resume_state = AUDIO_PLAYER_PLAYING;
    logLine("ap: start_current accepted track_id='%s'\n", g_playback.current_track.id);
    set_status(AUDIO_PLAYER_OPENING, g_playback.current_track.id, 0);

    s_worker_thread = sceKernelCreateThread("audio_player_worker",
                                            audio_player_worker,
                                            AUDIO_PLAYER_WORKER_PRIORITY,
                                            AUDIO_PLAYER_WORKER_STACK_SIZE,
                                            PSP_THREAD_ATTR_USER,
                                            NULL);
    if (s_worker_thread < 0) {
        set_status(AUDIO_PLAYER_ERROR, g_playback.current_track.id, (int)s_worker_thread);
        return (int)s_worker_thread;
    }

    s_worker_running = 1;
    {
        int start_rc = sceKernelStartThread(s_worker_thread, 0, NULL);
        if (start_rc < 0) {
            int rc = start_rc;
        s_worker_running = 0;
        sceKernelDeleteThread(s_worker_thread);
        s_worker_thread = -1;
        set_status(AUDIO_PLAYER_ERROR, g_playback.current_track.id, rc);
        return rc;
        }
    }

    log_worker_priority(s_worker_thread);

    logLine("ap: worker start requested track_id='%s'\n", g_playback.current_track.id);
    return 0;
}

int audio_player_quiesce(void)
{
    if (s_worker_thread < 0) return 0;
    s_stop_requested = 1;
    s_pause_requested = 0;
    {
        SceUInt timeout_us = 20000000U;
        if (sceKernelWaitThreadEnd(s_worker_thread, &timeout_us) < 0) {
            logLine("ap: worker still active; resources retained\n");
            return -1;
        }
    }
    s_worker_running = 0;
    return 0;
}

void audio_player_pause(void)
{
    AudioPlayerState state = (AudioPlayerState)s_state;

    if (!s_worker_running || s_pause_requested ||
        (state != AUDIO_PLAYER_OPENING &&
         state != AUDIO_PLAYER_PLAYING &&
         state != AUDIO_PLAYER_BUFFERING)) {
        return;
    }

    s_resume_state = state;
    s_pause_requested = 1;
    set_status(AUDIO_PLAYER_PAUSED,
               s_status.track_id[0] ? s_status.track_id : NULL, 0);
    logLine("ap: pause track_id='%s' position_ms=%d\n",
            s_status.track_id, s_status.position_ms);
}

void audio_player_resume(void)
{
    AudioPlayerState resume_state;

    if (!s_worker_running || !s_pause_requested) {
        return;
    }

    resume_state = (AudioPlayerState)s_resume_state;
    if (resume_state != AUDIO_PLAYER_OPENING &&
        resume_state != AUDIO_PLAYER_PLAYING &&
        resume_state != AUDIO_PLAYER_BUFFERING) {
        resume_state = AUDIO_PLAYER_PLAYING;
    }
    s_pause_requested = 0;
    set_status(resume_state,
               s_status.track_id[0] ? s_status.track_id : NULL, 0);
    logLine("ap: resume track_id='%s' position_ms=%d\n",
            s_status.track_id, s_status.position_ms);
}

void audio_player_stop(void)
{
    if (!s_worker_running && s_worker_thread < 0) {
        if (s_state != AUDIO_PLAYER_IDLE) {
            set_position_snapshot(0);
            set_status(AUDIO_PLAYER_STOPPED,
                       s_status.track_id[0] ? s_status.track_id : NULL, 0);
        }
        return;
    }

    logLine("ap: stop requested track_id='%s'\n",
            s_status.track_id[0] ? s_status.track_id : "");
    s_stop_requested = 1;
    s_pause_requested = 0;
    if (s_state != AUDIO_PLAYER_IDLE &&
        s_state != AUDIO_PLAYER_STOPPED &&
        s_state != AUDIO_PLAYER_FINISHED) {
        set_status(AUDIO_PLAYER_STOPPING, s_status.track_id[0] ? s_status.track_id : NULL, 0);
    }

    if (s_worker_thread >= 0) {
        sceKernelWaitThreadEnd(s_worker_thread, NULL);
        sceKernelDeleteThread(s_worker_thread);
        s_worker_thread = -1;
    }

    s_worker_running = 0;
    set_position_snapshot(0);
    set_status(AUDIO_PLAYER_STOPPED, s_status.track_id[0] ? s_status.track_id : NULL, 0);
}

AudioPlayerState audio_player_get_state(void)
{
    return (AudioPlayerState)s_state;
}

bool audio_player_get_status(AudioPlayerStatus *out)
{
    return audio_player_get_snapshot(out);
}

bool audio_player_get_snapshot(AudioPlayerSnapshot *out)
{
    if (!out) {
        return false;
    }

    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
        *out = s_status;
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
    } else {
        *out = s_status;
    }

    return true;
}
