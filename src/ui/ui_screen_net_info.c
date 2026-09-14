#include "ui/ui_screen_net_info.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "services/locale.h"
#include "services/net_ui_status.h"
#include <stdio.h>

/* Security type index → short label */
static const char *security_name(unsigned int t)
{
    switch (t) {
    case 0: return "None";
    case 1: return "WEP";
    case 2: return "WPA";
    case 3: return "WPA2";
    default: return "?";
    }
}

/*
 * Draw one labeled field row:
 *   label  (dim) at x=16
 *   value  (white) at x=VAL_X
 * Returns y advanced by ROW_H.
 */
#define VAL_X   120.0f
#define ROW_H    16.0f
#define COL_LABEL 0xFF888888u
#define COL_VALUE 0xFFFFFFFFu
#define COL_DIM   0xFFAAAAAau

static float draw_field(float y, const char *label, const char *value)
{
    ui_draw_text(16.0f, y, label, COL_LABEL);
    ui_draw_text(VAL_X, y, value, COL_VALUE);
    return y + ROW_H;
}

/*
 * Draw signal strength bar:
 *   background rect + filled rect + percentage text
 */
#define BAR_X     VAL_X
#define BAR_W     80.0f
#define BAR_H      8.0f

static float draw_signal(float y, unsigned int strength)
{
    char buf[16];
    float filled;
    u32 bar_color;

    if (strength > 99u) { strength = 99u; }

    filled = (float)(strength * (unsigned int)BAR_W) / 99.0f;

    if (strength > 66u)      { bar_color = 0xFF00CC00u; } /* green  */
    else if (strength > 33u) { bar_color = 0xFF00CCCCu; } /* cyan   */
    else                     { bar_color = 0xFF4444FFu; } /* red-ish */

    ui_draw_text(16.0f, y, locale_get(LOCALE_NET_INFO_SIGNAL), COL_LABEL);

    /* background */
    ui_draw_rect(BAR_X, y + 2.0f, BAR_W, BAR_H, 0xFF333333u);
    /* filled portion */
    if (filled > 0.0f) {
        ui_draw_rect(BAR_X, y + 2.0f, filled, BAR_H, bar_color);
    }

    snprintf(buf, sizeof(buf), "%u%%", strength);
    ui_draw_text(BAR_X + BAR_W + 6.0f, y, buf, COL_DIM);

    return y + ROW_H;
}

void ui_screen_net_info_render(void)
{
    NetUiStatusSnapshot status;
    NetApctlInfo info;
    float y = 48.0f;
    char buf[64];

    ui_draw_clear(0xFF1A1A1Au);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_NET_INFO));

    net_ui_status_get_snapshot(&status);
    info = status.apctl;

    if (!info.valid) {
        ui_draw_text(16.0f, y, locale_get(LOCALE_NET_INFO_NOT_CONNECTED), 0xFF4444FFu);
        ui_common_draw_prompts(LOCALE_MENU_BACK_PROMPT);
        return;
    }

    y = draw_field(y, locale_get(LOCALE_NET_INFO_PROFILE),  info.profile[0]  ? info.profile  : "-");
    y = draw_field(y, locale_get(LOCALE_NET_INFO_SSID),     info.ssid[0]     ? info.ssid     : "-");
    y = draw_field(y, locale_get(LOCALE_NET_INFO_BSSID),    info.bssid[0]    ? info.bssid    : "-");

    snprintf(buf, sizeof(buf), "%s  ch%u", security_name(info.security_type), info.channel);
    y = draw_field(y, locale_get(LOCALE_NET_INFO_SECURITY), buf);

    if (status.strength_valid) {
        y = draw_signal(y, info.strength);
    }

    /* separator before IP block */
    ui_draw_rect(16.0f, y, 440.0f, 1.0f, 0xFF333333u);
    y += 4.0f;

    y = draw_field(y, locale_get(LOCALE_NET_INFO_IP),      info.ip[0]      ? info.ip      : "-");
    y = draw_field(y, locale_get(LOCALE_NET_INFO_SUBNET),  info.subnet[0]  ? info.subnet  : "-");
    y = draw_field(y, locale_get(LOCALE_NET_INFO_GATEWAY), info.gateway[0] ? info.gateway : "-");
    y = draw_field(y, locale_get(LOCALE_NET_INFO_DNS1),    info.dns1[0]    ? info.dns1    : "-");
    draw_field(y,     locale_get(LOCALE_NET_INFO_DNS2),    info.dns2[0]    ? info.dns2    : "-");

    ui_common_draw_prompts(LOCALE_MENU_BACK_PROMPT);
}
