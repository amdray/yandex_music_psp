#ifndef YM_UI_SCREEN_ARTIST_MENU_H
#define YM_UI_SCREEN_ARTIST_MENU_H

#include "app/app_state.h"

void ui_screen_artist_menu_update(AppState *state);
int ui_screen_artist_menu_content_ready(AppState *state);
void ui_screen_artist_menu_reset(void);
void ui_screen_artist_menu_handle_input(AppState *state, const InputState *input);
void ui_screen_artist_menu_render(const AppState *state);

#endif
