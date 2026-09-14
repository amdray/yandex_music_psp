#ifndef YM_AUDIO_PLAYER_H
#define YM_AUDIO_PLAYER_H

#include <stdbool.h>

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
AudioPlayerState audio_player_get_state(void);
bool             audio_player_get_snapshot(AudioPlayerSnapshot *out);
bool             audio_player_get_status(AudioPlayerStatus *out);

#endif /* YM_AUDIO_PLAYER_H */
