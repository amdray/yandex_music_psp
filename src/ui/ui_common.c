#include "ui/ui_common.h"
#include "ui/ui_draw.h"
#include "ui/ui_icon_atlas.h"
#include "ui/ui_layout.h"
#include "hal/hal_gfx_config.h"
#include "hal/hal_gpu.h"
#include "fonts/text.h"
#include "services/locale.h"
#include "services/eq.h"
#include "services/ym_api.h"
#include "services/system_status.h"
#include "services/net_ui_status.h"
#include "services/audio_player.h"
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
    unsigned phase = frame++ % 120U;
    unsigned wave = phase < 60U ? phase : 119U - phase;
    unsigned level = 0x77U + wave * 0x88U / 59U;
    return 0xFF000000U | level | (level << 8) | (level << 16);
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
    hal_gpu_set_scissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
}

static void draw_lock_icon(float x, float y, u32 color)
{
    ui_draw_rect(x, y + 7.0f, 10.0f, 8.0f, color);
    ui_draw_rect(x + 2.0f, y + 3.0f, 6.0f, 1.0f, color);
    ui_draw_rect(x + 1.0f, y + 4.0f, 1.0f, 4.0f, color);
    ui_draw_rect(x + 8.0f, y + 4.0f, 1.0f, 4.0f, color);
}

void ui_common_draw_top_status(void)
{
    static char s_clock_text[6] = "--:--";
    static unsigned long long s_clock_next_us;
    NetUiStatusSnapshot net;
    UiLayoutWidget wifi;
    UiLayoutWidget playback_state;
    UiLayoutWidget clock;
    UiLayoutWidget activity_widget;
    UiLayoutWidget battery_percent;
    UiLayoutWidget battery;
    const char *activity = NULL;
    float clock_width;
    float clock_x;
    int active_bars = 0;
    int i;

    if (ui_layout_get_widget("status_overlay", "wifi", &wifi) != 0 ||
        ui_layout_get_widget("status_overlay", "playback_state", &playback_state) != 0 ||
        ui_layout_get_widget("status_overlay", "clock", &clock) != 0 ||
        ui_layout_get_widget("status_overlay", "activity", &activity_widget) != 0 ||
        ui_layout_get_widget("status_overlay", "battery_percent", &battery_percent) != 0 ||
        ui_layout_get_widget("status_overlay", "battery", &battery) != 0)
        return;
    net_ui_status_get_snapshot(&net);

    if (net.strength_valid) {
        unsigned int strength = net.apctl.strength > 99U ? 99U : net.apctl.strength;
        active_bars = strength == 0U ? 0 : (int)((strength * 5U + 98U) / 99U);
    }
    for (i = 0; i < 5; ++i) {
        float height = 3.0f + (float)i * 2.0f;
        ui_draw_rect(wifi.x + (float)i * 4.0f, wifi.y + wifi.h - height,
                     2.0f, height,
                     i < active_bars ? wifi.color : 0xFF555555);
    }

    {
        const char *icon_name;
        int icon_width;
        int icon_height;
        int icon_x;
        int icon_y;
        AudioPlayerState player_state = audio_player_get_state();

        if (player_state == AUDIO_PLAYER_PAUSED) {
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

    if (net.state == NET_UI_DISCONNECTED) activity = "OFF";
    else if (net.state == NET_UI_STUCK) activity = "WiFi!";
    else if (net.state == NET_UI_RECOVERING) activity = "WiFi...";
    if (activity) {
        text_render_clipped(activity_widget.x, activity_widget.y, activity,
                            activity_widget.color, activity_widget.w);
    }

    {
        SystemStatusSnapshot status;
        char percent[8];
        float percent_width;
        float body_w = battery.w - 2.0f;
        float inner_w = body_w - 4.0f;
        float fill_width;

        system_status_get_snapshot(&status);
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

    if (kind == 2) {
        // Предусиление: «VOL +4dB».
        snprintf(buf, sizeof(buf), "VOL +%ddB", (int)eq_get_preamp_db());
    } else if (kind == 3) {
        // Качество: «MP3 320» / «MP3 192».
        snprintf(buf, sizeof(buf), "MP3 %s",
                 strcmp(ym_api_download_quality(), "hq") == 0 ? "320" : "192");
    } else if (kind == 1) {
        snprintf(buf, sizeof(buf), "EQ: %s",
                 ui_common_eq_preset_name(eq_get_preset()));
    } else {
        return;
    }
    w = text_measure_width(buf);
    x = ((float)SCREEN_WIDTH - w) * 0.5f;
    ui_draw_rect(x - 10.0f, y - 6.0f, w + 20.0f, 28.0f, 0xDD1A1A1A);
    ui_draw_text(x, y, buf, 0xFF00D5FF);
}
