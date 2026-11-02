#include "core/io_profile.h"
#include "services/audio_player.h"
#include "services/audio_analysis.h"

#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspaudio.h>
#include <pspkernel.h>

#include <string.h>
#include <stdint.h>

#include "core/logger.h"
#include "services/audio_cache.h"
#include "services/eq.h"
#include "services/ym_flac.h"
#include "services/ym_aac.h"
#include "services/ym_mp3.h"
#include "services/audio_storage.h"
#include "services/net_http.h"
#include "services/net_stack.h"
#include "services/playback_controller.h"
#include "services/resource_policy.h"

#define AUDIO_PLAYER_WORKER_STACK_SIZE 0x10000
#define AUDIO_PLAYER_WORKER_PRIORITY   0x10
#define AUDIO_PLAYER_WAIT_US           (20 * 1000)
#define AUDIO_PLAYER_ERR_STOP          (-1001)
#define AUDIO_PLAYER_ERR_CACHE         YM_MP3_ERR_CACHE
#define AUDIO_PLAYER_ERR_SOURCE        YM_MP3_ERR_SOURCE
#define AUDIO_PLAYER_EOF               YM_MP3_EOF
#define AUDIO_PLAYER_ERR_TIMEOUT       (-1006)
/* Startup watchdog since the last cache progress; suspended during
 * network recovery. Playback data waits belong to audio_reader. */
#define AUDIO_PLAYER_WAIT_DEADLINE_US  (45ULL * 1000000ULL)
/* Выходной PCM-буфер для блоков FLAC до 4096 кадров. */
#define AUDIO_PLAYER_PCM_BUF_SIZE      ((4096 + 32) * 4)

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

static AudioPlayerSnapshot s_status;
static SceLwMutexWorkarea s_status_mutex;
static int s_mutex_initialized = 0;

static SceUID s_worker_thread = -1;
static volatile int s_worker_running = 0;
static volatile int s_stop_requested = 0;
static volatile int s_pause_requested = 0;
static AudioPlayerState s_phase = AUDIO_PLAYER_IDLE;
static int s_backend_ready = 0;
static int s_backend_error = 0;
/* Ping-pong выходные буферы. sceAudioSRCOutputBlocking блокируется до конца
 * предыдущего буфера и отдаёт текущий в очередь: SRC-DMA читает его
 * асинхронно следующие ~93 мс, пока декодер готовит следующий блок. Один
 * общий буфер = перезапись во время чтения DMA → порча начала каждого блока
 * на реальном PSP (периодический треск); PPSSPP копирует контент в момент
 * постановки в очередь и это маскирует. Два буфера: HW читает один, CPU
 * декодирует в другой. */
static unsigned char s_out_buf[2][AUDIO_PLAYER_PCM_BUF_SIZE] __attribute__((aligned(64)));
static int s_out_idx = 0;

/* UI публикует запрос перемотки; worker потребляет его перед декодированием. */
static volatile int s_seek_pending = 0;
static volatile int s_seek_target_ms = 0;

static int release_audio_src_channel(void)
{
    int rc;
    int last_error = 0;

    logger_checkpoint("ap: src release begin");
    do {
        rc = sceAudioSRCChRelease();
        if (rc < 0) {
            if (rc != last_error) {
                logger_checkpoint("ap: src release waiting rc=0x%08X", (unsigned int)rc);
                last_error = rc;
            }
            sceKernelDelayThread(100);
        }
    } while (rc < 0);

    logger_checkpoint("ap: src release done rc=0x%08X", (unsigned int)rc);
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
    s_status.bits_per_sample = 0;
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

static void set_format_snapshot(int sample_rate, int channels, int bits_per_sample)
{
    if (s_mutex_initialized) {
        sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    }
    s_status.sample_rate = sample_rate;
    s_status.channels = channels;
    s_status.bits_per_sample = bits_per_sample;
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

typedef struct {
    AudioPlayerState previous;
    AudioPlayerState state;
    int error_code;
    int position_ms;
    int changed;
    unsigned int sequence;
    char track_id[sizeof(s_status.track_id)];
} AudioStateEvent;

static unsigned int s_state_sequence;
static struct { u64 total_us, since_us; int active; } s_listening;

/* Caller holds s_status_mutex. Position changes never alter elapsed listening. */
static void listening_update_locked(int playing)
{
    u64 now = sceKernelGetSystemTimeWide();
    if (s_listening.active) s_listening.total_us += now - s_listening.since_us;
    s_listening.since_us = now;
    s_listening.active = playing;
    s_status.total_played_ms = (int)(s_listening.total_us / 1000);
}

static int is_live_phase(AudioPlayerState state)
{
    return state == AUDIO_PLAYER_OPENING || state == AUDIO_PLAYER_PLAYING ||
           state == AUDIO_PLAYER_BUFFERING;
}

/* Caller holds s_status_mutex, or initializes the player before any worker exists. */
static void publish_status_locked(AudioPlayerState state, const char *track_id,
                                  int error_code, AudioStateEvent *event)
{
    event->previous = s_status.state;
    listening_update_locked(state == AUDIO_PLAYER_PLAYING);
    event->state = state;
    event->error_code = error_code;
    event->changed = s_status.state != state || error_code != 0;
    s_status.state = state;
    s_status.error_code = error_code;
    if (track_id) {
        if (track_id != s_status.track_id)
            strncpy(s_status.track_id, track_id, sizeof(s_status.track_id) - 1);
        s_status.track_id[sizeof(s_status.track_id) - 1] = '\0';
    } else {
        s_status.track_id[0] = '\0';
    }

    event->position_ms = s_status.position_ms;
    memcpy(event->track_id, s_status.track_id, sizeof(event->track_id));
    if (event->changed) ++s_state_sequence;
    event->sequence = s_state_sequence;
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

}

static void emit_status(const AudioStateEvent *event)
{
    if (!event->changed) return;
    logLine("ap: state -> %s track_id='%s' err=0x%08X seq=%u\n",
            audio_player_state_name(event->state), event->track_id,
            event->error_code, event->sequence);
    if (event->previous == AUDIO_PLAYER_OPENING && event->state == AUDIO_PLAYER_PLAYING)
        logger_flush();
}

static void set_status(AudioPlayerState state, const char *track_id, int error_code)
{
    AudioStateEvent event;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    s_phase = state;
    publish_status_locked(state, track_id, error_code, &event);
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    emit_status(&event);
}

static void set_active_status(AudioPlayerState state, const char *track_id)
{
    AudioStateEvent event;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (!is_live_phase(s_phase)) {
        if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
        return;
    }
    s_phase = state;
    publish_status_locked(s_stop_requested ? AUDIO_PLAYER_STOPPING :
                          s_pause_requested ? AUDIO_PLAYER_PAUSED : state,
                          track_id, 0, &event);
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    emit_status(&event);
}

static void request_stop(void)
{
    AudioStateEvent event;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    s_stop_requested = 1;
    s_pause_requested = 0;
    s_seek_pending = 0;
    publish_status_locked(is_live_phase(s_status.state) || s_status.state == AUDIO_PLAYER_PAUSED
                              ? AUDIO_PLAYER_STOPPING : s_status.state,
                          s_status.track_id, s_status.error_code, &event);
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    emit_status(&event);
    audio_reader_cancel();
}

static void publish_worker_result(const char *track_id, int close_error,
                                  int error_code)
{
    AudioStateEvent event;
    AudioPlayerState state;
    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (close_error < 0) {
        state = AUDIO_PLAYER_ERROR;
        error_code = close_error;
    } else if (s_stop_requested) {
        state = AUDIO_PLAYER_STOPPED;
        error_code = 0;
    } else if (error_code != 0) {
        state = AUDIO_PLAYER_ERROR;
    } else {
        state = AUDIO_PLAYER_FINISHED;
    }
    s_phase = state;
    publish_status_locked(state, track_id, error_code, &event);
    sceKernelUnlockLwMutex(&s_status_mutex, 1);
    emit_status(&event);
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
static int s_terminal_position_ms;
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
                                          int had_first_pcm, int position_ms)
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
    s_terminal_position_ms = position_ms;
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
                                        int *out_had_first_pcm,
                                        int *out_position_ms)
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
        if (out_position_ms) *out_position_ms = s_terminal_position_ms;
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

static void ap_note_output(int frames, int rate, const char *tag)
{
    u64 now = sceKernelGetSystemTimeWide();
    u64 budget = (u64)frames * 1000000ULL / (unsigned)rate;
    io_profile_event(IP_AUDIO_SUBMIT, frames, rate, now, now);

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

static int wait_while_paused(void)
{
    if (!s_pause_requested) {
        return 0;
    }

    while (s_pause_requested && !s_stop_requested) {
        sceKernelDelayThread(AUDIO_PLAYER_WAIT_US);
    }
    ap_forget_output_timing();
    return s_stop_requested ? AUDIO_PLAYER_ERR_STOP : 0;
}

static int reap_worker_thread(void)
{
    if (s_worker_thread >= 0 && !s_worker_running) {
        int rc = sceKernelWaitThreadEnd(s_worker_thread, NULL);
        if (rc < 0) return rc;
        rc = sceKernelDeleteThread(s_worker_thread);
        if (rc < 0) return rc;
        s_worker_thread = -1;
    }
    return 0;
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

static int ensure_backend_ready(void)
{
    if (s_backend_ready) return 0;
    s_backend_error = audio_reader_init();
    if (s_backend_error < 0) return s_backend_error;
    s_backend_error = ym_mp3_init();
    if (s_backend_error < 0) return s_backend_error;
    s_backend_ready = 1;
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
                if (audio_cache_get_source(track_id, source) == 0) {
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

static void reader_buffering(void *user)
{
    const char *track_id = user;
    AudioPlayerState phase;
    ap_forget_output_timing();
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    phase = s_phase;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    if (phase == AUDIO_PLAYER_PLAYING || phase == AUDIO_PLAYER_BUFFERING)
        set_active_status(AUDIO_PLAYER_BUFFERING, track_id);
}

/* Блок FLAC сохраняет текущую длительность SRC-буфера (~93 мс). */
#define FLAC_DECODE_FRAMES 4096

static int audio_player_worker(SceSize args, void *argp)
{
    IoProfileLane profile;
    io_profile_begin(&profile, IP_AUDIO);
    TrackEntry track;
    AudioSourceSnapshot source;
    enum { DECODE_MP3, DECODE_FLAC, DECODE_AAC } decoder = DECODE_MP3;
    const char *codec = "mp3";
    YmFlac *flac = NULL;
    YmAac *aac = NULL;
    YmMp3Format mp3_fmt = {0};
    AudioReader *reader = NULL;
    int audio_reserved = 0, reserved_bytes = 0;
    int sampling_rate = 0, num_channels = 0, bits_per_sample = 0;
    int error_code = 0, stop_state = 0;
    int64_t played_samples = 0;
    int first_pcm_done = 0;
    unsigned int worker_generation = s_report_generation;
    const char *dot;
    char complete_path[sizeof(source.path)];

    (void)args;
    (void)argp;
    memset(&track, 0, sizeof(track));
    memset(&source, 0, sizeof(source));
    memcpy(&track, &g_playback.current_track, sizeof(track));
    if (!track.id[0]) {
        error_code = AUDIO_PLAYER_ERR_SOURCE;
        goto cleanup;
    }
    set_active_status(AUDIO_PLAYER_OPENING, track.id);
    error_code = wait_for_source(track.id, &source);
    if (error_code != 0) goto cleanup;
    reader = audio_reader_open(&source, reader_buffering, track.id);
    if (!reader || s_stop_requested) {
        error_code = AUDIO_PLAYER_ERR_SOURCE;
        goto cleanup;
    }
    ap_reset_output_stats();
    set_bitrate_snapshot(source.bitrate_kbps);
    if (audio_storage_complete_path(source.path, complete_path,
                                    sizeof(complete_path)) != 0) {
        error_code = AUDIO_PLAYER_ERR_SOURCE;
        goto cleanup;
    }
    dot = strrchr(complete_path, '.');
    if (dot && (strcmp(dot, ".flac") == 0 || strcmp(dot, ".mp4") == 0)) {
        decoder = DECODE_FLAC;
        codec = "flac";
    } else if (dot && strcmp(dot, ".aac") == 0) {
        decoder = DECODE_AAC;
        codec = "aac";
    }
    set_codec_snapshot(codec);
    if (!s_backend_ready) {
        error_code = s_backend_error != 0 ? s_backend_error : AUDIO_PLAYER_ERR_SOURCE;
        goto cleanup;
    }
    if (decoder == DECODE_FLAC) {
        YmFlacFormat fmt = {0};
        flac = strcmp(dot, ".mp4") == 0
            ? ym_flac_open_mp4(reader)
            : ym_flac_open_file(reader);
        if (!flac) {
            error_code = AUDIO_PLAYER_ERR_SOURCE;
            goto cleanup;
        }
        if (ym_flac_format(flac, &fmt) != 0 || fmt.sample_rate != 44100 ||
            fmt.channels != 2 || fmt.bits_per_sample < 16 || fmt.bits_per_sample > 32) {
            logLine("ap: flac format rejected track_id='%s' %d/%d/%d\n",
                    track.id, fmt.sample_rate, fmt.channels, fmt.bits_per_sample);
            error_code = AUDIO_PLAYER_ERR_SOURCE;
            goto cleanup;
        }
        sampling_rate = fmt.sample_rate;
        num_channels = fmt.channels;
        bits_per_sample = fmt.bits_per_sample;
    } else if (decoder == DECODE_AAC) {
        YmAacFormat fmt = {0};
        aac = ym_aac_open_file(reader);
        if (!aac || ym_aac_format(aac, &fmt) != 0 ||
            fmt.sample_rate <= 0 || fmt.channels != 2) {
            error_code = AUDIO_PLAYER_ERR_SOURCE;
            goto cleanup;
        }
        sampling_rate = fmt.sample_rate;
        num_channels = fmt.channels;
    } else {
        eq_reset();
        error_code = ym_mp3_open(reader, &mp3_fmt);
        if (mp3_fmt.bitrate_kbps > 0) set_bitrate_snapshot(mp3_fmt.bitrate_kbps);
        if (error_code < 0) goto cleanup;
        error_code = 0;
        sampling_rate = mp3_fmt.sample_rate;
        num_channels = mp3_fmt.channels;
    }
    set_format_snapshot(sampling_rate, num_channels, bits_per_sample);
    if (decoder != DECODE_MP3) {
        set_bitrate_snapshot(source.bitrate_kbps > 0 ? source.bitrate_kbps
            : (track.duration_ms > 0 && source.content_length > 0
                ? (int)(source.content_length * 8 / track.duration_ms) : 0));
        eq_reset();
    }
    logLine("ap: %s format ok track_id='%s' rate=%d channels=%d\n",
            codec, track.id, sampling_rate, num_channels);
    set_active_status(AUDIO_PLAYER_PLAYING, track.id);

    while (!s_stop_requested) {
        io_profile_phase(&profile, IP_OTHER);
        short *pcm = (short *)s_out_buf[s_out_idx];
        int frames, output_frames, bytes_decoded;
        int rc;
        io_profile_phase(&profile, IP_WAIT);
        if (wait_while_paused() != 0) {
            stop_state = 1;
            break;
        }
        io_profile_phase(&profile, IP_OTHER);
        if (s_seek_pending) {
            set_active_status(AUDIO_PLAYER_BUFFERING, track.id);
            int target_ms = s_seek_target_ms;
            ap_forget_output_timing();
            int64_t target_frame = (int64_t)target_ms * sampling_rate / 1000;
            if (track.duration_ms > 0 && target_ms >= track.duration_ms) {
                stop_state = 1;
                break;
            }
            if (audio_reserved) {
                release_audio_src_channel();
                audio_reserved = 0;
                reserved_bytes = 0;
            }
            if (decoder == DECODE_MP3) {
                int position_ms;
                rc = ym_mp3_seek(target_ms, track.duration_ms, &mp3_fmt, &position_ms);
                if (position_ms >= 0) {
                    sampling_rate = mp3_fmt.sample_rate;
                    num_channels = mp3_fmt.channels;
                    target_frame = (int64_t)target_ms * sampling_rate / 1000;
                    set_format_snapshot(sampling_rate, num_channels, 0);
                    played_samples = target_frame;
                    set_position_snapshot(position_ms);
                }
            } else {
                rc = flac ? ym_flac_seek_frame(flac, target_frame)
                          : ym_aac_seek_frame(aac, target_frame);
            }
            if (rc == AUDIO_PLAYER_EOF) {
                stop_state = 1;
                break;
            }
            if (rc != 0) {
                error_code = rc;
                goto cleanup;
            }
            if (decoder != DECODE_MP3) {
                played_samples = target_frame;
                set_position_snapshot(target_ms);
            }
            eq_reset();
            sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
            s_seek_pending = 0;
            sceKernelUnlockLwMutex(&s_status_mutex, 1);
            logLine("ap: %s seek ok track_id='%s' target_ms=%d\n", codec, track.id, target_ms);
            continue;
        }

        io_profile_phase(&profile, IP_DECODE);
        if (decoder == DECODE_MP3) {
            rc = ym_mp3_read_s16(&pcm);
            if (rc == AUDIO_PLAYER_EOF) {
                stop_state = 1;
                break;
            }
            if (rc < 0) {
                error_code = rc;
                goto cleanup;
            }
            frames = rc / (2 * num_channels);
        } else {
            frames = flac ? (int)ym_flac_read_s16(flac, pcm, FLAC_DECODE_FRAMES)
                          : ym_aac_read_s16(aac, pcm);
            if (frames < 0) {
                error_code = AUDIO_PLAYER_ERR_SOURCE;
                goto cleanup;
            }
            if (frames == 0) {
                if (flac ? ym_flac_at_end(flac) : ym_aac_at_end(aac)) {
                    stop_state = 1;
                    break;
                }
                error_code = AUDIO_PLAYER_ERR_SOURCE;
                goto cleanup;
            }
        }

        io_profile_phase(&profile, IP_PROCESS);
        if (eq_is_active()) {
            eq_process(pcm, frames, num_channels, sampling_rate);
            if (decoder == DECODE_MP3) {
                sceKernelDcacheWritebackInvalidateRange(pcm, frames * 2 * num_channels);
            }
        }
        audio_analysis_feed_pcm(pcm, frames, num_channels, sampling_rate);
        output_frames = flac ? FLAC_DECODE_FRAMES : frames;
        bytes_decoded = output_frames * 2 * num_channels;
        if (decoder == DECODE_MP3) {
            memcpy(s_out_buf[s_out_idx], pcm, (size_t)bytes_decoded);
        } else if (flac && frames < output_frames) {
            /* Сохраняем полный блок SRC для короткого хвоста FLAC. */
            memset(pcm + frames * num_channels, 0,
                   (size_t)(output_frames - frames) * 2 * num_channels);
        }
        sceKernelDcacheWritebackRange(s_out_buf[s_out_idx], bytes_decoded);
        if (!audio_reserved || reserved_bytes != bytes_decoded) {
            if (audio_reserved) {
                release_audio_src_channel();
                audio_reserved = 0;
            }
            if (output_frames < 17 || output_frames > 4111) {
                error_code = AUDIO_PLAYER_ERR_SOURCE;
                goto cleanup;
            }
            rc = sceAudioSRCChReserve(output_frames, sampling_rate, num_channels);
            if (rc < 0) {
                error_code = decoder == DECODE_MP3 ? rc : AUDIO_PLAYER_ERR_SOURCE;
                goto cleanup;
            }
            audio_reserved = 1;
            reserved_bytes = bytes_decoded;
            logLine("ap: sceAudioSRCChReserve ok (%s) track_id='%s' samples=%d rate=%d channels=%d\n",
                    codec, track.id, output_frames, sampling_rate, num_channels);
        }
        io_profile_phase(&profile, IP_WAIT);
        if (wait_while_paused() != 0) {
            stop_state = 1;
            break;
        }
        io_profile_phase(&profile, IP_OUTPUT);
        rc = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, (short *)s_out_buf[s_out_idx]);
        if (rc < 0) {
            error_code = rc;
            goto cleanup;
        }
        io_profile_phase(&profile, IP_OTHER);
        s_out_idx ^= 1;
        ap_note_output(output_frames, sampling_rate, codec);
        played_samples += frames;
        if (!first_pcm_done) {
            first_pcm_done = 1;
            audio_report_publish_first_pcm(track.id, worker_generation);
        }
        set_position_snapshot((int)(played_samples * 1000 / sampling_rate));
        if (audio_player_get_state() != AUDIO_PLAYER_PLAYING)
            set_active_status(AUDIO_PLAYER_PLAYING, track.id);
    }
    if (s_stop_requested) stop_state = 1;

cleanup:
    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    listening_update_locked(0);
    s_seek_pending = 0;
    sceKernelUnlockLwMutex(&s_status_mutex, 1);
    io_profile_phase(&profile, IP_OTHER);
    logger_checkpoint("ap: cleanup begin track='%s' stop=%d pause=%d decoder=%d src=%d",
                      track.id, s_stop_requested, s_pause_requested,
                      decoder, audio_reserved);
    if (flac) ym_flac_close(flac);
    if (aac) ym_aac_close(aac);
    if (decoder == DECODE_MP3) ym_mp3_close();
    logger_checkpoint("ap: decoder cleanup done track='%s'", track.id);
    /* Capture elapsed listening and position before stop resets the UI position. */
    {
        AudioPlayerSnapshot terminal;
        audio_player_get_snapshot(&terminal);
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
                                          terminal.total_played_ms,
                                          first_pcm_done, terminal.position_ms);
        }
    }
    if (s_stop_requested && audio_player_get_state() != AUDIO_PLAYER_STOPPING) {
        set_active_status(AUDIO_PLAYER_STOPPING, track.id[0] ? track.id : NULL);
    }

    if (audio_reserved) {
        release_audio_src_channel();
    }
    int close_error;
    {
        int close_rc = audio_reader_close();
        close_error = close_rc;
        if (close_rc < 0) {
            error_code = close_rc;
            stop_state = 0;
            logLine("ap: reader cleanup retained track='%s' rc=0x%08X\n",
                    track.id, close_rc);
        }
    }
    if (close_error >= 0 && (s_stop_requested || stop_state)) {
        logLine("ap: cleanup stop track_id='%s' stop_requested=%d stop_state=%d position_ms=%d error=0x%08X\n",
                track.id[0] ? track.id : "",
                s_stop_requested,
                stop_state,
                sampling_rate > 0 ? (int)((played_samples * 1000) / sampling_rate) : 0,
                error_code);
        logger_flush();
    } else if (error_code != 0) {
        logLine("ap: worker error track_id='%s' rc=0x%08X\n",
                track.id, error_code);
        logger_flush();
    }
    publish_worker_result(track.id[0] ? track.id : NULL, close_error, error_code);

    io_profile_end(&profile);
    logger_checkpoint("ap: worker exit track='%s'", track.id);
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    s_worker_running = 0;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    return 0;
}

void audio_player_init(void)
{
    int backend_rc;
    int sync_rc = 0;

    if (!s_mutex_initialized) {
        memset(&s_status_mutex, 0, sizeof(s_status_mutex));
        sync_rc = sceKernelCreateLwMutex(&s_status_mutex, "audio_player_status", 0, 0, NULL);
        if (sync_rc >= 0) {
            s_mutex_initialized = 1;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(&s_listening, 0, sizeof(s_listening));
    s_worker_thread = -1;
    s_worker_running = 0;
    s_stop_requested = 0;
    s_pause_requested = 0;
    s_phase = AUDIO_PLAYER_IDLE;
    s_state_sequence = 0;
    s_backend_ready = 0;
    /* SEEK: no queued seek across init. */
    s_seek_pending = 0;
    s_backend_error = 0;

    if (!s_mutex_initialized) {
        s_backend_error = sync_rc;
        set_status(AUDIO_PLAYER_ERROR, NULL, sync_rc);
        logLine("ap: status mutex create failed rc=0x%08X\n", (unsigned int)sync_rc);
        return;
    }

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
    if (s_worker_running || s_worker_thread >= 0 || audio_reader_close() < 0) return;
    ym_mp3_shutdown();
    s_backend_ready = 0;
    set_status(AUDIO_PLAYER_IDLE, NULL, 0);
    logLine("ap: shutdown\n");
}

int audio_player_start_current(void)
{
    AudioPlayerStatus status;
    int rc;

    rc = reap_worker_thread();
    if (rc < 0) return rc;
    audio_player_get_status(&status);

    if (g_playback.current_track.id[0] == '\0') {
        return -1;
    }

    if (!s_backend_ready) {
        rc = s_backend_error != 0 ? s_backend_error : AUDIO_PLAYER_ERR_SOURCE;
        set_status(AUDIO_PLAYER_ERROR, g_playback.current_track.id, rc);
        return rc;
    }

    if (s_worker_running && strcmp(status.track_id, g_playback.current_track.id) == 0) {
        return 0;
    }

    if (s_worker_running) {
        audio_player_stop();
        if (s_worker_running) return AUDIO_PLAYER_ERR_STOP;
        rc = reap_worker_thread();
        if (rc < 0) return rc;
    }

    {
        rc = audio_reader_close();
        if (rc < 0) {
            set_status(AUDIO_PLAYER_ERROR, g_playback.current_track.id, rc);
            return rc;
        }
    }

    set_track_snapshot(&g_playback.current_track);
    AudioStateEvent event;
    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    s_stop_requested = 0;
    s_pause_requested = 0;
    s_phase = AUDIO_PLAYER_OPENING;
    s_seek_pending = 0;
    memset(&s_listening, 0, sizeof(s_listening));
    publish_status_locked(AUDIO_PLAYER_OPENING, g_playback.current_track.id, 0, &event);
    sceKernelUnlockLwMutex(&s_status_mutex, 1);
    logLine("ap: start_current accepted track_id='%s'\n", g_playback.current_track.id);
    emit_status(&event);

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

    sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    s_worker_running = 1;
    sceKernelUnlockLwMutex(&s_status_mutex, 1);
    {
        int start_rc = sceKernelStartThread(s_worker_thread, 0, NULL);
        if (start_rc < 0) {
            rc = start_rc;
            sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
            s_worker_running = 0;
            sceKernelUnlockLwMutex(&s_status_mutex, 1);
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
    if (s_worker_thread < 0) return audio_reader_close();
    request_stop();
    {
        SceUInt timeout_us = 20000000U;
        int rc = sceKernelWaitThreadEnd(s_worker_thread, &timeout_us);
        if (rc < 0) {
            set_status(AUDIO_PLAYER_ERROR, s_status.track_id, rc);
            logLine("ap: worker still active; resources retained\n");
            return -1;
        }
    }
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    s_worker_running = 0;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    int rc = audio_reader_close();
    set_status(rc < 0 ? AUDIO_PLAYER_ERROR : AUDIO_PLAYER_STOPPED, s_status.track_id, rc < 0 ? rc : 0);
    return rc;
}

void audio_player_pause(void)
{
    AudioStateEvent event;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (!s_worker_running || s_stop_requested || s_pause_requested || !is_live_phase(s_status.state)) {
        if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
        return;
    }
    s_pause_requested = 1;
    publish_status_locked(AUDIO_PLAYER_PAUSED, s_status.track_id, 0, &event);
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    emit_status(&event);
    logLine("ap: pause track_id='%s' position_ms=%d\n",
            event.track_id, event.position_ms);
}

void audio_player_resume(void)
{
    AudioStateEvent event;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (!s_worker_running || s_stop_requested || !s_pause_requested ||
        s_status.state != AUDIO_PLAYER_PAUSED) {
        if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
        return;
    }
    s_pause_requested = 0;
    publish_status_locked(s_phase, s_status.track_id, 0, &event);
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    emit_status(&event);
    logLine("ap: resume track_id='%s' position_ms=%d\n",
            event.track_id, event.position_ms);
}

/* SEEK: request a time-based seek. Never blocks: only queues a flag consumed
 * by the worker thread, so it is safe to call from the UI thread. See the
 * header doc for return codes and edge-case behavior. */
int audio_player_seek_to_ms(int target_ms)
{
    AudioPlayerState state;

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    if (!s_worker_running || s_stop_requested || s_seek_pending) {
        if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
        return -1;
    }
    state = s_status.state;
    switch (state) {
        case AUDIO_PLAYER_OPENING:
        case AUDIO_PLAYER_PLAYING:
        case AUDIO_PLAYER_PAUSED:
        case AUDIO_PLAYER_BUFFERING:
            break;
        default:
            if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
            return -1;
    }
    if (target_ms < 0) {
        target_ms = 0;
    }
    s_seek_target_ms = target_ms;
    s_seek_pending = 1;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    logLine("ap: seek requested target_ms=%d state=%s\n",
            target_ms, audio_player_state_name(state));
    return 0;
}

void audio_player_stop(void)
{
    if (!s_worker_running && s_worker_thread < 0) {
        int rc = audio_reader_close();
        if (rc < 0) {
            set_status(AUDIO_PLAYER_ERROR, s_status.track_id, rc);
            return;
        }
        if (audio_player_get_state() != AUDIO_PLAYER_IDLE) {
            set_position_snapshot(0);
            set_status(AUDIO_PLAYER_STOPPED,
                       s_status.track_id[0] ? s_status.track_id : NULL, 0);
        }
        return;
    }

    logLine("ap: stop requested track_id='%s'\n",
            s_status.track_id[0] ? s_status.track_id : "");
    request_stop();

    if (s_worker_thread >= 0) {
        int join_rc, delete_rc;
        logger_checkpoint("ap: stop join begin tid=%d track='%s'",
                          s_worker_thread, s_status.track_id);
        join_rc = sceKernelWaitThreadEnd(s_worker_thread, NULL);
        logger_checkpoint("ap: stop join done tid=%d rc=0x%08X",
                          s_worker_thread, (unsigned int)join_rc);
        if (join_rc < 0) {
            set_status(AUDIO_PLAYER_ERROR, s_status.track_id, join_rc);
            return;
        }
        delete_rc = sceKernelDeleteThread(s_worker_thread);
        logger_checkpoint("ap: stop delete done tid=%d rc=0x%08X",
                          s_worker_thread, (unsigned int)delete_rc);
        if (delete_rc < 0) {
            set_status(AUDIO_PLAYER_ERROR, s_status.track_id, delete_rc);
            return;
        }
        s_worker_thread = -1;
    }

    {
        int rc = audio_reader_close();
        if (rc < 0) {
            set_status(AUDIO_PLAYER_ERROR, s_status.track_id, rc);
            return;
        }
    }

    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    s_worker_running = 0;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    set_position_snapshot(0);
    set_status(AUDIO_PLAYER_STOPPED, s_status.track_id[0] ? s_status.track_id : NULL, 0);
}

AudioPlayerState audio_player_get_state(void)
{
    AudioPlayerState state;
    if (s_mutex_initialized) sceKernelLockLwMutex(&s_status_mutex, 1, NULL);
    state = s_status.state;
    if (s_mutex_initialized) sceKernelUnlockLwMutex(&s_status_mutex, 1);
    return state;
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
    }
    listening_update_locked(s_listening.active);
    *out = s_status;
    out->seek_pending = s_seek_pending;
    out->seek_target_ms = s_seek_target_ms;
    if (s_mutex_initialized) {
        sceKernelUnlockLwMutex(&s_status_mutex, 1);
    }

    return true;
}
