#ifndef YM_UI_SCREEN_ACCOUNT_H
#define YM_UI_SCREEN_ACCOUNT_H

#include "app/app_state.h"
#include "hal/hal_input.h"

void ui_screen_account_render(const AppState *state);
void ui_screen_account_handle_input(AppState *state, const InputState *input);

#endif
