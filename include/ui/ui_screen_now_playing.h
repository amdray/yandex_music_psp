#ifndef YM_UI_SCREEN_NOW_PLAYING_H
#define YM_UI_SCREEN_NOW_PLAYING_H

#include "app/app_state.h"

void ui_screen_now_playing_update(AppState *state);
void ui_screen_now_playing_handle_input(AppState *state, const InputState *input);
void ui_screen_now_playing_render(const AppState *state);

/* Release the cover-video ME decoder. Call on leaving now-playing so the ME does
 * not stay held (it may contend with sceMp3 audio). */
void ui_screen_now_playing_release_video(void);

#endif
