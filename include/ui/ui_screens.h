#ifndef YM_UI_SCREENS_H
#define YM_UI_SCREENS_H

#include "app/app_state.h"
#include "hal/hal_input.h"

void ui_screens_update(AppState *state, const InputState *input);
void ui_screens_handle_input(AppState *state, const InputState *input);
void ui_screens_render(const AppState *state);
int ui_screens_init(void);
int ui_screens_shutdown(void);

/* Navigation gate (screen-transition contract): if the target screen declares
   a content_ready hook, the push is deferred until its visible content is
   loaded; meanwhile the source screen stays interactive and shows a pending
   indicator. Targets without the hook push immediately. Any new input on the
   source screen cancels the pending navigation. */
void ui_screens_navigate(AppState *state, ScreenId target);
ScreenId ui_screens_nav_pending(void);
/* Ручной выход экрана с owns_back=1 (эквивалент generic Circle-pop). */
void ui_screens_pop_screen(AppState *state);

#endif
