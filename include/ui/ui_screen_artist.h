#ifndef YM_UI_SCREEN_ARTIST_H
#define YM_UI_SCREEN_ARTIST_H

#include "app/app_state.h"
#include "hal/hal_input.h"

void ui_screen_artist_update(AppState *state);
int ui_screen_artist_content_ready(AppState *state);
void ui_screen_artist_reset(void);
void ui_screen_artist_handle_input(AppState *state, const InputState *input);
void ui_screen_artist_render(const AppState *state);

#endif
