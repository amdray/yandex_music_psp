#include "ui/ui_screen_now_playing.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "hal/hal_fb.h"
#include "hal/hal_gpu.h"
#include "services/locale.h"
#include "services/eq.h"
#include "services/token_loader.h"
#include "services/wave.h"
#include "services/ym_api.h"
#include "services/ym_api_like.h"
#include "services/audio_cache.h"
#include "services/audio_player.h"
#include "services/cover_now_playing.h"
#include "services/playback_controller.h"
#include "services/video_player.h"
#include "services/video_cover.h"
#include "services/system_status.h"
#include "core/logger.h"
#include "app/app_state.h"
#include "fonts/text.h"
#include <pspctrl.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Cover video: progressive enhancement over the static cover ---------------
 * Plays the proxy-produced MP4 through the ME. Any failure falls back to the
 * static cover; video can never break now-playing. */
static VideoPlayer *s_np_video       = NULL;
static char         s_np_video_track[40] = {0};
static int          s_np_video_tried = 0;   /* opened (or failed) for this track */
static const void  *s_np_frame       = NULL;
static int          s_np_fw, s_np_fh, s_np_tex;
static u64          s_np_last_us      = 0;
static char         s_marquee_track[40] = {0};
static u64          s_marquee_start_us = 0;

#define MARQUEE_HOLD_US 1200000ULL
#define MARQUEE_SPEED_PX_PER_SEC 24ULL
#define MARQUEE_GAP_PX 28.0f

static float marquee_offset(float content_width, float viewport_width, u64 now_us)
{
    u64 travel_us;
    u64 cycle_us;
    u64 phase_us;

    if (content_width <= viewport_width) {
        return 0.0f;
    }

    travel_us = (u64)(((content_width + MARQUEE_GAP_PX) * 1000000.0f) /
                      (float)MARQUEE_SPEED_PX_PER_SEC);
    cycle_us = MARQUEE_HOLD_US + travel_us;
    phase_us = (now_us - s_marquee_start_us) % cycle_us;
    if (phase_us < MARQUEE_HOLD_US) {
        return 0.0f;
    }
    return (float)(((phase_us - MARQUEE_HOLD_US) * MARQUEE_SPEED_PX_PER_SEC) /
                   1000000ULL);
}

/* Draw one scrolling metadata line. `suffix` may use a quieter color while
 * remaining part of the same marquee as `text`. */
static void draw_marquee_parts(float x, float y, float max_width,
                                const char *text, u32 text_color,
                                const char *suffix, u32 suffix_color,
                                u64 now_us)
{
    float text_width;
    float separator_width = 0.0f;
    float suffix_width = 0.0f;
    float content_width;
    float offset;
    float first_x;
    int copies;
    int i;

    if (!text || !text[0] || max_width <= 0.0f) {
        return;
    }

    text_width = text_measure_width(text);
    if (suffix && suffix[0]) {
        separator_width = text_measure_width(" ");
        suffix_width = text_measure_width(suffix);
    }
    content_width = text_width + separator_width + suffix_width;
    if (content_width <= max_width) {
        ui_draw_text(x, y, text, text_color);
        if (suffix_width > 0.0f) {
            ui_draw_text(x + text_width + separator_width, y,
                         suffix, suffix_color);
        }
        return;
    }

    offset = marquee_offset(content_width, max_width, now_us);
    first_x = x - offset;
    hal_gpu_set_scissor((int)x, 0, (int)(max_width + 0.999f), 272);
    copies = offset > 0.0f ? 2 : 1;
    for (i = 0; i < copies; ++i) {
        float copy_x = first_x + (float)i * (content_width + MARQUEE_GAP_PX);
        float raw_skip = x - copy_x;
        float text_skip = raw_skip > 0.0f ? raw_skip : 0.0f;
        float text_draw_x = copy_x > x ? copy_x : x;
        float text_budget = max_width - (text_draw_x - x);
        if (text_budget > 0.0f) {
            text_render_window(text_draw_x, y, text, text_color,
                               text_skip, text_budget);
        }
        if (suffix_width > 0.0f) {
            float suffix_abs_x = copy_x + text_width + separator_width;
            float suffix_skip = raw_skip - (text_width + separator_width);
            float suffix_draw_x = suffix_abs_x > x ? suffix_abs_x : x;
            float suffix_budget = max_width - (suffix_draw_x - x);
            if (suffix_skip < 0.0f) {
                suffix_skip = 0.0f;
            }
            if (suffix_budget > 0.0f) {
                text_render_window(suffix_draw_x, y, suffix, suffix_color,
                                   suffix_skip, suffix_budget);
            }
        }
    }
    hal_gpu_set_scissor(0, 0, 480, 272);
}

static void np_video_release(void)
{
    if (s_np_video) {
        video_player_close(s_np_video);
        s_np_video = NULL;
    }
    s_np_frame       = NULL;
    s_np_video_track[0] = '\0';
    s_np_video_tried = 0;
}

/* Release ME resources when leaving now-playing (the ME may contend with sceMp3;
 * callers on screen exit should invoke this). */
void ui_screen_now_playing_release_video(void)
{
    np_video_release();
}

/* Draw the square cover video into the (cx,cy,size) box. Returns 1 if a video
 * frame was drawn (skip the static cover), else 0. */
static int np_video_draw(const TrackEntry *track, float cx, float cy, float size)
{
    AudioPlayerState audio_state;

    if (!track->id[0] || !track->background_video_uri[0]) {
        np_video_release();
        return 0;
    }

    if (strcmp(s_np_video_track, track->id) != 0) {
        np_video_release();
        snprintf(s_np_video_track, sizeof(s_np_video_track), "%s", track->id);
    }

    audio_state = audio_player_get_state();
    if (audio_state == AUDIO_PLAYER_STOPPING ||
        audio_state == AUDIO_PLAYER_STOPPED ||
        audio_state == AUDIO_PLAYER_FINISHED ||
        audio_state == AUDIO_PLAYER_ERROR ||
        audio_state == AUDIO_PLAYER_IDLE) {
        np_video_release();
        return 0;
    }

    if (!s_np_video && !s_np_video_tried) {
        /* Defer until audio is actually playing: the track's TLS handshake has
         * completed by then, so the decoder's ~0.5 MB can't starve mbedTLS of a
         * contiguous block mid-handshake (that error-skips the track). */
        if (audio_state != AUDIO_PLAYER_PLAYING) return 0;
        char path[96];
        if (!video_cover_ready_path(track->id, track->background_video_uri,
                                    path, sizeof(path))) return 0;
        s_np_video       = video_player_open(path);
        s_np_video_tried = 1;
        s_np_last_us     = 0;
    }
    if (!s_np_video) return 0;

    /* Advance at the native frame rate; hold the last frame between advances so
     * the 60 Hz UI loop does not decode faster than the video's cadence. */
    u64 now      = sceKernelGetSystemTimeWide();
    unsigned int sample_us = video_player_frame_duration_us(s_np_video);
    int fps = video_player_fps(s_np_video);
    u64 interval = sample_us > 0 ? sample_us :
                   (fps > 0 ? (1000000ULL / (u64)fps) : 40000ULL);
    if (audio_state == AUDIO_PLAYER_PAUSED) {
        s_np_last_us = now;
    } else if (s_np_frame == NULL || now - s_np_last_us >= interval) {
        const void *f = video_player_next_frame(s_np_video, &s_np_fw, &s_np_fh);
        s_np_last_us = now;
        if (f) {
            s_np_frame   = f;
            s_np_tex     = video_player_tex_dim(s_np_video);
            s_np_last_us = now;
        } else if (video_player_failed(s_np_video)) {
            char failed_track[sizeof(s_np_video_track)];
            snprintf(failed_track, sizeof(failed_track), "%s", track->id);
            np_video_release();
            snprintf(s_np_video_track, sizeof(s_np_video_track), "%s", failed_track);
            s_np_video_tried = 1;
            return 0;
        }
    }
    if (!s_np_frame) return 0;

    (void)size;
    /* Proxy contract: visible 200x200 content is centred in 208x208. The GE
     * clips the 4px transport border and copies the result at exactly 1:1. */
    ui_draw_image_rgba_crop(s_np_frame, s_np_tex, 256, 4, 4, 200, 200, cx, cy);
    return 1;
}

static void draw_perimeter_piece(float x, float y, int side, int start,
                                 int length, int thickness, u32 color)
{
    int perimeter = side * 4;
    int pos = start % perimeter;
    while (length > 0) {
        int edge = pos / side;
        int offset = pos % side;
        int take = side - offset;
        if (take > length) take = length;
        if (edge == 0) {
            ui_draw_rect(x + offset, y, (float)take, (float)thickness, color);
        } else if (edge == 1) {
            ui_draw_rect(x + side - thickness, y + offset,
                         (float)thickness, (float)take, color);
        } else if (edge == 2) {
            ui_draw_rect(x + side - offset - take, y + side - thickness,
                         (float)take, (float)thickness, color);
        } else {
            ui_draw_rect(x, y + side - offset - take,
                         (float)thickness, (float)take, color);
        }
        length -= take;
        pos = (pos + take) % perimeter;
    }
}

static void draw_video_cover_activity(float cover_x, float cover_y, float cover_size)
{
    const int gap = 2;
    const int thickness = 2;
    const int side = (int)cover_size + (gap + thickness) * 2;
    const int perimeter = side * 4;
    const int segment = side * 3 / 4;
    u64 now_us = sceKernelGetSystemTimeWide();
    int start = (int)((now_us / 4000ULL) % (u64)perimeter);
    draw_perimeter_piece(cover_x - gap - thickness,
                         cover_y - gap - thickness,
                         side, start, segment, thickness, 0xFF00D5FF);
}

static void format_duration(int duration_ms, char *out, size_t out_size)
{
    if (duration_ms < 0) {
        snprintf(out, out_size, "-");
        return;
    }
    int total_seconds = duration_ms / 1000;
    int minutes = total_seconds / 60;
    int seconds = total_seconds % 60;
    snprintf(out, out_size, "%d:%02d", minutes, seconds);
}

static void format_audio_info(const AudioPlayerSnapshot *snapshot, char *out, size_t out_size)
{
    char bitrate_str[32];
    char sample_rate_str[32];

    if (!snapshot || !out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    bitrate_str[0] = '\0';
    sample_rate_str[0] = '\0';

    if (snapshot->bitrate_kbps > 0) {
        snprintf(bitrate_str, sizeof(bitrate_str), "MP3 %d kbps", snapshot->bitrate_kbps);
    }

    if (snapshot->sample_rate > 0) {
        if ((snapshot->sample_rate % 1000) == 0) {
            snprintf(sample_rate_str, sizeof(sample_rate_str), "%d kHz", snapshot->sample_rate / 1000);
        } else {
            snprintf(sample_rate_str, sizeof(sample_rate_str), "%.1f kHz", snapshot->sample_rate / 1000.0f);
        }
    }

    if (bitrate_str[0] && sample_rate_str[0]) {
        snprintf(out, out_size, "%s · %s", bitrate_str, sample_rate_str);
    } else if (bitrate_str[0]) {
        snprintf(out, out_size, "%s", bitrate_str);
    } else if (sample_rate_str[0]) {
        snprintf(out, out_size, "%s", sample_rate_str);
    }
}

static const char *audio_player_state_label(AudioPlayerState state)
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

static void draw_progress_bar(float x, float y,
                              float width, float height,
                              const AudioPlayerSnapshot *snapshot,
                              const AudioCacheStatus *cache_status,
                              const char *track_id,
                              u32 playback_color,
                              u32 buffer_color,
                              u32 bg_color,
                              u32 text_color)
{
    int clamped_position_ms;
    float progress;
    float filled_width;
    float buffered_progress = 0.0f;
    float buffered_width = 0.0f;
    char elapsed_str[16];
    char total_str[16];
    float total_width;
    float total_x;

    if (!snapshot || snapshot->duration_ms <= 0) {
        return;
    }

    clamped_position_ms = snapshot->position_ms;
    if (clamped_position_ms < 0) {
        clamped_position_ms = 0;
    }
    if (clamped_position_ms > snapshot->duration_ms) {
        clamped_position_ms = snapshot->duration_ms;
    }

    progress = (float)clamped_position_ms / (float)snapshot->duration_ms;
    if (progress < 0.0f) {
        progress = 0.0f;
    }
    if (progress > 1.0f) {
        progress = 1.0f;
    }

    filled_width = width * progress;

    if (cache_status && track_id && strcmp(cache_status->track_id, track_id) == 0) {
        if (cache_status->content_length > 0 && cache_status->downloaded_bytes > 0) {
            buffered_progress = (float)cache_status->downloaded_bytes /
                                (float)cache_status->content_length;
        } else if (cache_status->complete || cache_status->state == AUDIO_CACHE_READY) {
            buffered_progress = 1.0f;
        }
        if (buffered_progress < 0.0f) {
            buffered_progress = 0.0f;
        }
        if (buffered_progress > 1.0f) {
            buffered_progress = 1.0f;
        }
    }

    buffered_width = width * buffered_progress;

    format_duration(clamped_position_ms, elapsed_str, sizeof(elapsed_str));
    format_duration(snapshot->duration_ms, total_str, sizeof(total_str));
    total_width = text_measure_width(total_str);
    total_x = x + width - total_width;

    ui_draw_text(x, y, elapsed_str, text_color);
    ui_draw_text(total_x, y, total_str, text_color);
    ui_draw_rect(x, y + 14.0f, width, height, bg_color);
    if (buffered_width > 0.0f) {
        ui_draw_rect(x, y + 14.0f, buffered_width, height, buffer_color);
    }
    if (filled_width > 0.0f) {
        ui_draw_rect(x, y + 14.0f, filled_width, height, playback_color);
    }
}

static void draw_volume_bar(float x, float y, float width, float height)
{
    SystemStatusSnapshot status;
    const int segments = 30;
    const float segment_width = 5.0f;
    const float gap = 1.0f;
    const float scale_width = segment_width * segments + gap * (segments - 1);
    const float scale_x = x + (width - scale_width) * 0.5f;
    int i;

    system_status_get_snapshot(&status);
    if (!status.volume_available) {
        return;
    }

    for (i = 0; i < segments; ++i) {
        u32 color = i < status.volume_level ? 0xFF00D5FF : 0xFF3A3A3A;
        ui_draw_rect(scale_x + i * (segment_width + gap), y,
                     segment_width, height, color);
    }
}

static char s_last_requested_track_id[128] = {0};
static int s_last_selected_index = -1;
static char s_last_loaded_track_id[128] = {0};
static char s_last_cover_uri[96] = {0};
static int s_video_cover_requested = 0;

/* Сердце лайка: всегда видно, красное = в лайках, серое = нет.
 * Геометрия 5x5, масштаб 2 (10x10 px). */
static void draw_like_heart(float x, float y, int liked)
{
    static const char rows[5][6] = {
        "XX XX",
        "XXXXX",
        "XXXXX",
        " XXX ",
        "  X  ",
    };
    u32 color = liked ? 0xFF0000FF : 0xFF444444;
    int r, c;
    for (r = 0; r < 5; r++) {
        for (c = 0; c < 5; c++) {
            if (rows[r][c] == 'X') {
                ui_draw_rect(x + (float)(c * 2), y + (float)(r * 2),
                             2.0f, 2.0f, color);
            }
        }
    }
}

/* --- Лайк треугольником (фон, без фриза UI) -------------------------------
 * Метка "*" — локальный паритет на трек (предзагрузки всего сета лайков
 * нет: ответ на 2500 треков не влезет в RAM). add/remove идемпотентны,
 * поэтому паритет всегда сходится с сервером независимо от стартового
 * состояния. Воркер одноразовый, подбирается в update(). */
static SceUID s_like_tid = -1;
static char s_like_token[256];
static int s_like_uid = 0;
static char s_like_track[40];
static int s_like_target = 0;
static char s_like_marked[40];
static int s_like_marked_on = 0;

static int like_worker(SceSize args, void *argp)
{
    YmApiContext ctx;

    (void)args;
    (void)argp;
    ctx.oauth_token = s_like_token;
    ctx.timeout_ms = 0;
    logLine("like: post id='%s' like=%d\n", s_like_track, s_like_target);
    if (ym_api_track_like(&ctx, s_like_uid, s_like_track,
                          s_like_target) == 0) {
        snprintf(s_like_marked, sizeof(s_like_marked), "%s", s_like_track);
        s_like_marked_on = s_like_target;
        logLine("like: ok id='%s' like=%d\n", s_like_track, s_like_target);
    } else {
        logLine("like: failed id='%s' like=%d\n", s_like_track, s_like_target);
    }
    logger_flush();
    memset(s_like_token, 0, sizeof(s_like_token));
    return 0;
}

static void like_reap(void)
{
    SceKernelThreadRunStatus st;

    if (s_like_tid < 0) {
        return;
    }
    st.size = sizeof(st);
    if (sceKernelReferThreadRunStatus(s_like_tid, &st) == 0 &&
        st.status == PSP_THREAD_STOPPED) {
        sceKernelDeleteThread(s_like_tid);
        s_like_tid = -1;
    }
}

static int like_is_on(const char *track_id)
{
    return track_id && track_id[0] && s_like_marked_on &&
           strcmp(s_like_marked, track_id) == 0;
}

static void like_request_toggle(AppState *state)
{
    const char *id = state->now_playing_track.id;
    char token[256];

    if (!id || !id[0] || state->currentUser.uid <= 0) {
        return;
    }
    like_reap();
    if (s_like_tid >= 0) {
        logLine("like: busy, ignored\n");
        return;  // прошлый POST ещё летит
    }
    if (token_loader_read(token, sizeof(token)) != 0) {
        logLine("like: no token\n");
        return;
    }
    snprintf(s_like_token, sizeof(s_like_token), "%s", token);
    memset(token, 0, sizeof(token));
    s_like_uid = state->currentUser.uid;
    snprintf(s_like_track, sizeof(s_like_track), "%s", id);
    s_like_target = like_is_on(id) ? 0 : 1;
    s_like_tid = sceKernelCreateThread("like_worker", like_worker,
                                       0x18, 32 * 1024, 0, NULL);
    if (s_like_tid < 0) {
        logLine("like: create thread failed 0x%08X\n", s_like_tid);
        memset(s_like_token, 0, sizeof(s_like_token));
        return;
    }
    if (sceKernelStartThread(s_like_tid, 0, NULL) < 0) {
        logLine("like: start thread failed\n");
        sceKernelDeleteThread(s_like_tid);
        s_like_tid = -1;
        memset(s_like_token, 0, sizeof(s_like_token));
        return;
    }
}

/* Pack the comma-separated artist names into the available column width.
 * Wrapping happens only between artists, so UTF-8 names are never split. */
static float draw_artist_list(float x, float y, const char *artists,
                              u32 color, float max_width, float line_height)
{
    const char *cursor = artists;
    char line[sizeof(((TrackEntry *)0)->artist)];
    size_t line_len = 0;

    line[0] = '\0';
    while (cursor && *cursor) {
        const char *delimiter = strstr(cursor, ", ");
        size_t name_len = delimiter ? (size_t)(delimiter - cursor) : strlen(cursor);
        char candidate[sizeof(line)];
        int written;

        if (line_len == 0) {
            written = snprintf(candidate, sizeof(candidate), "%.*s",
                               (int)name_len, cursor);
        } else {
            written = snprintf(candidate, sizeof(candidate), "%s, %.*s",
                               line, (int)name_len, cursor);
        }
        if (written < 0) break;
        candidate[sizeof(candidate) - 1] = '\0';

        if (line_len > 0 && text_measure_width(candidate) > max_width) {
            text_render_clipped(x, y, line, color, max_width);
            y += line_height;
            written = snprintf(line, sizeof(line), "%.*s", (int)name_len, cursor);
            if (written < 0) break;
            line[sizeof(line) - 1] = '\0';
            line_len = strlen(line);
        } else {
            strncpy(line, candidate, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            line_len = strlen(line);
        }

        cursor = delimiter ? delimiter + 2 : NULL;
    }

    if (line_len > 0) {
        text_render_clipped(x, y, line, color, max_width);
        y += line_height;
    }
    return y;
}
// Pointer caching: кэшируем указатель на обложку, чтобы не искать в кеше каждый кадр

void ui_screen_now_playing_update(AppState *state)
{
    const TrackEntry *track;
    AudioPlayerSnapshot audio;

    like_reap();
    wave_service();
    /* Keep state->now_playing_track in sync with the controller's current track.
     * Automatic advance (prefetch swap) updates g_playback.current_track but
     * not state->now_playing_track, which is only set on manual track selection. */
    if (g_playback.current_track.id[0] != '\0' &&
        (strcmp(state->now_playing_track.id, g_playback.current_track.id) != 0 ||
         /* Восстановление: id уже стоит, а метаданные докатились позже. */
         (state->now_playing_track.title[0] == '\0' &&
          g_playback.current_track.title[0] != '\0'))) {
        memcpy(&state->now_playing_track, &g_playback.current_track, sizeof(TrackEntry));
    }

    track = &state->now_playing_track;
    if (track->id[0] != '\0') {
        if (s_last_selected_index != state->track_ui.track_selected) {
            s_last_selected_index = state->track_ui.track_selected;
            s_last_requested_track_id[0] = '\0';
        }

        if (strcmp(s_last_loaded_track_id, track->id) != 0) {
            strncpy(s_last_loaded_track_id, track->id,
                    sizeof(s_last_loaded_track_id) - 1);
            s_last_loaded_track_id[sizeof(s_last_loaded_track_id) - 1] = '\0';
            s_last_cover_uri[0] = '\0';

            /* A track change invalidates every previous video-cover attempt.
             * Queue the ordinary cover first; its worker stays asleep while
             * audio is OPENING or BUFFERING. */
            video_cover_clear();
            s_video_cover_requested = 0;
            if (track->cover_uri[0] && track->album_id != 0) {
                cover_now_playing_request_load(track->album_id, track->cover_uri);
                snprintf(s_last_cover_uri, sizeof(s_last_cover_uri),
                         "%s", track->cover_uri);
            } else {
                cover_now_playing_clear();
            }
        } else if (track->cover_uri[0] && track->album_id != 0 &&
                   strcmp(s_last_cover_uri, track->cover_uri) != 0) {
            /* Тот же трек, но обложка появилась позже (восстановление
             * после перезапуска): запросить сейчас, иначе пусто навсегда. */
            cover_now_playing_request_load(track->album_id, track->cover_uri);
            snprintf(s_last_cover_uri, sizeof(s_last_cover_uri),
                     "%s", track->cover_uri);
            logLine("np: late cover request track_id='%s'\n", track->id);
        }

        /* Wake the ordinary-cover worker only when the audio state permits it. */
        cover_now_playing_process_pending();

        /* Video conversion is strictly lower priority: playback must already
         * be running, and the ordinary-cover attempt must be complete. An
         * ordinary-cover error is terminal too and must not wedge this stage. */
        audio_player_get_snapshot(&audio);
        if (!s_video_cover_requested && track->background_video_uri[0] &&
            audio.state == AUDIO_PLAYER_PLAYING && audio.position_ms > 0 &&
            (!track->cover_uri[0] || track->album_id == 0 ||
             !cover_now_playing_is_loading())) {
            video_cover_request(track->id, track->background_video_uri);
            s_video_cover_requested = 1;
        }
    } else if (s_last_loaded_track_id[0]) {
        s_last_loaded_track_id[0] = '\0';
        s_last_cover_uri[0] = '\0';
        s_video_cover_requested = 0;
        cover_now_playing_clear();
        video_cover_clear();
    }
}

void ui_screen_now_playing_handle_input(AppState *state, const InputState *input)
{
    (void)state;

    if (input->pressed & PSP_CTRL_SQUARE) {
        playback_controller_request_stop();
    } else if (input->pressed & PSP_CTRL_TRIANGLE) {
        like_request_toggle(state);
    } else if (input->pressed & PSP_CTRL_SELECT) {
        /* Качество MP3 на следующие треки: nq (192) <-> hq (320). */
        const char *q = ym_api_download_quality();
        ym_api_download_set_quality(strcmp(q, "hq") == 0 ? "nq" : "hq");
        eq_save();
        eq_notify_quality();
        logLine("now_playing: quality -> %s\n", ym_api_download_quality());
    } else if (input->pressed & PSP_CTRL_START) {
        playback_controller_request_toggle_pause();
    } else if (input->pressed & PSP_CTRL_RIGHT) {
        playback_controller_request_next();
    } else if (input->pressed & PSP_CTRL_LEFT) {
        playback_controller_request_previous();
    }
}

void ui_screen_now_playing_render(const AppState *state)
{
    AudioPlayerSnapshot player_snapshot;
    AudioCacheStatus cache_status;

    ui_draw_clear(0xFF1A1A1A);
    // Без шапки - обложка и текст занимают весь экран
    
    // Размеры и позиции
    const float cover_size = 200.0f;
    const float cover_x = 29.0f;  // Отступ слева 29px
    const float cover_y = 29.0f;  // Отступ сверху 29px (272 - 14 - 200) / 2 = 29
    const float text_x = cover_x + cover_size + 29.0f;  // 29px отступ справа от обложки
    const float text_y_start = cover_y;  // Текст начинается на уровне обложки
    const float text_line_height = 18.0f;
    const u32 text_color = 0xFFFFFFFF;
    const u32 text_color_secondary = 0xFFBBBBBB;
    const float progress_width = 190.0f;
    const float progress_height = 6.0f;
    const float text_width = 480.0f - text_x - 8.0f;
    
    if (state->now_playing_track.id[0] == '\0') {
        ui_draw_text(text_x, text_y_start, locale_get(LOCALE_SCREEN_EMPTY), text_color_secondary);
        ui_common_draw_prompts(LOCALE_NOW_PLAYING_TOGGLE_PROMPT,
                               LOCALE_NOW_PLAYING_STOP_PROMPT,
                               LOCALE_TRACK_BACK_PROMPT);
        return;
    }

    memset(&player_snapshot, 0, sizeof(player_snapshot));
    memset(&cache_status, 0, sizeof(cache_status));
    audio_player_get_snapshot(&player_snapshot);
    audio_cache_get_status(&cache_status);
    
    const TrackEntry *track = &state->now_playing_track;
    char title_line[192];
    u64 marquee_now_us = sceKernelGetSystemTimeWide();

    if (strcmp(s_marquee_track, track->id) != 0) {
        snprintf(s_marquee_track, sizeof(s_marquee_track), "%s", track->id);
        s_marquee_start_us = marquee_now_us;
    }
    
    // Draw cover (200x200) - используем кэшированный указатель (без поиска в кеше каждый кадр)
    const NowPlayingCover *cover =
        cover_now_playing_get_for(track->album_id, track->cover_uri);
    if (np_video_draw(track, cover_x, cover_y, cover_size)) {
        // Cover video drawn; static cover skipped this frame.
    } else if (cover && cover->rgba_data) {
        void *dst = hal_fb_get_draw_buffer();
        int dst_stride = hal_fb_get_stride();
        int dst_w = hal_fb_get_width();
        int dst_h = hal_fb_get_height();
        
        int dst_x = (int)cover_x;
        int dst_y = (int)cover_y;
        int copy_w = (cover->w < (int)cover_size) ? cover->w : (int)cover_size;
        int copy_h = (cover->h < (int)cover_size) ? cover->h : (int)cover_size;
        
        if (dst_x >= 0 && dst_y >= 0 && dst_x + copy_w <= dst_w && dst_y + copy_h <= dst_h) {
            int src_stride_pixels = cover->stride_bytes / 4;
            
            hal_gpu_flush_cache_range(cover->rgba_data, 
                (unsigned int)((size_t)cover->stride_bytes * (size_t)cover->h));
            hal_gpu_copy_image(GU_PSM_8888, 0, 0, copy_w, copy_h, 
                src_stride_pixels, cover->rgba_data, dst_x, dst_y, dst_stride, dst);
        }
    } else if (track->album_id != 0 && cover_now_playing_is_loading()) {
        // Draw placeholder only if cover is actually loading
        ui_draw_rect(cover_x, cover_y, cover_size, cover_size, 0xFF444444);
    }
    {
        VideoCoverState video_state = video_cover_state_for(
            track->id, track->background_video_uri);
        if (video_state == VIDEO_COVER_CONVERTING ||
            video_state == VIDEO_COVER_DOWNLOADING) {
            draw_video_cover_activity(cover_x, cover_y, cover_size);
        }
    }
    // Если обложка не загружается (не запрашивалась или ошибка) - не показываем placeholder
    
    // Draw track info справа от обложки
    float text_y = text_y_start;
    
    // 1. Название трека ("*" + сердце — наш локальный паритет лайка)
    if (track->title[0]) {
        int liked = like_is_on(track->id);
        float mx = text_x + 14.0f;
        float mw = text_width - 14.0f;
        draw_like_heart(text_x, text_y, liked);
        if (liked) {
            snprintf(title_line, sizeof(title_line), "* %s%s",
                     track->title,
                     track->explicit_content ? " [E]" : "");
        } else {
            snprintf(title_line, sizeof(title_line), "%s%s",
                     track->title,
                     track->explicit_content ? " [E]" : "");
        }
        title_line[sizeof(title_line) - 1] = '\0';
        draw_marquee_parts(mx, text_y, mw,
                            title_line, text_color,
                            track->version, 0xFF888888,
                            marquee_now_us);
        text_y += text_line_height;
    }
    
    // 2. Название альбома
    if (track->album[0]) {
        draw_marquee_parts(text_x, text_y, text_width,
                            track->album, text_color_secondary,
                            track->album_version, 0xFF777777,
                            marquee_now_us);
        text_y += text_line_height;
    }
    
    // 3. Исполнитель трека
    if (track->artist[0]) {
        text_y = draw_artist_list(text_x, text_y, track->artist,
                                  text_color_secondary, text_width,
                                  text_line_height);
    }

    if (strcmp(player_snapshot.track_id, track->id) == 0) {
        char audio_info[64];
        format_audio_info(&player_snapshot, audio_info, sizeof(audio_info));
        if (audio_info[0]) {
            ui_draw_text(text_x, text_y, audio_info, text_color_secondary);
            text_y += text_line_height;
        }

        ui_draw_text(text_x, text_y,
                     audio_player_state_label(player_snapshot.state),
                     text_color_secondary);
        text_y += text_line_height;

        {
            char eqline[48];
            float pre = eq_get_preamp_db();
            if (pre > 0.0f) {
                snprintf(eqline, sizeof(eqline), "EQ: %s +%ddB",
                         ui_common_eq_preset_name(eq_get_preset()),
                         (int)(pre + 0.5f));
            } else {
                snprintf(eqline, sizeof(eqline), "EQ: %s",
                         ui_common_eq_preset_name(eq_get_preset()));
            }
            ui_draw_text(text_x, text_y, eqline, text_color_secondary);
            text_y += text_line_height;
        }

        if (player_snapshot.duration_ms > 0) {
            draw_progress_bar(text_x, text_y,
                              progress_width, progress_height,
                              &player_snapshot,
                              &cache_status,
                              track->id,
                              0xFF00D5FF,
                              0xFF4E7F66,
                              0xFF3A3A3A,
                              text_color_secondary);
            text_y += 28.0f;

            draw_volume_bar(text_x, text_y,
                            progress_width, progress_height);
            text_y += 14.0f;
        }
    }
    
    // 5. Дополнительная информация: год, жанр
    char info_line[256];
    int has_year = (track->year > 0);

    if (has_year) {
        snprintf(info_line, sizeof(info_line), "%d", track->year);
    } else {
        info_line[0] = '\0';
    }

    if (track->genre[0]) {
        if (has_year) {
            strncat(info_line, " ", sizeof(info_line) - strlen(info_line) - 1);
        }
        strncat(info_line, track->genre, sizeof(info_line) - strlen(info_line) - 1);
    }

    if (info_line[0]) {
        ui_draw_text(text_x, text_y, info_line, text_color_secondary);
    }

    ui_common_draw_prompts(LOCALE_NOW_PLAYING_TOGGLE_PROMPT,
                           LOCALE_NOW_PLAYING_STOP_PROMPT,
                           LOCALE_NOW_PLAYING_LIKE_PROMPT,
                           LOCALE_TRACK_BACK_PROMPT);
}
