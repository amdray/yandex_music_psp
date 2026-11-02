#include "services/playback_queue.h"
#include "services/last_play.h"
#include "ui/ui_screen_now_playing.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_glow_bg.h"
#include "ui/ui_icon_atlas.h"
#include "ui/ui_idle.h"
#include <math.h>
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
#include "core/logger.h"
#include "app/app_state.h"
#include "fonts/text.h"
#include <pspctrl.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        /* Без %f: dtoa newlib не потокобезопасен (см. core/libc_runtime.h). */
        int khz10 = (sample_rate + 50) / 100;
        snprintf(out, out_size, "%d.%d kHz", khz10 / 10, khz10 % 10);
    }
}

static void draw_now_playing_bottom_bar(const UiLayoutWidget *status_widget,
                                        const char *status)
{
    UiLayoutWidget bar;
    bar = ui_layout_slot_now_playing__bottom_bar;
    ui_draw_rect(bar.x, bar.y, bar.w, bar.h, bar.color);
    if (status) {
        float width = text_measure_width(status);
        ui_draw_text(status_widget->x +
                         (status_widget->w - width) * 0.5f,
                     status_widget->y +
                         (status_widget->h - 14.0f) * 0.5f,
                     status, status_widget->color);
    }
}

static void draw_now_playing_format(const UiLayoutWidget *codec_widget,
                                    const UiLayoutWidget *detail_widget,
                                    const UiLayoutWidget *sample_rate_widget,
                                    const AudioPlayerSnapshot *snapshot)
{
    char format_detail[16];
    char sample_rate[16];
    const char *codec_icon;
    int is_flac = strcmp(snapshot->codec, "flac") == 0;
    int is_mp3 = strcmp(snapshot->codec, "mp3") == 0;
    int is_aac = strcmp(snapshot->codec, "aac") == 0;
    int icon_width;
    int icon_height;

    format_detail[0] = '\0';
    if (is_flac) {
        if (snapshot->bits_per_sample > 0) {
            snprintf(format_detail, sizeof(format_detail), "%d %s",
                     snapshot->bits_per_sample, locale_get(LOCALE_UNIT_BIT));
        }
    } else if ((is_mp3 || is_aac) && snapshot->bitrate_kbps > 0) {
        snprintf(format_detail, sizeof(format_detail), "%d kbps",
                 snapshot->bitrate_kbps);
    }
    format_sample_rate(snapshot->sample_rate, sample_rate, sizeof(sample_rate));

    codec_icon = is_mp3 ? "in_mp3" : (is_aac ? "in_aac" : NULL);
    if (codec_icon && ui_icon_atlas_get_size(codec_icon, &icon_width,
                                             &icon_height) == 0) {
        int icon_x = (int)codec_widget->x + ((int)codec_widget->w - icon_width) / 2;
        int icon_y = (int)codec_widget->y + ((int)codec_widget->h - icon_height) / 2;
        ui_icon_atlas_draw(codec_icon, icon_x, icon_y, codec_widget->color);
    }
    if (format_detail[0]) {
        ui_draw_text(detail_widget->x + detail_widget->w -
                         text_measure_width(format_detail), detail_widget->y,
                     format_detail, detail_widget->color);
    }
    if (sample_rate[0]) {
        ui_draw_text(sample_rate_widget->x, sample_rate_widget->y,
                     sample_rate, sample_rate_widget->color);
    }
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

static void draw_cover_activity(float cover_x, float cover_y, float cover_size,
                                u32 color)
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
                         side, start, segment, thickness, color);
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
static int s_glow_pal_album = 0;  /* альбом, из которого взята палитра свечения */

/* Холд влево/вправо = перемотка, короткое нажатие = команда назад/вперёд.
 * Тап срабатывает на отпускании (до 450 мс), иначе seek-повторы. */
#define NAV_HOLD_US 450000ULL
#define NAV_SEEK_REPEAT_US 300000ULL
#define NAV_SEEK_STEP_MS 10000
static unsigned long long s_nav_t0 = 0;
static int s_nav_dir = 0;
static int s_nav_seeking = 0;
static unsigned long long s_nav_last = 0;

void ui_screen_now_playing_on_exit(AppState *state)
{
    (void)state;
    s_nav_t0 = 0;
    s_nav_dir = 0;
    s_nav_seeking = 0;
    s_nav_last = 0;
}

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
    glow_bg_set_accent(track->id[0] ? track->accent_color : 0);
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
            glow_bg_pulse();
            logLine("np: track change -> glow pulse id='%s'\n", track->id);

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

    } else if (s_last_loaded_track_id[0]) {
        s_last_loaded_track_id[0] = '\0';
        s_last_cover_uri[0] = '\0';
        cover_now_playing_clear();
    }

    /* Свечение реагирует на воспроизведение и лайк; пульс при смене трека
     * поднимается в точке обновления s_last_loaded_track_id. */
    if (track->id[0] != '\0') {
        glow_bg_set_liked(track_like_is_on(track->id));

        /* Палитра вайб-фона — из декодированной обложки (как в Яндексе).
         * Один раз на обложку, после её загрузки. */
        if (track->album_id != 0 && track->album_id != s_glow_pal_album) {
            const NowPlayingCover *glow_cover =
                cover_now_playing_get_for(track->album_id, track->cover_uri);
            if (glow_cover && glow_cover->rgba_data && glow_cover->loaded) {
                glow_bg_set_cover(glow_cover->rgba_data, glow_cover->w,
                                  glow_cover->h, glow_cover->stride_bytes);
                s_glow_pal_album = track->album_id;
                logLine("np: glow palette from cover album_id=%d\n",
                        track->album_id);
            }
        }
    }
    audio_player_get_snapshot(&audio);
    glow_bg_set_playing(audio.state == AUDIO_PLAYER_PLAYING);
}

void ui_screen_now_playing_handle_input(AppState *state, const InputState *input)
{
    unsigned long long now = sceKernelGetSystemTimeWide();

    if (input->buttons) idle_note_activity();
    if (input->pressed & PSP_CTRL_CIRCLE) {
        ui_screens_pop_screen(state);
        return;
    }
    if (input->pressed & PSP_CTRL_SQUARE) {
        if (playback_queue_set_order_mode(PLAYBACK_ORDER_SHUFFLE) == 0)
            last_play_save();
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
    UiLayoutWidget shuffle_widget;
    UiLayoutWidget bottom_bitrate_widget;
    UiLayoutWidget bottom_sample_rate_widget;

    cover_widget = ui_layout_slot_now_playing__cover;
    title_widget = ui_layout_slot_now_playing__title;
    album_widget = ui_layout_slot_now_playing__album;
    artists_widget = ui_layout_slot_now_playing__artists;
    progress_time_widget = ui_layout_slot_now_playing__progress_time;
    progress_bar_widget = ui_layout_slot_now_playing__progress_bar;
    metadata_widget = ui_layout_slot_now_playing__metadata;
    like_widget = ui_layout_slot_now_playing__like;
    bottom_status_widget = ui_layout_slot_now_playing__bottom_status;
    bottom_mp3_widget = ui_layout_slot_now_playing__bottom_mp3;
    shuffle_widget = ui_layout_slot_now_playing__shuffle;
    bottom_bitrate_widget = ui_layout_slot_now_playing__bottom_bitrate;
    bottom_sample_rate_widget = ui_layout_slot_now_playing__bottom_sample_rate;
    (void)ui_layout_render(&ui_layout_screen_now_playing, NULL, NULL);
    glow_bg_draw(0.0f, 0.0f, 480.0f, 272.0f);

    const float idle_k = idle_step();
    const int glow_style = glow_bg_get_style();
    const int configured_idle_mode = idle_get_mode();
    const int idle_mode = glow_style == GLOW_STYLE_OFF ||
                         (glow_style == GLOW_STYLE_ACCENT &&
                          configured_idle_mode == IDLE_DIM)
                              ? IDLE_OFF : configured_idle_mode;
    const float idle_s = idle_k * idle_k * (3.0f - 2.0f * idle_k);
    const int cover_idle = idle_mode != IDLE_OFF && idle_k > 0.002f;
    int ui_a = 255;
    int cover_a = 255;
    if (idle_mode == IDLE_DIM) {
        ui_a = cover_a = (int)(255.0f * (1.0f - 0.62f * idle_s));
    } else if (idle_mode == IDLE_HIDE) {
        ui_a = cover_a = (int)(255.0f * (1.0f - idle_s));
    } else if (idle_mode == IDLE_COVER) {
        float ts = idle_s * 1.7f;
        ui_a = (int)(255.0f * (1.0f - (ts > 1.0f ? 1.0f : ts)));
    }

    if (state->now_playing_track.id[0] == '\0') {
        ui_draw_text(title_widget.x, title_widget.y,
                     locale_get(LOCALE_SCREEN_EMPTY), album_widget.color);
        draw_now_playing_bottom_bar(&bottom_status_widget, NULL);
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

    int heart_width = 0, heart_height = 0;
    int heart_available = ui_icon_atlas_get_size("in_heart", &heart_width,
                                                &heart_height) == 0;
    UiLayoutWidget heart_bounds = like_widget;
    if (heart_available) {
        heart_bounds.x = (int)like_widget.x + ((int)like_widget.w - heart_width) / 2;
        heart_bounds.y = (int)like_widget.y + ((int)like_widget.h - heart_height + 1) / 2;
        heart_bounds.w = (float)heart_width;
        heart_bounds.h = (float)heart_height;
    }
    const UiLayoutWidget *scrim_widget = &ui_layout_slot_now_playing__text_scrim;
    const UiGlowTextRegion scrim_region = {
        scrim_widget->x, scrim_widget->y,
        scrim_widget->x + scrim_widget->w, scrim_widget->y + scrim_widget->h,
        title_widget.h
    };
    const unsigned int scrim_text_colors[] = {
        track->title[0] ? title_widget.color : 0,
        track->album[0] ? album_widget.color : 0,
        track->artist[0] ? artists_widget.color : 0,
        strcmp(player_snapshot.track_id, track->id) == 0 &&
            player_snapshot.duration_ms > 0 ? progress_time_widget.color : 0,
        track->genre[0] ? metadata_widget.color : 0
    };
    /* Подложка рисуется до обложки и текста. */
    ui_draw_set_alpha(ui_a);
    glow_bg_text_scrim(&scrim_region, scrim_text_colors,
                       sizeof(scrim_text_colors) / sizeof(scrim_text_colors[0]));
    text_set_shadow(1);

    if (strcmp(s_marquee_track, track->id) != 0) {
        snprintf(s_marquee_track, sizeof(s_marquee_track), "%s", track->id);
        s_marquee_start_us = marquee_now_us;
        idle_note_activity();   /* новый трек — показать информацию */
    }
    
    // Draw cover (200x200) - используем кэшированный указатель (без поиска в кеше каждый кадр)
    const NowPlayingCover *cover =
        cover_now_playing_get_for(track->album_id, track->cover_uri);
    if (cover && cover->rgba_data && cover_idle) {
        /* Бездействие: обложка рисуется текстурой — с прозрачностью и растяжением. */
        float cw = (float)cover->w, ch = (float)cover->h;
        float x0 = cover_widget.x, y0 = cover_widget.y;
        float x1 = x0 + (cw < cover_widget.w ? cw : cover_widget.w);
        float y1 = y0 + (ch < cover_widget.h ? ch : cover_widget.h);
        float u0 = 0.0f, v0 = 0.0f, u1 = x1 - x0, v1 = y1 - y0;
        int a = cover_a;
        if (idle_mode == IDLE_COVER) {
            float t = (float)(sceKernelGetSystemTimeWide() / 1000ULL) * 0.001f;
            float zoom = 1.0f + 0.07f * (0.5f + 0.5f * sinf(t * 0.35f));
            float uw = cw / zoom, uh = uw * (272.0f / 480.0f);
            float ucx = cw * 0.5f + 0.04f * cw * sinf(t * 0.21f);
            float ucy = ch * 0.5f + 0.03f * ch * sinf(t * 0.17f + 1.0f);
            float tu0 = ucx - uw * 0.5f, tv0 = ucy - uh * 0.5f;
            float target;
            if (tu0 < 0.0f) tu0 = 0.0f;
            if (tu0 + uw > cw) tu0 = cw - uw;
            if (tv0 < 0.0f) tv0 = 0.0f;
            if (tv0 + uh > ch) tv0 = ch - uh;
            x0 += (0.0f - x0) * idle_s;      x1 += (480.0f - x1) * idle_s;
            y0 += (0.0f - y0) * idle_s;      y1 += (272.0f - y1) * idle_s;
            u1 = u1 + ((tu0 + uw) - u1) * idle_s;
            v1 = v1 + ((tv0 + uh) - v1) * idle_s;
            u0 = tu0 * idle_s;
            v0 = tv0 * idle_s;
            /* «дыхание» прозрачности поверх живого свечения — картинка и эффект смешиваются */
            target = 0.58f + 0.14f * sinf(t * 0.9f) + 0.06f * sinf(t * 2.3f + 0.7f);
            a = (int)(255.0f * (1.0f + (target - 1.0f) * idle_s));
        }
        if (a > 0) {
            ui_draw_cover_quad(cover->rgba_data, cover->stride_bytes / 4, cover->h,
                               u0, v0, u1, v1, x0, y0, x1, y1,
                               0x00FFFFFFu | ((u32)(a > 255 ? 255 : a) << 24));
        }
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
    if (cover_now_playing_is_loading()) {
        draw_cover_activity(cover_widget.x, cover_widget.y,
                            cover_widget.w, title_widget.color);
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
        if (heart_available) {
            u32 color = track_like_is_on(track->id)
                            ? like_widget.color : UI_COLOR_INACTIVE;
            ui_icon_atlas_draw("in_heart", (int)heart_bounds.x,
                                (int)heart_bounds.y, color);
        }
    }

    if (strcmp(player_snapshot.track_id, track->id) == 0) {
        draw_now_playing_format(&bottom_mp3_widget, &bottom_bitrate_widget,
                                 &bottom_sample_rate_widget, &player_snapshot);
    }
    PlaybackQueueInfo queue_info;
    int shuffle_width, shuffle_height;
    if (playback_queue_get_info(&queue_info) == 0 &&
        queue_info.order_mode == PLAYBACK_ORDER_SHUFFLE &&
        ui_icon_atlas_get_size("in_shuf", &shuffle_width, &shuffle_height) == 0) {
        ui_icon_atlas_draw("in_shuf",
            (int)shuffle_widget.x + ((int)shuffle_widget.w - shuffle_width) / 2,
            (int)shuffle_widget.y + ((int)shuffle_widget.h - shuffle_height) / 2,
            shuffle_widget.color);
    }
    text_set_shadow(0);
    draw_now_playing_bottom_bar(&bottom_status_widget, bottom_status);
    ui_draw_set_alpha(255);

}
