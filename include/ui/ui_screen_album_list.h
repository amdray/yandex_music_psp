#ifndef YM_UI_SCREEN_ALBUM_LIST_H
#define YM_UI_SCREEN_ALBUM_LIST_H

#include "app/app_state.h"
#include "hal/hal_input.h"

void ui_screen_album_list_handle_input(AppState *state, const InputState *input);
void ui_screen_album_list_render(void);

#endif
