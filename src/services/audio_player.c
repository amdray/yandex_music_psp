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
#include "services/ym_flac.h"
#include "services/ym_aac.h"
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
/* Буфер PCM под 4096-кадровые куски FLAC/AAC (+запас под AAC-хвост). */
#define AUDIO_PLAYER_PCM_BUF_SIZE      ((4096 + 32) * 4)
/* SEEK: upper bound on garbage/EOS probe decodes after a decoder restart
 * while waiting for sceMp3 frame sync. See audio_player_apply_seek(). */
#define AUDIO_PLAYER_SEEK_RESYNC_FRAMES 8

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
/* Ping-pong выходные буферы. sceAudioSRCOutputBlocking блокируется до конца
 * предыдущего буфера и отдаёт текущий в очередь: SRC-DMA читает его
 * асинхронно следующие ~93 мс, пока декодер готовит следующий блок. Один
 * общий буфер = перезапись во время чтения DMA → порча начала каждого блока
 * на реальном PSP (периодический треск); PPSSPP копирует контент в момент
 * постановки в очередь и это маскирует. Два буфера: HW читает один, CPU
 * декодирует в другой. */
static unsigned char s_out_buf[2][AUDIO_PLAYER_PCM_BUF_SIZE] __attribute__((aligned(64)));
static int s_out_idx = 0;
/* SEEK: pending time-based seek request. The UI thread sets
 * s_seek_target_ms and s_seek_pending = 1 and returns immediately (never
 * blocks); the worker thread consumes the flag once per decode iteration
 * (see audio_player_apply_seek). Plain int volatile: single-copy atomic
 * on MIPS, no mutex needed. */
static volatile int s_seek_pending = 0;
static volatile int s_seek_target_ms = 0;

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
    s_status.codec[0] = '\0';

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

static void set_codec_snapshot(const char *codec)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    }
    if (codec && codec[0]) {
        strncpy(s_status.codec, codec, sizeof(s_status.codec) - 1);
    }
    s_status.codec[sizeof(s_status.codec) - 1] = '\0';
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
        set_status(state, track_id, 0);
    } else {
        set_status(state, track_id, 0);
    }
}

/* WAVE-REPORT: really-audible events (spec section 6). Static storage only:
 * no malloc, no HTTP/JSON on the audio thread. The controller consumes each
 * event exactly once; a new start_current() discards stale ones. */
static unsigned int s_report_generation = 0;
static int s_first_pcm_pending = 0;
static char s_first_pcm_track_id[40];
static unsigned int s_first_pcm_generation = 0;
static int s_terminal_pending = 0;
static char s_terminal_track_id[40];
static unsigned int s_terminal_generation = 0;
static AudioEndReason s_terminal_reason = AUDIO_END_MANUAL_STOP;
static int s_terminal_audible_ms = 0;
static int s_terminal_had_first_pcm = 0;

static void audio_report_lock(void)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    }
}

static void audio_report_unlock(void)
{
    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
    }
}

static void audio_report_publish_first_pcm(const char *track_id,
                                           unsigned int generation)
{
    if (!track_id || !track_id[0]) {
        return;
    }
    audio_report_lock();
    snprintf(s_first_pcm_track_id, sizeof(s_first_pcm_track_id),
             "%s", track_id);
    s_first_pcm_generation = generation;
    s_first_pcm_pending = 1;
    audio_report_unlock();
}

static void audio_report_publish_terminal(const char *track_id,
                                          unsigned int generation,
                                          AudioEndReason reason,
                                          int audible_ms,
                                          int had_first_pcm)
{
    if (!track_id || !track_id[0]) {
        return;
    }
    audio_report_lock();
    snprintf(s_terminal_track_id, sizeof(s_terminal_track_id),
             "%s", track_id);
    s_terminal_generation = generation;
    s_terminal_reason = reason;
    s_terminal_audible_ms = (audible_ms > 0) ? audible_ms : 0;
    s_terminal_had_first_pcm = had_first_pcm ? 1 : 0;
    s_terminal_pending = 1;
    audio_report_unlock();
}

void audio_player_set_report_generation(unsigned int generation)
{
    s_report_generation = generation;
}

int audio_player_take_first_pcm_event(char *out_track_id,
                                      size_t track_id_size,
                                      unsigned int *out_generation)
{
    int present = 0;

    if (!out_track_id || track_id_size == 0) {
        return -1;
    }
    out_track_id[0] = '\0';
    audio_report_lock();
    if (s_first_pcm_pending) {
        snprintf(out_track_id, track_id_size, "%s", s_first_pcm_track_id);
        if (out_generation) {
            *out_generation = s_first_pcm_generation;
        }
        s_first_pcm_pending = 0;
        present = 1;
    }
    audio_report_unlock();
    return present ? 0 : -1;
}

int audio_player_take_terminal_snapshot(char *out_track_id,
                                        size_t track_id_size,
                                        unsigned int *out_generation,
                                        AudioEndReason *out_reason,
                                        int *out_audible_ms,
                                        int *out_had_first_pcm)
{
    int present = 0;

    if (!out_track_id || track_id_size == 0) {
        return -1;
    }
    out_track_id[0] = '\0';
    audio_report_lock();
    if (s_terminal_pending) {
        snprintf(out_track_id, track_id_size, "%s", s_terminal_track_id);
        if (out_generation) {
            *out_generation = s_terminal_generation;
        }
        if (out_reason) {
            *out_reason = s_terminal_reason;
        }
        if (out_audible_ms) {
            *out_audible_ms = s_terminal_audible_ms;
        }
        if (out_had_first_pcm) {
            *out_had_first_pcm = s_terminal_had_first_pcm;
        }
        s_terminal_pending = 0;
        present = 1;
    }
    audio_report_unlock();
    return present ? 0 : -1;
}

/* Детекция недоёма SRC-канала. sceAudioSRCOutputBlocking блокируется до
 * конца предыдущего буфера: здоровый ритм подачи = длительность буфера;
 * ритм дольше бюджета (+15 мс запас) означает, что канал стоял — на
 * реальном PSP слышен щелчок/обрыв. Ожидаемые паузы (буферизация, пауза,
 * seek, смена трека) сбрасывают маркер и не считаются. */
static u64 s_out_submit_us;
static int s_out_gap_count;
static int s_out_gap_logged;

static void ap_note_output(int frames, const char *tag)
{
    u64 now = sceKernelGetSystemTimeWide();
    u64 budget = (u64)frames * 1000000ULL / 44100ULL;

    if (s_out_submit_us != 0 && now > s_out_submit_us + budget + 15000ULL) {
        s_out_gap_count++;
        if (s_out_gap_logged < 5 || (s_out_gap_count % 25) == 0) {
            s_out_gap_logged++;
            logLine("ap: gap %s +%dms budget=%dms count=%d\n",
                    tag, (int)((now - s_out_submit_us - budget) / 1000ULL),
                    (int)(budget / 1000ULL), s_out_gap_count);
        }
    }
    s_out_submit_us = now;
}

static void ap_forget_output_timing(void)
{
    s_out_submit_us = 0;
}

static void ap_reset_output_stats(void)
{
    s_out_submit_us = 0;
    s_out_gap_count = 0;
    s_out_gap_logged = 0;
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
    ap_forget_output_timing();
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
    int64_t last_bytes = -1;

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

            /* Идёт здоровое скачивание (в т.ч. длинные непрогрессивные
             * загрузки flac-mp4 ~40+ c): рост байтов продлевает дедлайн,
             * чтобы не обрывать ещё не готовый источник. */
            if (status.state == AUDIO_CACHE_DOWNLOADING &&
                status.downloaded_bytes != last_bytes) {
                last_bytes = status.downloaded_bytes;
                deadline = sceKernelGetSystemTimeWide() + AUDIO_PLAYER_WAIT_DEADLINE_US;
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

    /* Ожидание данных — ожидаемый обрыв звука (уже виден по 'buffering
     * wait' в логе), в подаче вывода не считаем. */
    ap_forget_output_timing();
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
            if (required_bytes <= status.downloaded_bytes) {
                /* Файл докачан целиком и bytes для required уже на диске:
                 * даём декодеру ещё шанс прочитать, а не уводим трек в
                 * ложный FINISHED (пользователь видел смену трека). */
                logLine("ap: buffering complete, retry track_id='%s' have=%d required=%d\n",
                        track_id,
                        (int)status.downloaded_bytes,
                        (int)required_bytes);
                return 0;
            }
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

/* SEEK: apply one queued time-based seek on the worker thread.
 *
 * Byte mapping is byte_offset = total_bytes * target_ms / duration_ms, i.e. a
 * CBR approximation assuming constant byte rate over the track. On VBR the
 * real audio position drifts from the request by the local bitrate ratio.
 *
 * There is no public cache "restart at offset" API (Range resume lives inside
 * audio_cache.c's own download loop), and restarting the cache job would
 * discard buffered data, so no cache job is touched: the read cursor is
 * repositioned inside the already-acquired source (RAM stream buffer or
 * cached file, both randomly readable) and the decoder window is restarted
 * at byte_offset. If the target bytes are not downloaded yet,
 * wait_for_more_data() parks on the async producer exactly like the normal
 * buffering path -- no blocking network calls here.
 *
 * No malloc: reuses s_mp3_buf/s_pcm_buf. Position source of truth is
 * unchanged (played_samples/sampling_rate); nothing new is exposed.
 *
 * Decoder resync relies on sceMp3 frame sync after the restart: up to
 * AUDIO_PLAYER_SEEK_RESYNC_FRAMES probe decodes are attempted and
 * garbage/EOS results are skipped; persistent failure gives up with an
 * error. At most one good frame (~26 ms) is discarded as the sync probe, so
 * no bulk "skip until target" is needed -- the new window already starts at
 * the target. A mid-frame mp3StreamStart that makes sceMp3Init itself fail
 * is NOT retried here (the error is returned; see risks).
 *
 * Returns 0 on success, AUDIO_PLAYER_EOF when the target is at/past EOF
 * (caller finishes the track like a natural EOF), <0 on error.
 * Consumes (clears) s_seek_pending in all cases. */
static int audio_player_apply_seek(TrackEntry *track, AudioSourceSnapshot *source,
                                   int *fd_ptr, int *handle_ptr,
                                   AudioStreamBuf *sbuf, int stream_end,
                                   int *rate_ptr, int *channels_ptr,
                                   int64_t *played_ptr,
                                   int *reserved_ptr, int *reserved_bytes_ptr)
{
    int target_ms = s_seek_target_ms;
    int64_t total_bytes = (source->content_length > 0)
        ? source->content_length : (int64_t)stream_end;
    int64_t byte_offset;
    int64_t target_samples;
    int resync_left;
    int rc;
    SceMp3InitArg init_arg;

    s_seek_pending = 0;

    if (!track || track->duration_ms <= 0 || total_bytes <= 0) {
        return AUDIO_PLAYER_ERR_SOURCE;
    }
    if (target_ms < 0) {
        target_ms = 0;
    }
    if ((int64_t)target_ms >= (int64_t)track->duration_ms) {
        return AUDIO_PLAYER_EOF;
    }
    /* CBR approximation; 64-bit math (bytes * ms overflows 32 bit). */
    byte_offset = (total_bytes * (int64_t)target_ms) / (int64_t)track->duration_ms;
    if (byte_offset < 0) {
        byte_offset = 0;
    }
    if (byte_offset >= total_bytes) {
        return AUDIO_PLAYER_EOF;
    }

    /* Flush decoded-but-unplayed data. Output is synchronous
     * (sceAudioSRCOutputBlocking), so the only live audio state is the
     * reserved SRC channel; drop it and let the main loop re-reserve once
     * the post-seek frame size is known. */
    if (*reserved_ptr) {
        release_audio_src_channel();
        *reserved_ptr = 0;
        *reserved_bytes_ptr = 0;
    }
    if (*handle_ptr >= 0) {
        sceMp3ReleaseMp3Handle(*handle_ptr);
        *handle_ptr = -1;
    }

    memset(&init_arg, 0, sizeof(init_arg));
    init_arg.mp3StreamStart = (SceOff)byte_offset;
    init_arg.mp3StreamEnd = (SceOff)total_bytes;
    memset(s_mp3_buf, 0, sizeof(s_mp3_buf));
    memset(s_pcm_buf, 0, sizeof(s_pcm_buf));
    sceKernelDcacheWritebackRange(s_mp3_buf, sizeof(s_mp3_buf));
    sceKernelDcacheWritebackRange(s_pcm_buf, sizeof(s_pcm_buf));
    init_arg.mp3Buf = s_mp3_buf;
    init_arg.mp3BufSize = sizeof(s_mp3_buf);
    init_arg.pcmBuf = s_pcm_buf;
    init_arg.pcmBufSize = sizeof(s_pcm_buf);

    *handle_ptr = sceMp3ReserveMp3Handle(&init_arg);
    if (*handle_ptr < 0) {
        return *handle_ptr;
    }
    logLine("ap: seek reserve track_id='%s' target_ms=%d byte_offset=%d total=%d\n",
            track->id, target_ms, (int)byte_offset, (int)total_bytes);

    {
        int64_t required_bytes = 0;
        rc = sbuf ? feed_mp3_stream_ram(*handle_ptr, track->id, &required_bytes, sbuf)
                  : feed_mp3_stream(fd_ptr, *handle_ptr, track->id, &required_bytes);
        if (rc == 0) {
            rc = wait_for_more_data(track->id, required_bytes);
            if (rc == AUDIO_PLAYER_EOF) {
                sceMp3ReleaseMp3Handle(*handle_ptr);
                *handle_ptr = -1;
                return AUDIO_PLAYER_EOF;
            }
            if (rc != 0) {
                sceMp3ReleaseMp3Handle(*handle_ptr);
                *handle_ptr = -1;
                return rc;
            }
            rc = sbuf ? feed_mp3_stream_ram(*handle_ptr, track->id, &required_bytes, sbuf)
                      : feed_mp3_stream(fd_ptr, *handle_ptr, track->id, &required_bytes);
        }
        if (rc == AUDIO_PLAYER_EOF) {
            sceMp3ReleaseMp3Handle(*handle_ptr);
            *handle_ptr = -1;
            return AUDIO_PLAYER_EOF;
        }
        if (rc < 0) {
            sceMp3ReleaseMp3Handle(*handle_ptr);
            *handle_ptr = -1;
            return rc;
        }
    }

    rc = sceMp3Init(*handle_ptr);
    if (rc < 0) {
        logLine("ap: seek init failed track_id='%s' rc=0x%08X (mid-frame Start?)\n",
                track->id, rc);
        sceMp3ReleaseMp3Handle(*handle_ptr);
        *handle_ptr = -1;
        return rc;
    }

    rc = sceMp3SetLoopNum(*handle_ptr, 0);
    if (rc < 0) {
        sceMp3ReleaseMp3Handle(*handle_ptr);
        *handle_ptr = -1;
        return rc;
    }

    rc = sceMp3GetSamplingRate(*handle_ptr);
    if (rc < 0) {
        sceMp3ReleaseMp3Handle(*handle_ptr);
        *handle_ptr = -1;
        return rc;
    }
    *rate_ptr = rc;

    rc = sceMp3GetMp3ChannelNum(*handle_ptr);
    if (rc < 0) {
        sceMp3ReleaseMp3Handle(*handle_ptr);
        *handle_ptr = -1;
        return rc;
    }
    *channels_ptr = rc;
    set_format_snapshot(*rate_ptr, *channels_ptr);

    target_samples = ((int64_t)target_ms * (int64_t)*rate_ptr) / 1000;
    *played_ptr = target_samples;
    set_position_snapshot(target_ms);

    /* Resync probe: rely on sceMp3 frame sync, skip garbage/EOS up to N. */
    rc = AUDIO_PLAYER_ERR_SOURCE;
    resync_left = AUDIO_PLAYER_SEEK_RESYNC_FRAMES;
    while (resync_left > 0) {
        short *probe_pcm = NULL;
        int need_data;
        int decoded;
        int64_t required_bytes = 0;
        int feed_rc;

        resync_left--;

        need_data = sceMp3CheckStreamDataNeeded(*handle_ptr);
        if (need_data < 0) {
            rc = need_data;
            logLine("ap: seek probe need_data bad track_id='%s' left=%d rc=0x%08X\n",
                    track->id, resync_left, need_data);
            continue;
        }
        if (need_data > 0) {
            feed_rc = sbuf ? feed_mp3_stream_ram(*handle_ptr, track->id, &required_bytes, sbuf)
                           : feed_mp3_stream(fd_ptr, *handle_ptr, track->id, &required_bytes);
            if (feed_rc == 0) {
                feed_rc = wait_for_more_data(track->id, required_bytes);
                if (feed_rc != 0) {
                    rc = (feed_rc == AUDIO_PLAYER_EOF) ? AUDIO_PLAYER_EOF : feed_rc;
                    break;
                }
                feed_rc = sbuf ? feed_mp3_stream_ram(*handle_ptr, track->id, &required_bytes, sbuf)
                               : feed_mp3_stream(fd_ptr, *handle_ptr, track->id, &required_bytes);
            }
            if (feed_rc == AUDIO_PLAYER_EOF) {
                rc = AUDIO_PLAYER_EOF;
                break;
            }
            if (feed_rc < 0) {
                rc = feed_rc;
                break;
            }
        }

        decoded = sceMp3Decode(*handle_ptr, &probe_pcm);
        if (decoded > 0) {
            /* In sync; this one probe frame is discarded (<= ~26 ms gap)
             * and the main loop outputs subsequent frames normally. */
            logLine("ap: seek sync ok track_id='%s' target_ms=%d\n",
                    track->id, target_ms);
            rc = 0;
            break;
        }
        rc = (decoded == AUDIO_PLAYER_DECODE_EOS || decoded == 0)
             ? AUDIO_PLAYER_ERR_SOURCE : decoded;
        logLine("ap: seek probe bad track_id='%s' left=%d rc=0x%08X\n",
                track->id, resync_left, decoded);
    }
    if (rc == AUDIO_PLAYER_EOF) {
        sceMp3ReleaseMp3Handle(*handle_ptr);
        *handle_ptr = -1;
        return AUDIO_PLAYER_EOF;
    }
    if (rc != 0) {
        sceMp3ReleaseMp3Handle(*handle_ptr);
        *handle_ptr = -1;
        logLine("ap: seek resync give-up track_id='%s' rc=0x%08X\n",
                track->id, rc);
        return rc;
    }

    eq_reset();
    set_active_status(AUDIO_PLAYER_PLAYING, track->id);
    return 0;
}

/* FLAC: длина доступного префикса файла (прогрессивная докачка). */
static int64_t flac_file_avail_cb(void *user){
    const char *track_id = (const char *)user;
    AudioCacheStatus status;
    if (!track_id || !audio_cache_get_status(&status)) {
        return 0;
    }
    if (strcmp(status.track_id, track_id) != 0) {
        return 0;
    }
    return status.downloaded_bytes;
}

/* Колбэк ожидания фронтира для ym_flac: чтение блокируется прямо в
 * декодере, пока данные не доедут. Так dr_flac не получает 0 посреди
 * трека (ложный EOF -> автоскип, который видел пользователь). */
static int flac_wait_cb(void *user, int64_t need_bytes)
{
    const char *track_id = (const char *)user;
    return wait_for_more_data(track_id, need_bytes) == 0 ? 0 : -1;
}

/* FLAC-цикл: файл (прогрессивный или готовый) -> dr_flac -> PCM s16 ->
 * тот же SRC-вывод/EQ/позиция, что у MP3. MP3-путь ниже не тронут. */
/* 4096 кадров (~93 мс) вместо 1152 (~26 мс): на реальном PSP декодер+fs+
 * сеть легко проскакивают 26 мс, канал замирает — слышно щелчки. 93 мс
 * дают запас; предел sceAudioSRCChReserve в этой версии — 4111 кадров. */
#define FLAC_DECODE_FRAMES 4096

static int audio_player_flac_file_run(const TrackEntry *track,
                                      const AudioSourceSnapshot *source,
                                      int *fd,
                                      YmFlacAvailFn avail_fn, void *avail_user,
                                      int *rate_ptr, int *channels_ptr,
                                      int64_t *played_ptr,
                                      int *audio_reserved_ptr,
                                      int *reserved_bytes_ptr,
                                      int *first_pcm_done_ptr,
                                      unsigned worker_generation,
                                      int *stop_state_ptr)
{
    YmFlac *flac = NULL;
    YmFlacFormat fmt;
    int64_t played = 0;
    int seek_wait_left;
    int flac_stuck = 0;

    memset(&fmt, 0, sizeof(fmt));
    {
        const char *dot = strrchr(source->path, '.');
        if (dot && strcmp(dot, ".mp4") == 0) {
            flac = ym_flac_open_mp4(fd, source->path, avail_fn, avail_user);
        } else {
            flac = ym_flac_open_file(fd, source->path, avail_fn, avail_user);
        }
    }
    if (!flac) {
        logLine("ap: flac open failed track_id='%s'\n", track->id);
        return AUDIO_PLAYER_ERR_SOURCE;
    }
    ym_flac_set_wait(flac, flac_wait_cb, (void *)track->id);
    if (ym_flac_format(flac, &fmt) != 0 ||
        fmt.sample_rate != 44100 || fmt.channels != 2 ||
        fmt.bits_per_sample < 16 || fmt.bits_per_sample > 32) {
        logLine("ap: flac format rejected track_id='%s' %d/%d/%d (need 44100/2/16..32)\n",
                track->id, fmt.sample_rate, fmt.channels, fmt.bits_per_sample);
        ym_flac_close(flac);
        return AUDIO_PLAYER_ERR_SOURCE;
    }
    logLine("ap: flac ok track_id='%s' 44100/16/stereo total_frames=%d\n",
            track->id, (int)fmt.total_frames);
    *rate_ptr = 44100;
    *channels_ptr = 2;
    set_format_snapshot(44100, 2);
    set_bitrate_snapshot(source->bitrate_kbps > 0
                             ? source->bitrate_kbps
                             : (track->duration_ms > 0 &&
                                        source->content_length > 0
                                    ? (int)((source->content_length * 8) /
                                            track->duration_ms)
                                    : 0));
    eq_reset();
    set_active_status(AUDIO_PLAYER_PLAYING, track->id);

    while (!s_stop_requested) {
        int64_t got;
        int bytes_decoded;
        int sample_count;
        int out_rc;

        if (wait_while_paused(track->id) != 0) {
            *stop_state_ptr = 1;
            break;
        }
        if (s_seek_pending) {
            int target_ms = s_seek_target_ms;
            int64_t target_frame = ((int64_t)target_ms * 44100) / 1000;
            int64_t est = target_frame * 4 + 8192;
            int rc;
            s_seek_pending = 0;
            rc = wait_for_more_data(track->id, est);
            if (rc == AUDIO_PLAYER_EOF) {
                *stop_state_ptr = 1;
                break;
            }
            if (rc != 0) {
                ym_flac_close(flac);
                return rc;
            }
            seek_wait_left = 3;
            for (;;) {
                rc = ym_flac_seek_frame(flac, target_frame);
                if (rc == 0) break;
                if (rc != -1 || seek_wait_left-- <= 0) {
                    logLine("ap: flac seek failed track_id='%s' rc=%d\n",
                            track->id, rc);
                    ym_flac_close(flac);
                    return AUDIO_PLAYER_ERR_SOURCE;
                }
                rc = wait_for_more_data(track->id, est + 65536);
                if (rc == AUDIO_PLAYER_EOF) {
                    *stop_state_ptr = 1;
                    break;
                }
                if (rc != 0) {
                    ym_flac_close(flac);
                    return rc;
                }
            }
            if (rc == AUDIO_PLAYER_EOF) {
                break;
            }
            played = target_frame;
            *played_ptr = played;
            set_position_snapshot(target_ms);
            eq_reset();
            logLine("ap: flac seek ok track_id='%s' target_ms=%d\n",
                    track->id, target_ms);
            continue;
        }

        got = ym_flac_read_s16(flac, (short *)s_out_buf[s_out_idx], FLAC_DECODE_FRAMES);
        if (got < 0) {
            ym_flac_close(flac);
            return AUDIO_PLAYER_ERR_SOURCE;
        }
        if (got == 0) {
            AudioCacheStatus status;
            int rc;
            audio_cache_get_status(&status);
            if (strcmp(status.track_id, track->id) != 0) {
                ym_flac_close(flac);
                return AUDIO_PLAYER_ERR_TRACK_MISMATCH;
            }
            if (!status.complete) {
                /* Ждём сдвиг счётчика (как mp3-путь): +64K сверх текущего
                 * на докачанном файле не добирается никогда и давал ложный
                 * EOF сразу после complete. */
                rc = wait_for_more_data(track->id,
                                        status.downloaded_bytes);
                if (rc == AUDIO_PLAYER_EOF) {
                    *stop_state_ptr = 1;
                    break;
                }
                if (rc != 0) {
                    ym_flac_close(flac);
                    return rc;
                }
                continue;
            }
            if (ym_flac_at_end(flac)) {
                /* Настоящий конец: декодер дошёл до total_frames. */
                logLine("ap: eof decode terminal (flac) track_id='%s' played_ms=%d have=%d len=%d\n",
                        track->id, (int)((played * 1000) / 44100),
                        (int)status.downloaded_bytes, (int)status.content_length);
                logger_flush();
                *stop_state_ptr = 1;
                break;
            }
            /* Данные целиком на диске, а кадр не читается — раньше это
             * считалось концом трека и уводило на следующий. Даём
             * ресинку ym_flac (переоткрытие + возврат на кадр) несколько
             * попыток, не объявляя ложный EOF. */
            if (++flac_stuck <= 5) {
                logLine("ap: flac resync retry %d track_id='%s' have=%d\n",
                        flac_stuck, track->id, (int)status.downloaded_bytes);
                continue;
            }
            logLine("ap: flac decode stuck track_id='%s' played_ms=%d -> source error\n",
                    track->id, (int)((played * 1000) / 44100));
            ym_flac_close(flac);
            return AUDIO_PLAYER_ERR_SOURCE;
        }
        flac_stuck = 0;

        if (eq_is_active()) {
            eq_process((short *)s_out_buf[s_out_idx], (int)got, 2, 44100);
        }
        /* Короткий хвост добиваем тишиной до FLAC_DECODE_FRAMES: размер
         * выхода константен → резерв SRC-канала ровно один раз за трек
         * (без release/reserve посреди игры) и DMA всегда читает полный
         * блок. */
        if (got < FLAC_DECODE_FRAMES) {
            memset((unsigned char *)s_out_buf[s_out_idx] + (size_t)got * 4, 0,
                   (size_t)(FLAC_DECODE_FRAMES - (int)got) * 4);
        }
        bytes_decoded = FLAC_DECODE_FRAMES * 4;
        /* PCM декодируется CPU в кэш: выталкиваем ВЕСЬ выходной буфер в RAM
         * до вывода ВСЕГДА, иначе на реальном PSP SRC-DMA читает старые строки
         * кэша (PPSSPP это маскирует) — звук «хрипит». */
        sceKernelDcacheWritebackRange(s_out_buf[s_out_idx], bytes_decoded);

        if (!*audio_reserved_ptr || *reserved_bytes_ptr != bytes_decoded) {
            if (*audio_reserved_ptr) {
                release_audio_src_channel();
                *audio_reserved_ptr = 0;
            }
            sample_count = bytes_decoded / 4;
            if (sample_count < 17 || sample_count > 4111) {
                ym_flac_close(flac);
                return AUDIO_PLAYER_ERR_SOURCE;
            }
            if (sceAudioSRCChReserve(sample_count, 44100, 2) < 0) {
                ym_flac_close(flac);
                return AUDIO_PLAYER_ERR_SOURCE;
            }
            *audio_reserved_ptr = 1;
            *reserved_bytes_ptr = bytes_decoded;
            logLine("ap: sceAudioSRCChReserve ok (flac) track_id='%s' samples=%d\n",
                    track->id, sample_count);
        }

        if (wait_while_paused(track->id) != 0) {
            *stop_state_ptr = 1;
            break;
        }

        out_rc = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX,
                                           (short *)s_out_buf[s_out_idx]);
        if (out_rc < 0) {
            ym_flac_close(flac);
            return out_rc;
        }
        s_out_idx ^= 1;
        ap_note_output(FLAC_DECODE_FRAMES, "flac");

        played += got;
        *played_ptr = played;
        if (!*first_pcm_done_ptr) {
            *first_pcm_done_ptr = 1;
            audio_report_publish_first_pcm(track->id, worker_generation);
        }
        set_position_snapshot((int)((played * 1000) / 44100));
        if (s_state != AUDIO_PLAYER_PLAYING) {
            set_active_status(AUDIO_PLAYER_PLAYING, track->id);
        }
    }

    ym_flac_close(flac);
    return 0;
}

/* AAC: файл .aac (MP4, esds/ASC) -> faad2 -> PCM s16 44100/stereo ->
 * тот же SRC-вывод/EQ/позиция, что у FLAC/MP3. */
static int audio_player_aac_file_run(const TrackEntry *track,
                                     const AudioSourceSnapshot *source,
                                     int *fd,
                                     YmFlacAvailFn avail_fn, void *avail_user,
                                     int *rate_ptr, int *channels_ptr,
                                     int64_t *played_ptr,
                                     int *audio_reserved_ptr,
                                     int *reserved_bytes_ptr,
                                     int *first_pcm_done_ptr,
                                     unsigned worker_generation,
                                     int *stop_state_ptr)
{
    YmAac *aac = NULL;
    YmAacFormat fmt;
    int64_t played = 0;
    int seek_wait_left;
    int aac_stuck = 0;

    memset(&fmt, 0, sizeof(fmt));
    aac = ym_aac_open_file(fd, source->path, avail_fn, avail_user);
    if (!aac) {
        logLine("ap: aac open failed track_id='%s'\n", track->id);
        return AUDIO_PLAYER_ERR_SOURCE;
    }
    if (ym_aac_format(aac, &fmt) != 0 ||
        fmt.sample_rate != 44100 || fmt.channels != 2) {
        logLine("ap: aac format rejected track_id='%s' %d/%d (need 44100/2)\n",
                track->id, fmt.sample_rate, fmt.channels);
        ym_aac_close(aac);
        return AUDIO_PLAYER_ERR_SOURCE;
    }
    logLine("ap: aac ok track_id='%s' 44100/stereo total_frames=%d\n",
            track->id, (int)fmt.total_frames);
    *rate_ptr = 44100;
    *channels_ptr = 2;
    set_format_snapshot(44100, 2);
    set_bitrate_snapshot(source->bitrate_kbps > 0
                             ? source->bitrate_kbps
                             : (track->duration_ms > 0 &&
                                        source->content_length > 0
                                    ? (int)((source->content_length * 8) /
                                            track->duration_ms)
                                    : 0));
    eq_reset();
    set_active_status(AUDIO_PLAYER_PLAYING, track->id);

    while (!s_stop_requested) {
        int64_t got;
        int bytes_decoded;
        int sample_count;
        int out_rc;

        if (wait_while_paused(track->id) != 0) {
            *stop_state_ptr = 1;
            break;
        }
        if (s_seek_pending) {
            int target_ms = s_seek_target_ms;
            int64_t target_frame = ((int64_t)target_ms * 44100) / 1000;
            int64_t est = target_frame * 4 + 8192;
            int rc;
            s_seek_pending = 0;
            rc = wait_for_more_data(track->id, est);
            if (rc == AUDIO_PLAYER_EOF) {
                *stop_state_ptr = 1;
                break;
            }
            if (rc != 0) {
                ym_aac_close(aac);
                return rc;
            }
            seek_wait_left = 3;
            for (;;) {
                rc = ym_aac_seek_frame(aac, target_frame);
                if (rc == 0) break;
                if (rc != -1 || seek_wait_left-- <= 0) {
                    logLine("ap: aac seek failed track_id='%s' rc=%d\n",
                            track->id, rc);
                    ym_aac_close(aac);
                    return AUDIO_PLAYER_ERR_SOURCE;
                }
                rc = wait_for_more_data(track->id, est + 65536);
                if (rc == AUDIO_PLAYER_EOF) {
                    *stop_state_ptr = 1;
                    break;
                }
                if (rc != 0) {
                    ym_aac_close(aac);
                    return rc;
                }
            }
            if (rc == AUDIO_PLAYER_EOF) {
                break;
            }
            played = target_frame;
            *played_ptr = played;
            set_position_snapshot(target_ms);
            eq_reset();
            logLine("ap: aac seek ok track_id='%s' target_ms=%d\n",
                    track->id, target_ms);
            continue;
        }

        got = ym_aac_read_s16(aac, (short *)s_out_buf[s_out_idx], FLAC_DECODE_FRAMES);
        if (got < 0) {
            ym_aac_close(aac);
            return AUDIO_PLAYER_ERR_SOURCE;
        }
        if (got == 0) {
            AudioCacheStatus status;
            int rc;
            audio_cache_get_status(&status);
            if (strcmp(status.track_id, track->id) != 0) {
                ym_aac_close(aac);
                return AUDIO_PLAYER_ERR_TRACK_MISMATCH;
            }
            if (!status.complete) {
                /* См. комментарий в flac-цикле: ждём только сдвиг
                 * счётчика, иначе complete даёт ложный EOF. */
                rc = wait_for_more_data(track->id,
                                        status.downloaded_bytes);
                if (rc == AUDIO_PLAYER_EOF) {
                    *stop_state_ptr = 1;
                    break;
                }
                if (rc != 0) {
                    ym_aac_close(aac);
                    return rc;
                }
                continue;
            }
            if (ym_aac_at_end(aac)) {
                logLine("ap: eof decode terminal (aac) track_id='%s' played_ms=%d have=%d len=%d\n",
                        track->id, (int)((played * 1000) / 44100),
                        (int)status.downloaded_bytes, (int)status.content_length);
                logger_flush();
                *stop_state_ptr = 1;
                break;
            }
            /* Сэмплы целиком на диске, а чтение возвращает 0 — не EOF:
             * несколько повторов, затем ошибка (как в flac-цикле). */
            if (++aac_stuck <= 5) {
                logLine("ap: aac resync retry %d track_id='%s' have=%d\n",
                        aac_stuck, track->id, (int)status.downloaded_bytes);
                continue;
            }
            logLine("ap: aac decode stuck track_id='%s' played_ms=%d -> source error\n",
                    track->id, (int)((played * 1000) / 44100));
            ym_aac_close(aac);
            return AUDIO_PLAYER_ERR_SOURCE;
        }
        aac_stuck = 0;

        if (eq_is_active()) {
            eq_process((short *)s_out_buf[s_out_idx], (int)got, 2, 44100);
        }
        /* Хвост добиваем тишиной до FLAC_DECODE_FRAMES — см. комментарий
         * в flac-цикле: константный размер выхода = один резерв SRC за трек
         * и полный блок на чтение DMA (в т.ч. хвост < 17 кадров). */
        if (got < FLAC_DECODE_FRAMES) {
            memset((unsigned char *)s_out_buf[s_out_idx] + (size_t)got * 4, 0,
                   (size_t)(FLAC_DECODE_FRAMES - (int)got) * 4);
        }
        bytes_decoded = FLAC_DECODE_FRAMES * 4;
        sample_count = bytes_decoded / 4;
        /* PCM декодируется CPU в кэш: выталкиваем ВЕСЬ выходной буфер в RAM
         * до вывода ВСЕГДА, иначе на реальном PSP SRC-DMA читает старые строки
         * кэша (PPSSPP это маскирует) — звук «хрипит». */
        sceKernelDcacheWritebackRange(s_out_buf[s_out_idx], bytes_decoded);

        if (!*audio_reserved_ptr || *reserved_bytes_ptr != bytes_decoded) {
            if (*audio_reserved_ptr) {
                release_audio_src_channel();
                *audio_reserved_ptr = 0;
            }
            if (sample_count < 17 || sample_count > 4111) {
                ym_aac_close(aac);
                return AUDIO_PLAYER_ERR_SOURCE;
            }
            if (sceAudioSRCChReserve(sample_count, 44100, 2) < 0) {
                ym_aac_close(aac);
                return AUDIO_PLAYER_ERR_SOURCE;
            }
            *audio_reserved_ptr = 1;
            *reserved_bytes_ptr = bytes_decoded;
            logLine("ap: sceAudioSRCChReserve ok (aac) track_id='%s' samples=%d\n",
                    track->id, sample_count);
        }

        if (wait_while_paused(track->id) != 0) {
            *stop_state_ptr = 1;
            break;
        }

        out_rc = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX,
                                           (short *)s_out_buf[s_out_idx]);
        if (out_rc < 0) {
            ym_aac_close(aac);
            return out_rc;
        }
        s_out_idx ^= 1;
        ap_note_output(sample_count, "aac");

        played += got;
        *played_ptr = played;
        if (!*first_pcm_done_ptr) {
            *first_pcm_done_ptr = 1;
            audio_report_publish_first_pcm(track->id, worker_generation);
        }
        set_position_snapshot((int)((played * 1000) / 44100));
        if (s_state != AUDIO_PLAYER_PLAYING) {
            set_active_status(AUDIO_PLAYER_PLAYING, track->id);
        }
    }

    ym_aac_close(aac);
    return 0;
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
    /* WAVE-REPORT: per-run audible reporting state. */
    int first_pcm_done = 0;
    unsigned int worker_generation = 0;

    (void)args;
    (void)argp;

    memset(&track, 0, sizeof(track));
    memset(&source, 0, sizeof(source));
    memcpy(&track, &g_playback.current_track, sizeof(track));
    /* WAVE-REPORT: stamp the run with the controller's generation. */
    worker_generation = s_report_generation;

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
    ap_reset_output_stats();
    set_bitrate_snapshot(source.bitrate_kbps);
    {
        /* Codec badge: по расширению кэш-файла: .flac -> FLAC,
         * .mp4 -> FLAC (flac-mp4 без демукса), .aac -> AAC (mp4), всё
         * остальное (диск .mp3 и live RAM stream) декодится как MP3. */
        const char *dot = (!source.use_stream_buf && source.path[0])
                              ? strrchr(source.path, '.')
                              : NULL;
        const char *codec = "mp3";
        if (dot && strcmp(dot, ".flac") == 0) codec = "flac";
        else if (dot && strcmp(dot, ".mp4") == 0) codec = "flac";
        else if (dot && strcmp(dot, ".aac") == 0) codec = "aac";
        set_codec_snapshot(codec);
    }

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

    /* FLAC: файл .flac либо flac-mp4 (декод прямой, без демукса) ->
     * софт-декод dr_flac. AAC: файл .aac (MP4) -> faad2. MP3 идёт
     * старым путём ниже. */
    {
        const char *dot = source.path[0] ? strrchr(source.path, '.') : NULL;
        if (!sbuf && fd >= 0 && dot && strcmp(dot, ".flac") == 0) {
            logLine("ap: flac file mode track_id='%s' path='%s'\n",
                    track.id, source.path);
            error_code = audio_player_flac_file_run(&track, &source, &fd,
                flac_file_avail_cb, (void *)track.id,
                &sampling_rate, &num_channels, &played_samples,
                &audio_reserved, &reserved_bytes,
                &first_pcm_done, worker_generation, &stop_state);
            goto cleanup;
        }
        if (!sbuf && fd >= 0 && dot && strcmp(dot, ".mp4") == 0) {
            logLine("ap: flac file mode track_id='%s' path='%s'\n",
                    track.id, source.path);
            error_code = audio_player_flac_file_run(&track, &source, &fd,
                flac_file_avail_cb, (void *)track.id,
                &sampling_rate, &num_channels, &played_samples,
                &audio_reserved, &reserved_bytes,
                &first_pcm_done, worker_generation, &stop_state);
            goto cleanup;
        }
        if (!sbuf && fd >= 0 && dot && strcmp(dot, ".aac") == 0) {
            logLine("ap: aac file mode track_id='%s' path='%s'\n",
                    track.id, source.path);
            error_code = audio_player_aac_file_run(&track, &source, &fd,
                flac_file_avail_cb, (void *)track.id,
                &sampling_rate, &num_channels, &played_samples,
                &audio_reserved, &reserved_bytes,
                &first_pcm_done, worker_generation, &stop_state);
            goto cleanup;
        }
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
        /* SEEK: consume one queued seek per decode iteration. While
         * paused/buffering the flag stays queued (set by
         * audio_player_seek_to_ms) and applies here once the worker
         * reaches the decode loop; at most one already-decoded frame
         * (~26 ms) may still be output first. */
        if (s_seek_pending) {
            int seek_rc = audio_player_apply_seek(&track, &source, &fd, &handle,
                                                 sbuf, stream_end,
                                                 &sampling_rate, &num_channels,
                                                 &played_samples,
                                                 &audio_reserved, &reserved_bytes);
            if (seek_rc == AUDIO_PLAYER_EOF) {
                logLine("ap: eof from seek track_id='%s'\n", track.id);
                logger_flush();
                stop_state = 1;
                break;
            }
            if (seek_rc != 0) {
                error_code = seek_rc;
                goto cleanup;
            }
            continue;
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

            /* Ping-pong: PCM копируется в буфер, который SRC-DMA сейчас НЕ
             * читает (см. s_out_buf), иначе на реальном PSP следующий вызов
             * sceMp3Decode затирает буфер во время воспроизведения. */
            memcpy(s_out_buf[s_out_idx], pcm_ptr, (size_t)bytes_decoded);
            sceKernelDcacheWritebackRange(s_out_buf[s_out_idx], bytes_decoded);

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
            int out_rc = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX,
                                                   (short *)s_out_buf[s_out_idx]);
            if (out_rc < 0) {
                error_code = out_rc;
                goto cleanup;
            }
            s_out_idx ^= 1;
            ap_note_output(bytes_decoded / (2 * num_channels), "mp3");

            played_samples += bytes_decoded / (2 * num_channels);
            /* WAVE-REPORT: first really-audible block of this run. */
            if (!first_pcm_done) {
                first_pcm_done = 1;
                audio_report_publish_first_pcm(track.id, worker_generation);
            }
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
    /* WAVE-REPORT: terminal snapshot before state cleanup. audible_ms comes
     * from really-output samples only; pause/buffering never inflates it.
     * A stop request reads as MANUAL_STOP here — the controller refines it
     * to NEXT/PREVIOUS/STOP from its own pre-cursor capture. */
    {
        int terminal_audible_ms = (sampling_rate > 0)
            ? (int)((played_samples * 1000) / sampling_rate) : 0;
        AudioEndReason terminal_reason = AUDIO_END_MANUAL_STOP;

        if (error_code != 0 && !s_stop_requested && !stop_state) {
            terminal_reason = AUDIO_END_DECODER_ERROR;
        } else if (!s_stop_requested && stop_state) {
            terminal_reason = AUDIO_END_NATURAL;
        } else if (error_code != 0) {
            terminal_reason = AUDIO_END_DECODER_ERROR;
        }
        if (track.id[0]) {
            audio_report_publish_terminal(track.id, worker_generation,
                                          terminal_reason,
                                          terminal_audible_ms,
                                          first_pcm_done);
        }
    }
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
    /* SEEK: no queued seek across init. */
    s_seek_pending = 0;
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
    /* SEEK: a new track starts with no queued seek. */
    s_seek_pending = 0;
    logLine("ap: start_current accepted track_id='%s'\n", g_playback.current_track.id);
    logger_checkpoint("play start id='%s'", g_playback.current_track.id);
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

/* SEEK: request a time-based seek. Never blocks: only queues a flag consumed
 * by the worker thread, so it is safe to call from the UI thread. See the
 * header doc for return codes and edge-case behavior. */
int audio_player_seek_to_ms(int target_ms)
{
    AudioPlayerState state;

    if (!s_worker_running) {
        return -1;
    }
    if (s_seek_pending) {
        return -1;
    }
    state = (AudioPlayerState)s_state;
    switch (state) {
        case AUDIO_PLAYER_OPENING:
        case AUDIO_PLAYER_PLAYING:
        case AUDIO_PLAYER_PAUSED:
        case AUDIO_PLAYER_BUFFERING:
            break;
        default:
            return -1;
    }
    if (target_ms < 0) {
        target_ms = 0;
    }
    s_seek_target_ms = target_ms;
    s_seek_pending = 1;
    ap_forget_output_timing();
    logLine("ap: seek requested target_ms=%d state=%s\n",
            target_ms, audio_player_state_name(state));
    return 0;
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
    /* SEEK: drop any queued seek so it cannot leak into the next track. */
    s_seek_pending = 0;
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
