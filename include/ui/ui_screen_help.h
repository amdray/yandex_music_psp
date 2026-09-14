#ifndef YM_UI_SCREEN_HELP_H
#define YM_UI_SCREEN_HELP_H

#include "app/app_state.h"
#include "hal/hal_input.h"

// Экран «Помощь»: список кнопок и комбинаций. Только отрисовка,
// назад — generic Circle-pop.
void ui_screen_help_render(const AppState *state);

#endif
