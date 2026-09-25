#ifndef YM_UI_COMMON_H
#define YM_UI_COMMON_H

#include "app/app_state.h"
#include "fonts/text.h"
#include "services/locale.h"

// Один пункт меню описывается ровно один раз: метка + экран перехода.
// Отрисовка, границы навигации и переход выводятся из таблицы таких строк.
typedef struct MenuItem {
    LocaleKey label;
    ScreenId  target;
    int       requires_auth;
} MenuItem;

void ui_common_draw_header(const char *title);
void ui_common_draw_top_status(void);
u32 ui_common_pulse_color(void);
void ui_common_draw_marquee(float x, float y, float max_width,
                            const char *text, u32 text_color,
                            const char *suffix, u32 suffix_color,
                            u64 start_us, u64 now_us);
void ui_common_draw_marquee_font(TextFont font,
                                 float x, float y, float max_width,
                                 const char *text, u32 text_color,
                                 const char *suffix, u32 suffix_color,
                                 u64 start_us, u64 now_us);
void ui_common_draw_marquee_font_icon(TextFont font,
                                      float x, float y, float max_width,
                                      const char *text, u32 text_color,
                                      const char *icon_name, u32 icon_color,
                                      int icon_y_offset,
                                      const char *suffix, u32 suffix_color,
                                      u64 start_us, u64 now_us);

// Имя пресета эквалайзера (локализовано) + тост «EQ: ...» на 2.5 c
// после смены (кнопка ♪ или экран). Рисовать каждый кадр поверх всего.
const char *ui_common_eq_preset_name(int preset);
void ui_common_draw_eq_toast(void);

#endif
