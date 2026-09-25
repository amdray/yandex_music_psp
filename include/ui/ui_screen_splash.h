#ifndef YM_UI_SCREEN_SPLASH_H
#define YM_UI_SCREEN_SPLASH_H

#include "app/app_state.h"
#include "hal/hal_input.h"
#include "services/splash_flow.h"

void ui_screen_splash_update(AppState *state, SplashFlow *flow);
void ui_screen_splash_handle_input(AppState *state, const InputState *input, SplashFlow *flow);
void ui_screen_splash_render(const SplashFlow *flow);

#endif
