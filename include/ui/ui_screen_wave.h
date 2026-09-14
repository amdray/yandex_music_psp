#ifndef YM_UI_SCREEN_WAVE_H
#define YM_UI_SCREEN_WAVE_H

#include "app/app_state.h"
#include "hal/hal_input.h"

// Экран «Моя волна»: на входе тянет первую пачку станции,
// кладёт в очередь (SOURCE_FLOW) и уходит на плеер.
// Дальше пачки подтягивает wave_service из now_playing.
void ui_screen_wave_on_enter(AppState *state);
void ui_screen_wave_on_exit(AppState *state);
void ui_screen_wave_update(AppState *state);
void ui_screen_wave_render(const AppState *state);

#endif
