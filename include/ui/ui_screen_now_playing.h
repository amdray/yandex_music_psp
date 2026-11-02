#ifndef YM_UI_SCREEN_NOW_PLAYING_H
#define YM_UI_SCREEN_NOW_PLAYING_H

#include "app/app_state.h"

void ui_screen_now_playing_update(AppState *state);
void ui_screen_now_playing_on_exit(AppState *state);
int ui_screen_now_playing_content_ready(AppState *state);
void ui_screen_now_playing_handle_input(AppState *state, const InputState *input);
void ui_screen_now_playing_render(const AppState *state);

#endif
