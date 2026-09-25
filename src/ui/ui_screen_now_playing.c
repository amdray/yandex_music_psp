#include "ui/ui_screen_now_playing.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_icon_atlas.h"
#include "ui/ui_layout.h"
#include "ui/ui_screens.h"
#include "hal/hal_gfx_config.h"
#include "hal/hal_gpu.h"
#include "services/locale.h"
#include "services/track_like.h"
#include "services/wave.h"
#include "services/audio_cache.h"
#include "services/audio_player.h"
#include "services/cover_now_playing.h"
#include "services/playback_controller.h"
#include "services/video_player.h"
#include "services/video_cover.h"
#include "core/logger.h"
#include "app/app_state.h"
#include "fonts/text.h"
#include <pspctrl.h>
#include <pspgu.h>
#include <pspkernel.h>
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

static void format_sample_rate(int sample_rate, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (sample_rate <= 0) return;

    if ((sample_rate % 1000) == 0) {
        snprintf(out, out_size, "%d kHz", sample_rate / 1000);
    } else {
        snprintf(out, out_size, "%.1f kHz", sample_rate / 1000.0f);
    }
}

static void draw_now_playing_bottom_bar(const UiLayoutWidget *status_widget,
                                        const UiLayoutWidget *mp3,
                                        const UiLayoutWidget *bitrate_widget,
                                        const UiLayoutWidget *sample_rate_widget,
                                        const char *status,
                                        const AudioPlayerSnapshot *snapshot)
{
    if (status_widget) {
        if (status && status[0]) {
            float width = text_measure_width(status);
            ui_draw_text(status_widget->x +
                             (status_widget->w - width) * 0.5f,
                         status_widget->y +
                             (status_widget->h - 14.0f) * 0.5f,
                         status, status_widget->color);
        } else if (snapshot) {
            char bitrate[16];
            char sample_rate[16];
            int icon_width = 0;
            int icon_height = 0;

            bitrate[0] = '\0';
            if (snapshot->bitrate_kbps > 0) {
                snprintf(bitrate, sizeof(bitrate), "%d kbps",
                         snapshot->bitrate_kbps);
            }
            format_sample_rate(snapshot->sample_rate,
                               sample_rate, sizeof(sample_rate));

            if (mp3 &&
                ui_icon_atlas_get_size("in_mp3", &icon_width,
                                       &icon_height) == 0) {
                int icon_x;
                int icon_y;
                icon_x = (int)mp3->x + ((int)mp3->w - icon_width) / 2;
                icon_y = (int)mp3->y + ((int)mp3->h - icon_height) / 2;
                ui_icon_atlas_draw("in_mp3", icon_x, icon_y, mp3->color);
            }
            if (bitrate_widget && bitrate[0]) {
                ui_draw_text(bitrate_widget->x, bitrate_widget->y,
                             bitrate, bitrate_widget->color);
            }
            if (sample_rate_widget && sample_rate[0]) {
                ui_draw_text(sample_rate_widget->x, sample_rate_widget->y,
                             sample_rate, sample_rate_widget->color);
            }
        }
    }
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
                         side, start, segment, thickness, UI_COLOR_ACCENT);
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

static const char *audio_player_bottom_status(AudioPlayerState state)
{
    switch (state) {
        case AUDIO_PLAYER_OPENING:   return "Opening";
        case AUDIO_PLAYER_BUFFERING: return "Buffering";
        case AUDIO_PLAYER_ERROR:     return "Error";
        default:                     return NULL;
    }
}

static void draw_progress_time(const UiLayoutWidget *widget,
                               const AudioPlayerSnapshot *snapshot)
{
    int clamped_position_ms;
    char elapsed_str[16];
    char total_str[16];
    float total_width;

    if (!widget || !snapshot || snapshot->duration_ms <= 0) return;

    clamped_position_ms = snapshot->position_ms;
    if (clamped_position_ms < 0) clamped_position_ms = 0;
    if (clamped_position_ms > snapshot->duration_ms)
        clamped_position_ms = snapshot->duration_ms;

    format_duration(clamped_position_ms, elapsed_str, sizeof(elapsed_str));
    format_duration(snapshot->duration_ms, total_str, sizeof(total_str));
    total_width = text_measure_width(total_str);
    ui_draw_text(widget->x, widget->y, elapsed_str, widget->color);
    ui_draw_text(widget->x + widget->w - total_width, widget->y,
                 total_str, widget->color);
}

static void draw_progress_fill(const UiLayoutWidget *widget,
                               const AudioPlayerSnapshot *snapshot,
                               const AudioCacheStatus *cache_status,
                               const char *track_id)
{
    int clamped_position_ms;
    float progress;
    float filled_width;
    float buffered_progress = 0.0f;
    float buffered_width;

    if (!widget || !snapshot || snapshot->duration_ms <= 0) return;

    clamped_position_ms = snapshot->position_ms;
    if (clamped_position_ms < 0) clamped_position_ms = 0;
    if (clamped_position_ms > snapshot->duration_ms)
        clamped_position_ms = snapshot->duration_ms;

    progress = (float)clamped_position_ms / (float)snapshot->duration_ms;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    filled_width = widget->w * progress;

    if (cache_status && track_id && strcmp(cache_status->track_id, track_id) == 0) {
        if (cache_status->content_length > 0 && cache_status->downloaded_bytes > 0) {
            buffered_progress = (float)cache_status->downloaded_bytes /
                                (float)cache_status->content_length;
        } else if (cache_status->complete || cache_status->state == AUDIO_CACHE_READY) {
            buffered_progress = 1.0f;
        }
        if (buffered_progress < 0.0f) buffered_progress = 0.0f;
        if (buffered_progress > 1.0f) buffered_progress = 1.0f;
    }
    buffered_width = widget->w * buffered_progress;

    ui_draw_rect(widget->x, widget->y, widget->w, widget->h,
                 widget->background_color);
    if (buffered_width > 0.0f) {
        ui_draw_rect(widget->x, widget->y, buffered_width, widget->h,
                     widget->secondary_color);
    }
    if (filled_width > 0.0f) {
        ui_draw_rect(widget->x, widget->y, filled_width, widget->h,
                     widget->color);
    }
}

static char s_last_requested_track_id[128] = {0};
static int s_last_selected_index = -1;
static char s_last_loaded_track_id[128] = {0};
static char s_last_cover_uri[96] = {0};
static int s_video_cover_requested = 0;

/* Холд влево/вправо = перемотка, тап = соседний трек.
 * Тап срабатывает на отпускании (до 450 мс), иначе seek-повторы. */
#define NAV_HOLD_US 450000ULL
#define NAV_SEEK_REPEAT_US 300000ULL
#define NAV_SEEK_STEP_MS 10000
static unsigned long long s_nav_t0 = 0;
static int s_nav_dir = 0;
static int s_nav_seeking = 0;
static unsigned long long s_nav_last = 0;

int ui_screen_now_playing_content_ready(AppState *state)
{
    TrackEntry track;
    int ready;

    if (!state) return -1;
    ready = playback_controller_prepare_current(&track);
    if (ready > 0) {
        memcpy(&state->now_playing_track, &track, sizeof(track));
    }
    return ready;
}

// Pointer caching: кэшируем указатель на обложку, чтобы не искать в кеше каждый кадр

void ui_screen_now_playing_update(AppState *state)
{
    const TrackEntry *track;
    AudioPlayerSnapshot audio;

    track_like_poll();
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

        /* A restored paused session has no audio worker to open this gate, so
         * its current cover may run immediately. Ordinary playback still
         * keeps cover I/O behind the first submitted audio block. */
        cover_now_playing_process_pending(
            playback_controller_get_restored_pause(track->id, NULL));

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
    unsigned long long now = sceKernelGetSystemTimeWide();

    if (input->pressed & PSP_CTRL_CIRCLE) {
        ui_screens_pop_screen(state);
        return;
    }
    if (input->pressed & PSP_CTRL_SQUARE) {
        playback_controller_request_stop();
    } else if (input->pressed & PSP_CTRL_TRIANGLE) {
        track_like_request_toggle(state->currentUser.uid,
                                  state->now_playing_track.id);
    } else if (input->pressed & PSP_CTRL_START) {
        playback_controller_request_toggle_pause();
    } else if (input->pressed & PSP_CTRL_RIGHT) {
        s_nav_t0 = now;
        s_nav_dir = +1;
        s_nav_seeking = 0;
    } else if (input->pressed & PSP_CTRL_LEFT) {
        s_nav_t0 = now;
        s_nav_dir = -1;
        s_nav_seeking = 0;
    }

    /* Холд-автомат соседних треков/перемотки (каждый кадр). */
    if (s_nav_t0 != 0) {
        int bit = (s_nav_dir < 0) ? PSP_CTRL_LEFT : PSP_CTRL_RIGHT;
        if (input->buttons & bit) {
            if (!s_nav_seeking && now - s_nav_t0 >= NAV_HOLD_US) {
                s_nav_seeking = 1;
                s_nav_last = 0;
                logLine("now_playing: seek hold dir=%d\n", s_nav_dir);
            }
            if (s_nav_seeking &&
                (s_nav_last == 0 || now - s_nav_last >= NAV_SEEK_REPEAT_US)) {
                s_nav_last = now;
                playback_controller_request_seek_relative(s_nav_dir * NAV_SEEK_STEP_MS);
            }
        } else {
            if (!s_nav_seeking) {
                if (s_nav_dir < 0) {
                    playback_controller_request_previous();
                } else {
                    playback_controller_request_next();
                }
            }
            s_nav_t0 = 0;
            s_nav_dir = 0;
            s_nav_seeking = 0;
        }
    }
}

void ui_screen_now_playing_render(const AppState *state)
{
    AudioPlayerSnapshot player_snapshot;
    AudioCacheStatus cache_status;
    const char *bottom_status = NULL;
    UiLayoutWidget cover_widget;
    UiLayoutWidget title_widget;
    UiLayoutWidget album_widget;
    UiLayoutWidget artists_widget;
    UiLayoutWidget progress_time_widget;
    UiLayoutWidget progress_bar_widget;
    UiLayoutWidget metadata_widget;
    UiLayoutWidget like_widget;
    UiLayoutWidget bottom_status_widget;
    UiLayoutWidget bottom_mp3_widget;
    UiLayoutWidget bottom_bitrate_widget;
    UiLayoutWidget bottom_sample_rate_widget;

    if (ui_layout_get_widget("now_playing", "cover", &cover_widget) != 0 ||
        ui_layout_get_widget("now_playing", "title", &title_widget) != 0 ||
        ui_layout_get_widget("now_playing", "album", &album_widget) != 0 ||
        ui_layout_get_widget("now_playing", "artists", &artists_widget) != 0 ||
        ui_layout_get_widget("now_playing", "progress_time",
                             &progress_time_widget) != 0 ||
        ui_layout_get_widget("now_playing", "progress_bar",
                             &progress_bar_widget) != 0 ||
        ui_layout_get_widget("now_playing", "metadata", &metadata_widget) != 0 ||
        ui_layout_get_widget("now_playing", "like", &like_widget) != 0 ||
        ui_layout_get_widget("now_playing", "bottom_status",
                             &bottom_status_widget) != 0 ||
        ui_layout_get_widget("now_playing", "bottom_mp3",
                             &bottom_mp3_widget) != 0 ||
        ui_layout_get_widget("now_playing", "bottom_bitrate",
                             &bottom_bitrate_widget) != 0 ||
        ui_layout_get_widget("now_playing", "bottom_sample_rate",
                             &bottom_sample_rate_widget) != 0) {
        return;
    }
    (void)ui_layout_render("now_playing", NULL, NULL, NULL);
    
    if (state->now_playing_track.id[0] == '\0') {
        ui_draw_text(title_widget.x, title_widget.y,
                     locale_get(LOCALE_SCREEN_EMPTY), album_widget.color);
        draw_now_playing_bottom_bar(&bottom_status_widget,
                                    &bottom_mp3_widget,
                                    &bottom_bitrate_widget,
                                    &bottom_sample_rate_widget, NULL, NULL);
        return;
    }

    memset(&player_snapshot, 0, sizeof(player_snapshot));
    memset(&cache_status, 0, sizeof(cache_status));
    audio_player_get_snapshot(&player_snapshot);
    audio_cache_get_status(&cache_status);
    
    const TrackEntry *track = &state->now_playing_track;
    u64 marquee_now_us = sceKernelGetSystemTimeWide();

    if (strcmp(player_snapshot.track_id, track->id) != 0) {
        int paused_position_ms;
        if (playback_controller_get_restored_pause(track->id,
                                                   &paused_position_ms)) {
            memset(&player_snapshot, 0, sizeof(player_snapshot));
            player_snapshot.state = AUDIO_PLAYER_PAUSED;
            player_snapshot.position_ms = paused_position_ms;
            player_snapshot.duration_ms = track->duration_ms;
            snprintf(player_snapshot.track_id,
                     sizeof(player_snapshot.track_id), "%s", track->id);
        }
    }

    if (strcmp(s_marquee_track, track->id) != 0) {
        snprintf(s_marquee_track, sizeof(s_marquee_track), "%s", track->id);
        s_marquee_start_us = marquee_now_us;
    }
    
    // Draw cover (200x200) - используем кэшированный указатель (без поиска в кеше каждый кадр)
    const NowPlayingCover *cover =
        cover_now_playing_get_for(track->album_id, track->cover_uri);
    if (np_video_draw(track, cover_widget.x, cover_widget.y, cover_widget.w)) {
        // Cover video drawn; static cover skipped this frame.
    } else if (cover && cover->rgba_data) {
        void *dst = hal_gpu_get_draw_buffer_cpu();
        int dst_stride = VRAM_BUFFER_WIDTH;
        int dst_w = SCREEN_WIDTH;
        int dst_h = SCREEN_HEIGHT;
        
        int dst_x = (int)cover_widget.x;
        int dst_y = (int)cover_widget.y;
        int copy_w = (cover->w < (int)cover_widget.w)
                         ? cover->w : (int)cover_widget.w;
        int copy_h = (cover->h < (int)cover_widget.h)
                         ? cover->h : (int)cover_widget.h;
        
        if (dst_x >= 0 && dst_y >= 0 && dst_x + copy_w <= dst_w && dst_y + copy_h <= dst_h) {
            int src_stride_pixels = cover->stride_bytes / 4;
            
            hal_gpu_flush_cache_range(cover->rgba_data, 
                (unsigned int)((size_t)cover->stride_bytes * (size_t)cover->h));
            hal_gpu_copy_image(GU_PSM_8888, 0, 0, copy_w, copy_h, 
                src_stride_pixels, cover->rgba_data, dst_x, dst_y, dst_stride, dst);
        }
    } else if (track->album_id != 0 && cover_now_playing_is_loading()) {
        // Draw placeholder only if cover is actually loading
        ui_draw_rect(cover_widget.x, cover_widget.y,
                     cover_widget.w, cover_widget.h, UI_COLOR_INACTIVE);
    }
    {
        VideoCoverState video_state = video_cover_state_for(
            track->id, track->background_video_uri);
        if (video_state == VIDEO_COVER_CONVERTING ||
            video_state == VIDEO_COVER_DOWNLOADING) {
            draw_video_cover_activity(cover_widget.x, cover_widget.y,
                                      cover_widget.w);
        }
    }
    // Если обложка не загружается (не запрашивалась или ошибка) - не показываем placeholder
    
    // 1. Название трека
    if (track->title[0]) {
        ui_common_draw_marquee_font_icon(
            TEXT_FONT_UI16, title_widget.x, title_widget.y, title_widget.w,
            track->title, title_widget.color,
            track->explicit_content ? "in_explicit" : NULL,
            title_widget.color, -2,
            track->version, UI_COLOR_INACTIVE,
            s_marquee_start_us, marquee_now_us);
    }
    
    // 2. Название альбома
    if (track->album[0]) {
        ui_common_draw_marquee(album_widget.x, album_widget.y, album_widget.w,
                               track->album, album_widget.color,
                               track->album_version, UI_COLOR_INACTIVE,
                               s_marquee_start_us, marquee_now_us);
    }
    
    // 3. Исполнитель трека
    if (track->artist[0]) {
        ui_common_draw_marquee(artists_widget.x, artists_widget.y,
                               artists_widget.w,
                               track->artist, artists_widget.color,
                               NULL, 0,
                               s_marquee_start_us, marquee_now_us);
    }

    if (strcmp(player_snapshot.track_id, track->id) == 0) {
        bottom_status = audio_player_bottom_status(player_snapshot.state);

        if (player_snapshot.duration_ms > 0) {
            draw_progress_time(&progress_time_widget, &player_snapshot);
            draw_progress_fill(&progress_bar_widget, &player_snapshot,
                               &cache_status, track->id);
        }
    }
    
    // 5. Жанр
    if (track->genre[0]) {
        text_render_clipped(metadata_widget.x, metadata_widget.y,
                            track->genre, metadata_widget.color,
                            metadata_widget.w);
    }

    {
        int icon_width;
        int icon_height;
        if (ui_icon_atlas_get_size("in_bookmark", &icon_width,
                                   &icon_height) == 0) {
            int icon_x = (int)like_widget.x +
                         ((int)like_widget.w - icon_width) / 2;
            int icon_y = (int)like_widget.y +
                         ((int)like_widget.h - icon_height + 1) / 2;
            u32 color = track_like_is_on(track->id)
                            ? like_widget.color : UI_COLOR_INACTIVE;
            ui_icon_atlas_draw("in_bookmark", icon_x, icon_y, color);
        }
    }

    draw_now_playing_bottom_bar(
        &bottom_status_widget, &bottom_mp3_widget,
        &bottom_bitrate_widget,
        &bottom_sample_rate_widget,
        bottom_status,
        strcmp(player_snapshot.track_id, track->id) == 0 ? &player_snapshot : NULL);

}
