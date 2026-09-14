#ifndef YM_AUDIO_PLAYER_H
#define YM_AUDIO_PLAYER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    AUDIO_PLAYER_IDLE = 0,
    AUDIO_PLAYER_OPENING = 1,
    AUDIO_PLAYER_PLAYING = 2,
    AUDIO_PLAYER_PAUSED = 3,
    AUDIO_PLAYER_BUFFERING = 4,
    AUDIO_PLAYER_STOPPING = 5,
    AUDIO_PLAYER_STOPPED = 6,
    AUDIO_PLAYER_FINISHED = 7,
    AUDIO_PLAYER_ERROR = 8
} AudioPlayerState;

typedef struct {
    AudioPlayerState state;
    int              position_ms;
    int              duration_ms;
    int              error_code;
    int              sample_rate;
    int              channels;
    int              bitrate_kbps;
    char             track_id[40];
    char             title[128];
    char             artist[256];
} AudioPlayerSnapshot;

typedef AudioPlayerSnapshot AudioPlayerStatus;

void             audio_player_init(void);
void             audio_player_shutdown(void);
int              audio_player_quiesce(void);
int              audio_player_start_current(void);
void             audio_player_pause(void);
void             audio_player_resume(void);
void             audio_player_stop(void);
/* Time-based seek for CBR MP3 streams (read cursor inside the current
 * source; no cache job restart, no new API surface for position).
 * Non-blocking and UI-thread safe: queues at most one pending request,
 * consumed by the worker thread on its next decode iteration.
 * Returns 0 if the seek was queued, -1 when idle (no worker running, or
 * state IDLE/STOPPED/STOPPING/FINISHED/ERROR) or a seek is already pending.
 * target_ms is clamped to >= 0 worker-side; target at/past duration_ms
 * finishes the track like a natural EOF (FINISHED, queue auto-advance
 * applies). While OPENING/PAUSED/BUFFERING the request stays queued and
 * applies once the worker reaches the decode loop. Byte mapping is
 * total_bytes * target_ms / duration_ms (CBR approximation: on VBR the real
 * position drifts by the local bitrate ratio). Position source of truth is
 * unchanged (played_samples/sampling_rate). */
int              audio_player_seek_to_ms(int target_ms);
AudioPlayerState audio_player_get_state(void);
bool             audio_player_get_snapshot(AudioPlayerSnapshot *out);
bool             audio_player_get_status(AudioPlayerStatus *out);

/* WAVE-REPORT: really-audible reporting (spec section 6). The worker
 * publishes FIRST_PCM once per run after the first successful
 * sceAudioSRCOutputBlocking, plus a terminal snapshot (reason + audible_ms
 * from really-output samples, never wall-clock) before cleanup. No malloc,
 * no HTTP/JSON on the audio thread; the controller consumes both exactly
 * once via the take_ functions (0 = event present, -1 = none). */
typedef enum {
    AUDIO_END_NATURAL = 0,
    AUDIO_END_MANUAL_NEXT,
    AUDIO_END_MANUAL_PREVIOUS,
    AUDIO_END_MANUAL_STOP,
    AUDIO_END_DECODER_ERROR,
    AUDIO_END_NETWORK_RETRY
} AudioEndReason;

void audio_player_set_report_generation(unsigned int generation);
int audio_player_take_first_pcm_event(char *out_track_id,
                                      size_t track_id_size,
                                      unsigned int *out_generation);
int audio_player_take_terminal_snapshot(char *out_track_id,
                                        size_t track_id_size,
                                        unsigned int *out_generation,
                                        AudioEndReason *out_reason,
                                        int *out_audible_ms,
                                        int *out_had_first_pcm);

#endif /* YM_AUDIO_PLAYER_H */
