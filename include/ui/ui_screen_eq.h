#ifndef YM_UI_SCREEN_EQ_H
#define YM_UI_SCREEN_EQ_H

#include "app/app_state.h"
#include "hal/hal_input.h"

// Экран «Эквалайзер»: строка профиля (влево/вправо — смена) +
// 7 полос кастомного профиля (вверх/вниз — полоса, влево/вправо — дБ).
// Правка полосы копирует текущий звук в кастом и переключает на него.
// Сохранение — при выходе с экрана.
void ui_screen_eq_on_enter(AppState *state);
void ui_screen_eq_on_exit(AppState *state);
void ui_screen_eq_handle_input(AppState *state, const InputState *input);
void ui_screen_eq_render(const AppState *state);

#endif
