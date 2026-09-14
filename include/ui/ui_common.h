#ifndef YM_UI_COMMON_H
#define YM_UI_COMMON_H

#include "app/app_state.h"
#include "services/locale.h"

// Один пункт меню описывается ровно один раз: метка + экран перехода.
// Отрисовка, границы навигации и переход выводятся из таблицы таких строк.
typedef struct MenuItem {
    LocaleKey label;
    ScreenId  target;
} MenuItem;

void ui_common_draw_header(const char *title);
void ui_common_draw_menu(const MenuItem *items, int count, int selected);
void ui_common_draw_battery_status(void);
void ui_common_draw_top_status(void);

// Нижняя полоса подсказок для произвольного числа меток.
// Вызывать через макрос ui_common_draw_prompts(...) — он сам считает count.
void ui_common_draw_prompts_n(const LocaleKey *keys, int count);
#define ui_common_draw_prompts(...) \
    ui_common_draw_prompts_n((const LocaleKey[]){ __VA_ARGS__ }, \
        (int)(sizeof((const LocaleKey[]){ __VA_ARGS__ }) / sizeof(LocaleKey)))

// Имя пресета эквалайзера (локализовано) + тост «EQ: ...» на 2.5 c
// после смены (кнопка ♪ или экран). Рисовать каждый кадр поверх всего.
const char *ui_common_eq_preset_name(int preset);
void ui_common_draw_eq_toast(void);

#endif
