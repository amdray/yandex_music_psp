#include "ui/ui_screen_eq.h"

#include <pspctrl.h>
#include <stdio.h>

#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "services/locale.h"
#include "services/eq.h"
#include "core/logger.h"

// 0 = строка профиля, 1..7 = полосы.
static int s_sel = 0;

static void eq_edit_band(int band, float delta)
{
    float g[EQ_BANDS];
    int i, p = eq_get_preset();

    if (p != EQ_PRESET_CUSTOM) {
        // Стартуем кастом с текущего звучания, а не с нулей.
        eq_get_gains(g);
        for (i = 0; i < EQ_BANDS; i++) {
            eq_set_custom_gain(i, g[i]);
        }
        eq_set_preset(EQ_PRESET_CUSTOM);
    }
    eq_set_custom_gain(band, eq_get_custom_gain(band) + delta);
    logLine("eq: custom band %d %+g dB\n", band,
            (double)eq_get_custom_gain(band));
}

void ui_screen_eq_on_enter(AppState *state)
{
    (void)state;
    s_sel = 0;
}

void ui_screen_eq_on_exit(AppState *state)
{
    (void)state;
    eq_save();
    logLine("eq: saved on exit\n");
}

void ui_screen_eq_handle_input(AppState *state, const InputState *input)
{
    (void)state;
    if (input->pressed & PSP_CTRL_UP) {
        s_sel = (s_sel + 7) % 8;
    }
    if (input->pressed & PSP_CTRL_DOWN) {
        s_sel = (s_sel + 1) % 8;
    }
    if (input->pressed & PSP_CTRL_LEFT) {
        if (s_sel == 0) {
            eq_prev_preset();
        } else {
            eq_edit_band(s_sel - 1, -1.0f);
        }
    }
    if (input->pressed & PSP_CTRL_RIGHT) {
        if (s_sel == 0) {
            eq_next_preset();
        } else {
            eq_edit_band(s_sel - 1, +1.0f);
        }
    }
}

static void eq_band_label(int band, char *out, int out_size)
{
    float f = eq_band_freq(band);
    if (f >= 1000.0f) {
        snprintf(out, out_size, "%g kHz", (double)(f / 1000.0f));
    } else {
        snprintf(out, out_size, "%d Hz", (int)(f + 0.5f));
    }
}

void ui_screen_eq_render(const AppState *state)
{
    char line[96];
    char band[24];
    float gains[EQ_BANDS];
    float y = 48.0f;
    int i;

    (void)state;
    ui_draw_clear(UI_COLOR_BACKGROUND);
    ui_common_draw_header(locale_get(LOCALE_EQ_TITLE));
    eq_get_gains(gains);

    // Строка профиля.
    snprintf(line, sizeof(line), "%s: %s",
             locale_get(LOCALE_EQ_PRESET),
             ui_common_eq_preset_name(eq_get_preset()));
    if (s_sel == 0) {
        ui_draw_rect(12.0f, y - 2.0f, 6.0f, 14.0f, UI_COLOR_ACCENT);
    }
    ui_draw_text(24.0f, y, line,
                 s_sel == 0 ? UI_COLOR_PRIMARY : UI_COLOR_ACTIVE);
    y += 20.0f;

    // Полосы кастомного профиля (показываем активные дБ).
    for (i = 0; i < EQ_BANDS; i++) {
        eq_band_label(i, band, sizeof(band));
        snprintf(line, sizeof(line), "%-8s %+d dB", band, (int)gains[i]);
        if (s_sel == i + 1) {
            ui_draw_rect(12.0f, y - 2.0f, 6.0f, 14.0f, UI_COLOR_ACCENT);
        }
        ui_draw_text(24.0f, y, line,
                     s_sel == i + 1 ? UI_COLOR_PRIMARY : UI_COLOR_ACTIVE);
        y += 15.0f;
    }

}
