#ifndef YM_UI_SCREEN_PLAYLIST_LIST_H
#define YM_UI_SCREEN_PLAYLIST_LIST_H

#include "app/app_state.h"
#include "hal/hal_input.h"

void ui_screen_playlist_list_on_enter(AppState *state);
int ui_screen_playlist_list_content_ready(AppState *state);
void ui_screen_playlist_list_update(AppState *state);
void ui_screen_playlist_list_handle_input(AppState *state, const InputState *input);
void ui_screen_playlist_list_render(const AppState *state);

#endif
