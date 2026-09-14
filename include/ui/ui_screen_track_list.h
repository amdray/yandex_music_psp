#ifndef YM_UI_SCREEN_TRACK_LIST_H
#define YM_UI_SCREEN_TRACK_LIST_H

#include "app/app_state.h"
#include "hal/hal_input.h"

void ui_screen_track_list_update(AppState *state);
void ui_screen_track_list_handle_input(AppState *state, const InputState *input);
void ui_screen_track_list_render(const AppState *state);
void ui_screen_track_list_on_exit(AppState *state);
void ui_screen_track_list_clear_cache(void);  // Очистка кэша видимого диапазона при переходе на экран

#endif
