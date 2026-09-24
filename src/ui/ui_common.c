#include "ui/ui_common.h"
#include "ui/ui_draw.h"
#include "ui/ui_icon_atlas.h"
#include "ui/ui_layout.h"
#include "hal/hal_gfx_config.h"
#include "hal/hal_gpu.h"
#include "fonts/text.h"
#include "services/locale.h"
#include "services/eq.h"
#include "services/system_status.h"
#include "services/net_ui_status.h"
#include "services/audio_player.h"
#include "services/playback_controller.h"
#include <math.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void ui_common_draw_header(const char *title)
{
    UiLayoutWidget title_widget;
    UiLayoutWidget rule_widget;

    if (ui_layout_get_widget("screen_header", "title", &title_widget) != 0 ||
        ui_layout_get_widget("screen_header", "title_rule", &rule_widget) != 0)
        return;
    if (title_widget.w > 0.0f) {
        text_render_clipped(title_widget.x, title_widget.y, title,
                            title_widget.color, title_widget.w);
    } else {
        ui_draw_text(title_widget.x, title_widget.y, title,
                     title_widget.color);
    }
    ui_draw_rect(rule_widget.x, rule_widget.y, rule_widget.w, rule_widget.h,
                 rule_widget.color);
}

u32 ui_common_pulse_color(void)
{
    static unsigned frame;
    return ((frame++ / 30U) & 1U) ? UI_COLOR_ACCENT : UI_COLOR_ACTIVE;
}

#define UI_MARQUEE_HOLD_US 1200000ULL
#define UI_MARQUEE_SPEED_PX_PER_SEC 24ULL
#define UI_MARQUEE_GAP_PX 28.0f

static float ui_common_marquee_offset(float content_width,
                                      float viewport_width,
                                      u64 start_us, u64 now_us)
{
    u64 travel_us;
    u64 cycle_us;
    u64 phase_us;
    u64 elapsed_us;

    if (content_width <= viewport_width) {
        return 0.0f;
    }
    travel_us = (u64)(((content_width + UI_MARQUEE_GAP_PX) * 1000000.0f) /
                      (float)UI_MARQUEE_SPEED_PX_PER_SEC);
    cycle_us = UI_MARQUEE_HOLD_US + travel_us;
    elapsed_us = now_us >= start_us ? now_us - start_us : 0;
    phase_us = elapsed_us % cycle_us;
    if (phase_us < UI_MARQUEE_HOLD_US) {
        return 0.0f;
    }
    return (float)(((phase_us - UI_MARQUEE_HOLD_US) *
                    UI_MARQUEE_SPEED_PX_PER_SEC) / 1000000ULL);
}

void ui_common_draw_marquee(float x, float y, float max_width,
                            const char *text, u32 text_color,
                            const char *suffix, u32 suffix_color,
                            u64 start_us, u64 now_us)
{
    ui_common_draw_marquee_font(TEXT_FONT_UI, x, y, max_width,
                                text, text_color, suffix, suffix_color,
                                start_us, now_us);
}

static void draw_marquee_font_content(TextFont font,
                                      float x, float y, float max_width,
                                      const char *text, u32 text_color,
                                      const char *icon_name, u32 icon_color,
                                      int icon_y_offset,
                                      const char *suffix, u32 suffix_color,
                                      u64 start_us, u64 now_us)
{
    float text_width;
    float separator_width;
    float icon_offset = 0.0f;
    float suffix_offset = 0.0f;
    float suffix_width = 0.0f;
    float content_width;
    float offset;
    float first_x;
    int icon_width = 0;
    int icon_height = 0;
    int icon_y = 0;
    int copies;
    int i;

    if (!text || !text[0] || max_width <= 0.0f) {
        return;
    }
    text_width = text_measure_width_font(font, text);
    separator_width = text_measure_width_font(font, " ");
    content_width = text_width;
    if (icon_name && icon_name[0] &&
        ui_icon_atlas_get_size(icon_name, &icon_width, &icon_height) == 0) {
        int line_height = text_line_height_font(font);
        icon_offset = content_width + separator_width + 1.0f;
        content_width = icon_offset + (float)icon_width;
        icon_y = (int)(y + (float)(line_height - icon_height) * 0.5f) +
                 icon_y_offset;
    } else {
        icon_name = NULL;
    }
    if (suffix && suffix[0]) {
        suffix_width = text_measure_width_font(font, suffix);
        suffix_offset = content_width + separator_width;
        content_width = suffix_offset + suffix_width;
    }
    if (content_width <= max_width) {
        text_render_font(font, x, y, text, text_color);
        if (icon_name) {
            ui_icon_atlas_draw(icon_name, (int)(x + icon_offset), icon_y,
                               icon_color);
        }
        if (suffix_width > 0.0f) {
            text_render_font(font, x + suffix_offset, y,
                             suffix, suffix_color);
        }
        return;
    }

    offset = ui_common_marquee_offset(content_width, max_width,
                                      start_us, now_us);
    first_x = x - offset;
    hal_gpu_set_scissor((int)x, 0, (int)(max_width + 0.999f), SCREEN_HEIGHT);
    copies = offset > 0.0f ? 2 : 1;
    for (i = 0; i < copies; ++i) {
        float copy_x = first_x + (float)i *
                                     (content_width + UI_MARQUEE_GAP_PX);
        float raw_skip = x - copy_x;
        float text_skip = raw_skip > 0.0f ? raw_skip : 0.0f;
        float text_draw_x = copy_x > x ? copy_x : x;
        float text_budget = max_width - (text_draw_x - x);
        if (text_budget > 0.0f) {
            text_render_window_font(font, text_draw_x, y, text, text_color,
                                    text_skip, text_budget);
        }
        if (icon_name) {
            float icon_abs_x = copy_x + icon_offset;
            if (icon_abs_x + (float)icon_width > x &&
                icon_abs_x < x + max_width) {
                ui_icon_atlas_draw(icon_name, (int)icon_abs_x, icon_y,
                                   icon_color);
            }
        }
        if (suffix_width > 0.0f) {
            float suffix_abs_x = copy_x + suffix_offset;
            float suffix_skip = raw_skip - suffix_offset;
            float suffix_draw_x = suffix_abs_x > x ? suffix_abs_x : x;
            float suffix_budget = max_width - (suffix_draw_x - x);
            if (suffix_skip < 0.0f) {
                suffix_skip = 0.0f;
            }
            if (suffix_budget > 0.0f) {
                text_render_window_font(font, suffix_draw_x, y, suffix,
                                        suffix_color, suffix_skip,
                                        suffix_budget);
            }
        }
    }
    hal_gpu_set_scissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
}

void ui_common_draw_marquee_font(TextFont font,
                                 float x, float y, float max_width,
                                 const char *text, u32 text_color,
                                 const char *suffix, u32 suffix_color,
                                 u64 start_us, u64 now_us)
{
    draw_marquee_font_content(font, x, y, max_width, text, text_color,
                              NULL, 0, 0, suffix, suffix_color,
                              start_us, now_us);
}

void ui_common_draw_marquee_font_icon(TextFont font,
                                      float x, float y, float max_width,
                                      const char *text, u32 text_color,
                                      const char *icon_name, u32 icon_color,
                                      int icon_y_offset,
                                      const char *suffix, u32 suffix_color,
                                      u64 start_us, u64 now_us)
{
    draw_marquee_font_content(font, x, y, max_width, text, text_color,
                              icon_name, icon_color, icon_y_offset,
                              suffix, suffix_color,
                              start_us, now_us);
}

static void draw_lock_icon(float x, float y, u32 color)
{
    ui_draw_rect(x, y + 7.0f, 10.0f, 8.0f, color);
    ui_draw_rect(x + 2.0f, y + 3.0f, 6.0f, 1.0f, color);
    ui_draw_rect(x + 1.0f, y + 4.0f, 1.0f, 4.0f, color);
    ui_draw_rect(x + 8.0f, y + 4.0f, 1.0f, 4.0f, color);
}

#define VOLUME_DOT_COUNT 10
#define VOLUME_LEVEL_MAX 30
#define VOLUME_IMAGE_WIDTH 78
#define VOLUME_IMAGE_HEIGHT 16
#define VOLUME_TEXTURE_WIDTH 128
#define VOLUME_HALO_WIDTH 12
#define VOLUME_HALO_HEIGHT 13

static u32 s_volume_pixels[VOLUME_TEXTURE_WIDTH * VOLUME_IMAGE_HEIGHT]
    __attribute__((aligned(16)));
static float s_volume_halo_t[VOLUME_HALO_WIDTH * VOLUME_HALO_HEIGHT];
static float s_volume_front[VOLUME_DOT_COUNT];
static float s_volume_core[VOLUME_DOT_COUNT];
static float s_volume_halo[VOLUME_DOT_COUNT];
static u64 s_volume_previous_us;
static u64 s_volume_last_activity_us;
static int s_volume_previous_level;
static int s_volume_light_ready;

static unsigned char volume_byte(float value)
{
    if (value <= 0.0f) return 0;
    if (value >= 255.0f) return 255;
    return (unsigned char)(value + 0.5f);
}

static void volume_blend_pixel(int x, int y, float red, float green,
                               float blue, float alpha)
{
    u32 *pixel;
    float inverse;
    float dst_alpha;
    float out_alpha;
    float dst_red;
    float dst_green;
    float dst_blue;

    if (x < 0 || x >= VOLUME_IMAGE_WIDTH ||
        y < 0 || y >= VOLUME_IMAGE_HEIGHT || alpha <= 0.0f) {
        return;
    }
    if (alpha > 1.0f) alpha = 1.0f;
    pixel = &s_volume_pixels[y * VOLUME_TEXTURE_WIDTH + x];
    inverse = 1.0f - alpha;
    dst_alpha = (float)((*pixel >> 24) & 0xFFU) / 255.0f;
    out_alpha = alpha + dst_alpha * inverse;
    dst_red = (float)(*pixel & 0xFFU);
    dst_green = (float)((*pixel >> 8) & 0xFFU);
    dst_blue = (float)((*pixel >> 16) & 0xFFU);
    if (out_alpha <= 0.0f) {
        *pixel = 0;
        return;
    }
    *pixel = ((u32)volume_byte(out_alpha * 255.0f) << 24) |
             (u32)volume_byte((red * alpha +
                               dst_red * dst_alpha * inverse) / out_alpha) |
             ((u32)volume_byte((green * alpha +
                                dst_green * dst_alpha * inverse) / out_alpha) << 8) |
             ((u32)volume_byte((blue * alpha +
                                dst_blue * dst_alpha * inverse) / out_alpha) << 16);
}

static void volume_light_init(u64 now_us, int level)
{
    int x;
    int y;

    for (y = 0; y < VOLUME_HALO_HEIGHT; ++y) {
        for (x = 0; x < VOLUME_HALO_WIDTH; ++x) {
            float dx = (float)x - 5.5f;
            float dy = (float)y - 6.0f;
            float distance = sqrtf(dx * dx + dy * dy);
            float t = (distance - 0.7f) / 5.3f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            s_volume_halo_t[y * VOLUME_HALO_WIDTH + x] = t;
        }
    }
    s_volume_previous_us = now_us;
    s_volume_last_activity_us = now_us;
    s_volume_previous_level = level;
    s_volume_light_ready = 1;
}

static void volume_current_brightness(int level, float *target)
{
    float position;
    float fraction;
    int left;
    int i;

    for (i = 0; i < VOLUME_DOT_COUNT; ++i) target[i] = 0.0f;
    if (level <= 0) return;
    position = (float)(level - 1) * (float)(VOLUME_DOT_COUNT - 1) /
               (float)(VOLUME_LEVEL_MAX - 1);
    left = (int)position;
    fraction = position - (float)left;
    target[left] = cosf(fraction * 1.57079632679f);
    if (left + 1 < VOLUME_DOT_COUNT) {
        target[left + 1] = sinf(fraction * 1.57079632679f);
    }
}

static void volume_advance(int level, int button_down, u64 now_us)
{
    const float attack_tau = 0.048f;
    const float core_tau = 1.6f / 4.605170186f;
    const float halo_tau = 2.2f / 4.605170186f;
    const float held_brightness = 0.22f;
    float target[VOLUME_DOT_COUNT];
    float dt;
    float attack_amount;
    float core_decay;
    float halo_decay;
    int active;
    int i;

    if (!s_volume_light_ready) volume_light_init(now_us, level);
    if (button_down || level != s_volume_previous_level) {
        s_volume_last_activity_us = now_us;
    }
    s_volume_previous_level = level;
    dt = (float)(now_us - s_volume_previous_us) / 1000000.0f;
    if (dt > 0.05f) dt = 0.05f;
    s_volume_previous_us = now_us;
    active = now_us - s_volume_last_activity_us <= 2000000ULL;
    volume_current_brightness(active ? level : 0, target);
    if (active && level >= 1) {
        float position = (float)(level - 1) *
                         (float)(VOLUME_DOT_COUNT - 1) /
                         (float)(VOLUME_LEVEL_MAX - 1);
        int front_index = (int)position;
        for (i = 0; i <= front_index; ++i) {
            if (target[i] < held_brightness) target[i] = held_brightness;
        }
    }
    attack_amount = 1.0f - expf(-dt / attack_tau);
    core_decay = expf(-dt / core_tau);
    halo_decay = expf(-dt / halo_tau);
    for (i = 0; i < VOLUME_DOT_COUNT; ++i) {
        if (active && target[i] == held_brightness) {
            s_volume_front[i] = held_brightness;
            s_volume_core[i] = held_brightness;
            s_volume_halo[i] = held_brightness;
            continue;
        }
        if (target[i] > s_volume_front[i]) {
            s_volume_front[i] +=
                (target[i] - s_volume_front[i]) * attack_amount;
        } else {
            s_volume_front[i] = target[i];
        }
        s_volume_core[i] = s_volume_front[i] >= s_volume_core[i]
                               ? s_volume_front[i]
                               : s_volume_core[i] * core_decay;
        s_volume_halo[i] = s_volume_front[i] >= s_volume_halo[i]
                               ? s_volume_front[i]
                               : s_volume_halo[i] * halo_decay;
        if (s_volume_core[i] < 0.01f) s_volume_core[i] = 0.0f;
        if (s_volume_halo[i] < 0.01f) s_volume_halo[i] = 0.0f;
    }
}

static void volume_render_pixels(void)
{
    static const char oval[5][7] = {
        "011110", "111111", "111111", "111111", "011110"
    };
    int dot;
    int x;
    int y;

    for (y = 0; y < VOLUME_IMAGE_HEIGHT; ++y) {
        for (x = 0; x < VOLUME_TEXTURE_WIDTH; ++x) {
            s_volume_pixels[y * VOLUME_TEXTURE_WIDTH + x] = 0;
        }
    }
    for (dot = 0; dot < VOLUME_DOT_COUNT; ++dot) {
        float glow;
        if (s_volume_halo[dot] <= 0.0f) continue;
        glow = powf(s_volume_halo[dot], 1.4f) * 0.16f;
        for (y = 0; y < VOLUME_HALO_HEIGHT; ++y) {
            for (x = 0; x < VOLUME_HALO_WIDTH; ++x) {
                float t = s_volume_halo_t[y * VOLUME_HALO_WIDTH + x];
                float red;
                float green;
                float blue;
                float alpha;
                if (t <= 0.45f) {
                    float q = t / 0.45f;
                    red = 255.0f + (235.0f - 255.0f) * q;
                    green = 255.0f + (238.0f - 255.0f) * q;
                    blue = 255.0f + (242.0f - 255.0f) * q;
                    alpha = glow * (1.0f + (0.48f - 1.0f) * q);
                } else {
                    float q = (t - 0.45f) / 0.55f;
                    red = 235.0f + (220.0f - 235.0f) * q;
                    green = 238.0f + (225.0f - 238.0f) * q;
                    blue = 242.0f + (232.0f - 242.0f) * q;
                    alpha = glow * 0.48f * (1.0f - q);
                }
                volume_blend_pixel(dot * 8 - 3 + x, 1 + y,
                                   red, green, blue, alpha);
            }
        }
    }
    for (dot = 0; dot < VOLUME_DOT_COUNT; ++dot) {
        unsigned char channel = volume_byte(
            22.0f + powf(s_volume_core[dot], 0.82f) * 233.0f);
        unsigned char alpha = channel > 26
                                  ? volume_byte(((float)channel - 26.0f) *
                                                255.0f / 229.0f)
                                  : 0;
        u32 color = ((u32)alpha << 24) | 0x00FFFFFFU;
        for (y = 0; y < 5; ++y) {
            for (x = 0; x < 6; ++x) {
                if (oval[y][x] == '1') {
                    s_volume_pixels[(5 + y) * VOLUME_TEXTURE_WIDTH +
                                    dot * 8 + x] = color;
                }
            }
        }
    }
}

static void draw_volume_light(const UiLayoutWidget *widget,
                              const SystemStatusSnapshot *status)
{
    u64 now_us;
    float x;
    float y;

    if (!widget || !status || !status->volume_available) return;
    now_us = sceKernelGetSystemTimeWide();
    volume_advance(status->volume_level, status->volume_button_down, now_us);
    volume_render_pixels();
    x = widget->x + (widget->w - (float)VOLUME_IMAGE_WIDTH) * 0.5f;
    y = widget->y + (widget->h - (float)VOLUME_IMAGE_HEIGHT) * 0.5f;
    ui_draw_image_rgba_nearest(s_volume_pixels,
                               VOLUME_TEXTURE_WIDTH, VOLUME_IMAGE_HEIGHT,
                               VOLUME_IMAGE_WIDTH, VOLUME_IMAGE_HEIGHT,
                               x, y);
}

static void draw_eq_profile(const UiLayoutWidget *widget)
{
    float gains[EQ_BANDS];
    int x;
    int baseline;
    int band;

    if (!eq_is_active() || widget->w < 13.0f || widget->h < 9.0f) {
        return;
    }
    eq_get_gains(gains);
    x = (int)widget->x + ((int)widget->w - 13) / 2;
    baseline = (int)widget->y + ((int)widget->h - 9 + 1) / 2 + 4;
    for (band = 0; band < EQ_BANDS; ++band) {
        float magnitude = fabsf(gains[band]);
        int extent = (int)(magnitude * 0.5f + 0.5f);
        if (extent > 4) extent = 4;
        if (magnitude > 0.0f && extent == 0) extent = 1;
        if (gains[band] > 0.0f) {
            ui_draw_rect((float)(x + band * 2), (float)(baseline - extent),
                         1.0f, (float)(extent + 1), widget->color);
        } else {
            ui_draw_rect((float)(x + band * 2), (float)baseline,
                         1.0f, (float)(extent + 1), widget->color);
        }
    }
}

void ui_common_draw_top_status(void)
{
    static char s_clock_text[6] = "--:--";
    static unsigned long long s_clock_next_us;
    NetUiStatusSnapshot net;
    UiLayoutWidget wifi;
    UiLayoutWidget playback_state;
    UiLayoutWidget network_activity;
    UiLayoutWidget eq_profile;
    UiLayoutWidget clock;
    UiLayoutWidget volume;
    UiLayoutWidget battery_percent;
    UiLayoutWidget battery;
    SystemStatusSnapshot status;
    float clock_width;
    float clock_x;
    u32 wifi_active_color;
    u32 wifi_inactive_color;
    int active_bars = 0;
    int i;

    if (ui_layout_get_widget("status_overlay", "wifi", &wifi) != 0 ||
        ui_layout_get_widget("status_overlay", "playback_state", &playback_state) != 0 ||
        ui_layout_get_widget("status_overlay", "network_activity", &network_activity) != 0 ||
        ui_layout_get_widget("status_overlay", "eq_profile", &eq_profile) != 0 ||
        ui_layout_get_widget("status_overlay", "clock", &clock) != 0 ||
        ui_layout_get_widget("status_overlay", "volume", &volume) != 0 ||
        ui_layout_get_widget("status_overlay", "battery_percent", &battery_percent) != 0 ||
        ui_layout_get_widget("status_overlay", "battery", &battery) != 0)
        return;
    net_ui_status_get_snapshot(&net);
    system_status_get_snapshot(&status);

    wifi_active_color = wifi.color;
    wifi_inactive_color = UI_COLOR_INACTIVE;
    if (net.state == NET_UI_ONLINE && net.strength_valid) {
        unsigned int strength = net.apctl.strength > 99U ? 99U : net.apctl.strength;
        active_bars = strength == 0U ? 0 : (int)((strength * 5U + 98U) / 99U);
        wifi_inactive_color = UI_COLOR_INACTIVE;
    } else if (net.state == NET_UI_RECOVERING) {
        active_bars = (int)((sceKernelGetSystemTimeWide() / 200000ULL) % 6ULL);
    } else if (net.state == NET_UI_STUCK) {
        active_bars = 5;
        wifi_active_color = UI_COLOR_ERROR;
    }
    for (i = 0; i < 5; ++i) {
        float height = 3.0f + (float)i * 2.0f;
        ui_draw_rect(wifi.x + (float)i * 4.0f, wifi.y + wifi.h - height,
                     2.0f, height,
                     i < active_bars ? wifi_active_color
                                     : wifi_inactive_color);
    }

    {
        const char *icon_name;
        int icon_width;
        int icon_height;
        int icon_x;
        int icon_y;
        AudioPlayerState player_state = audio_player_get_state();

        if (player_state == AUDIO_PLAYER_PAUSED ||
            playback_controller_get_restored_pause(NULL, NULL)) {
            icon_name = "in_pause";
        } else if (player_state == AUDIO_PLAYER_STOPPING ||
                   player_state == AUDIO_PLAYER_STOPPED ||
                   player_state == AUDIO_PLAYER_FINISHED ||
                   player_state == AUDIO_PLAYER_ERROR ||
                   player_state == AUDIO_PLAYER_IDLE) {
            icon_name = "in_stop";
        } else {
            icon_name = "in_play";
        }
        if (ui_icon_atlas_get_size(icon_name, &icon_width, &icon_height) == 0) {
            icon_x = (int)playback_state.x +
                     ((int)playback_state.w - icon_width) / 2;
            icon_y = (int)playback_state.y +
                     ((int)playback_state.h - icon_height + 1) / 2;
            ui_icon_atlas_draw(icon_name, icon_x, icon_y, playback_state.color);
        }
    }

    {
        unsigned long long monotonic_now = sceKernelGetSystemTimeWide();
        if (s_clock_next_us == 0 || monotonic_now >= s_clock_next_us) {
            time_t wall_now = time(NULL);
            struct tm *local = localtime(&wall_now);
            if (local) {
                snprintf(s_clock_text, sizeof(s_clock_text), "%02d:%02d",
                         local->tm_hour, local->tm_min);
            }
            s_clock_next_us = monotonic_now + 1000000ULL;
        }
    }
    clock_width = text_measure_width(s_clock_text);
    clock_x = clock.x + (clock.w - clock_width) * 0.5f;
    if (net.hold) draw_lock_icon(clock_x - 15.0f, clock.y, clock.color);
    ui_draw_text(clock_x, clock.y, s_clock_text, clock.color);
    draw_volume_light(&volume, &status);
    draw_eq_profile(&eq_profile);

    if (net.download_alpha > 0U && net.state == NET_UI_ONLINE) {
        int icon_width;
        int icon_height;
        if (ui_icon_atlas_get_size("in_network", &icon_width, &icon_height) == 0) {
            unsigned int base_alpha = (network_activity.color >> 24) & 0xFFU;
            unsigned int alpha =
                (base_alpha * net.download_alpha + 127U) / 255U;
            unsigned int color =
                (network_activity.color & 0x00FFFFFFU) | (alpha << 24);
            int icon_x = (int)network_activity.x +
                         ((int)network_activity.w - icon_width) / 2;
            int icon_y = (int)network_activity.y +
                         ((int)network_activity.h - icon_height + 1) / 2;
            ui_icon_atlas_draw("in_network", icon_x, icon_y, color);
        }
    }

    {
        char percent[8];
        float percent_width;
        float body_w = battery.w - 2.0f;
        float inner_w = body_w - 4.0f;
        float fill_width;
        if (!status.battery_available) return;
        snprintf(percent, sizeof(percent), "%d%%", status.battery_percent);
        percent_width = text_measure_width(percent);
        ui_draw_text(battery_percent.x + battery_percent.w - percent_width,
                     battery_percent.y, percent, battery_percent.color);
        ui_draw_rect(battery.x, battery.y, body_w, 1.0f, battery.color);
        ui_draw_rect(battery.x, battery.y + battery.h - 1.0f,
                     body_w, 1.0f, battery.color);
        ui_draw_rect(battery.x, battery.y, 1.0f, battery.h, battery.color);
        ui_draw_rect(battery.x + body_w - 1.0f, battery.y,
                     1.0f, battery.h, battery.color);
        ui_draw_rect(battery.x + body_w, battery.y + 3.0f,
                     2.0f, 4.0f, battery.color);
        fill_width = inner_w * (float)status.battery_percent / 100.0f;
        if (fill_width > 0.0f) {
            ui_draw_rect(battery.x + 2.0f, battery.y + 2.0f,
                         fill_width, battery.h - 4.0f, battery.color);
        }
    }
}

const char *ui_common_eq_preset_name(int preset)
{
    switch (preset) {
    case EQ_PRESET_ROCK:      return locale_get(LOCALE_EQ_ROCK);
    case EQ_PRESET_POP:       return locale_get(LOCALE_EQ_POP);
    case EQ_PRESET_JAZZ:      return locale_get(LOCALE_EQ_JAZZ);
    case EQ_PRESET_CLASSICAL: return locale_get(LOCALE_EQ_CLASSICAL);
    case EQ_PRESET_BASS:      return locale_get(LOCALE_EQ_BASS);
    case EQ_PRESET_TREBLE:    return locale_get(LOCALE_EQ_TREBLE);
    case EQ_PRESET_VOCAL:     return locale_get(LOCALE_EQ_VOCAL);
    case EQ_PRESET_CUSTOM:    return locale_get(LOCALE_EQ_CUSTOM);
    case EQ_PRESET_OFF:
    default:                  return locale_get(LOCALE_EQ_OFF);
    }
}

void ui_common_draw_eq_toast(void)
{
    char buf[64];
    float w, x, y = 222.0f;
    int kind = eq_toast_kind();

    if (kind == 1) {
        snprintf(buf, sizeof(buf), "EQ: %s",
                 ui_common_eq_preset_name(eq_get_preset()));
    } else {
        return;
    }
    w = text_measure_width(buf);
    x = ((float)SCREEN_WIDTH - w) * 0.5f;
    ui_draw_rect(x - 10.0f, y - 6.0f, w + 20.0f, 28.0f,
                 UI_COLOR_PANEL);
    ui_draw_text(x, y, buf, UI_COLOR_ACCENT);
}
