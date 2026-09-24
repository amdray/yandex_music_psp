#ifndef YM_UI_SCREEN_MENU_H
#define YM_UI_SCREEN_MENU_H

#include "app/app_state.h"
#include "hal/hal_input.h"

void ui_screen_menu_handle_input(AppState *state, const InputState *input);
void ui_screen_menu_render(const AppState *state);
void ui_screen_settings_handle_input(AppState *state, const InputState *input);
void ui_screen_settings_render(const AppState *state);

#endif
