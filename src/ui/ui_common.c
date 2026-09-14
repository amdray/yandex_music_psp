#include "ui/ui_common.h"
#include "ui/ui_draw.h"
#include "hal/hal_gfx_config.h"
#include "fonts/text.h"
#include "services/locale.h"
#include "services/eq.h"
#include "services/system_status.h"
#include "services/net_ui_status.h"
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void ui_common_draw_header(const char *title)
{
    ui_draw_text(16.0f, 24.0f, title, 0xFFFFFFFF);
    ui_draw_rect(16.0f, 38.0f, 200.0f, 1.0f, 0xFF444444);
}

void ui_common_draw_menu(const MenuItem *items, int count, int selected)
{
    int i;
    float y = 48.0f;

    for (i = 0; i < count; ++i) {
        const char *text = locale_get(items[i].label);
        if (i == selected) {
            ui_draw_rect(12.0f, y - 2.0f, 6.0f, 14.0f, 0xFF00D5FF);
            ui_draw_text(24.0f, y, text, 0xFFFFFFFF);
        } else {
            ui_draw_text(24.0f, y, text, 0xFFBBBBBB);
        }
        y += 16.0f;
    }
}

void ui_common_draw_battery_status(void)
{
    SystemStatusSnapshot status;
    const float body_x = 446.0f;
    const float body_y = 3.0f;
    const float body_w = 24.0f;
    const float body_h = 10.0f;
    const float inner_w = body_w - 4.0f;
    char percent[8];
    float text_width;
    float fill_width;
    u32 fill_color;

    system_status_get_snapshot(&status);
    if (!status.battery_available) {
        return;
    }

    snprintf(percent, sizeof(percent), "%d%%", status.battery_percent);
    text_width = text_measure_width(percent);
    ui_draw_text(body_x - text_width - 6.0f, 2.0f, percent, 0xFFBBBBBB);

    ui_draw_rect(body_x, body_y, body_w, 1.0f, 0xFFBBBBBB);
    ui_draw_rect(body_x, body_y + body_h - 1.0f, body_w, 1.0f, 0xFFBBBBBB);
    ui_draw_rect(body_x, body_y, 1.0f, body_h, 0xFFBBBBBB);
    ui_draw_rect(body_x + body_w - 1.0f, body_y, 1.0f, body_h, 0xFFBBBBBB);
    ui_draw_rect(body_x + body_w, body_y + 3.0f, 2.0f, 4.0f, 0xFFBBBBBB);

    fill_width = inner_w * (float)status.battery_percent / 100.0f;
    if (status.battery_charging) {
        fill_color = 0xFF00D5FF;
    } else if (status.battery_percent <= 15) {
        fill_color = 0xFF3030FF;
    } else {
        fill_color = 0xFF70D070;
    }
    if (fill_width > 0.0f) {
        ui_draw_rect(body_x + 2.0f, body_y + 2.0f,
                     fill_width, body_h - 4.0f, fill_color);
    }
}

static void draw_lock_icon(float x)
{
    ui_draw_rect(x, 9.0f, 10.0f, 8.0f, 0xFFBBBBBB);
    ui_draw_rect(x + 2.0f, 5.0f, 6.0f, 1.0f, 0xFFBBBBBB);
    ui_draw_rect(x + 1.0f, 6.0f, 1.0f, 4.0f, 0xFFBBBBBB);
    ui_draw_rect(x + 8.0f, 6.0f, 1.0f, 4.0f, 0xFFBBBBBB);
}

static void make_elided(const char *src, char *dst, size_t cap, float max_width)
{
    size_t len;

    if (!src || !dst || cap == 0) return;
    snprintf(dst, cap, "%s", src);
    if (text_measure_width(dst) <= max_width) return;

    len = strlen(dst);
    while (len > 0) {
        do { --len; } while (len > 0 && ((unsigned char)dst[len] & 0xC0U) == 0x80U);
        dst[len] = '\0';
        if (len + 3 < cap) {
            memcpy(dst + len, "...", 4);
            if (text_measure_width(dst) <= max_width) return;
        }
    }
    dst[0] = '\0';
}

void ui_common_draw_top_status(void)
{
    static char s_clock_text[6] = "--:--";
    static unsigned long long s_clock_next_us;
    NetUiStatusSnapshot net;
    char ssid[36];
    const char *activity = NULL;
    float clock_width;
    float clock_x;
    float bars_x = 2.0f;
    int active_bars = 0;
    int i;

    net_ui_status_get_snapshot(&net);

    if (net.apctl.valid) {
        make_elided(net.apctl.ssid, ssid, sizeof(ssid), 168.0f);
        ui_draw_text(2.0f, 2.0f, ssid, 0xFFBBBBBB);
        bars_x += text_measure_width(ssid) + 3.0f;
    }

    if (net.strength_valid) {
        unsigned int strength = net.apctl.strength > 99U ? 99U : net.apctl.strength;
        active_bars = strength == 0U ? 0 : (int)((strength * 5U + 98U) / 99U);
    }
    for (i = 0; i < 5; ++i) {
        float height = 3.0f + (float)i * 2.0f;
        ui_draw_rect(bars_x + (float)i * 4.0f, 14.0f - height,
                     2.0f, height,
                     i < active_bars ? 0xFF00D5FF : 0xFF555555);
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
    clock_x = ((float)SCREEN_WIDTH - clock_width) * 0.5f;
    if (net.hold) draw_lock_icon(clock_x - 15.0f);
    ui_draw_text(clock_x, 2.0f, s_clock_text, 0xFFFFFFFF);

    if (net.state == NET_UI_DISCONNECTED) activity = "OFF";
    else if (net.state == NET_UI_STUCK) activity = "WiFi!";
    else if (net.state == NET_UI_RECOVERING) activity = "WiFi...";
    else if (net.download_visible) {
        const char *download_icon = "\xE2\x86\x93"; /* U+2193 DOWNWARDS ARROW */
        float icon_width = text_measure_width(download_icon);
        ui_draw_text(372.0f - icon_width * 0.5f, 2.0f,
                     download_icon, 0xFFBBBBBB);
    }
    if (activity) {
        text_render_clipped(350.0f, 2.0f, activity, 0xFFBBBBBB, 44.0f);
    }

    ui_common_draw_battery_status();
}

void ui_common_draw_prompts_n(const LocaleKey *keys, int count)
{
    const float prompt_height = 16.0f;
    const float prompt_y = (float)(SCREEN_HEIGHT - prompt_height);
    const float text_y = prompt_y;
    const float text_x = 16.0f;
    char prompt[128];
    size_t len = 0;
    int i;

    ui_draw_rect(0.0f, prompt_y, (float)SCREEN_WIDTH, prompt_height, 0xFF2A2A2A);

    prompt[0] = '\0';
    for (i = 0; i < count && len < sizeof(prompt) - 1; ++i) {
        int n = snprintf(prompt + len, sizeof(prompt) - len,
                         (i == 0) ? "%s" : "  %s", locale_get(keys[i]));
        if (n < 0) {
            break;
        }
        len += (size_t)n;
        if (len >= sizeof(prompt)) {  // snprintf truncated: clamp, stop
            len = sizeof(prompt) - 1;
            break;
        }
    }
    ui_draw_text(text_x, text_y, prompt, 0xFF888888);
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
    float w, x, y = 120.0f;
    int kind = eq_toast_kind();

    if (kind == 2) {
        // Предусиление: «VOL +4dB».
        snprintf(buf, sizeof(buf), "VOL +%ddB", (int)eq_get_preamp_db());
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
