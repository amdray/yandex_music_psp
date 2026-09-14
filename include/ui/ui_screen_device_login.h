#ifndef YM_UI_SCREEN_DEVICE_LOGIN_H
#define YM_UI_SCREEN_DEVICE_LOGIN_H

#include "app/app_state.h"
#include "hal/hal_input.h"

// Пункт главного меню «Профиль»: вход по коду с телефона через ya_auth
// (как SCR_LOGIN в psp_yandex). Показывает код, опрашивает токен,
// сохраняет в config/token.txt и подхватывает профиль в currentUser.
void ui_screen_device_login_on_enter(AppState *state);
void ui_screen_device_login_on_exit(AppState *state);
void ui_screen_device_login_update(AppState *state);
void ui_screen_device_login_handle_input(AppState *state, const InputState *input);
void ui_screen_device_login_render(const AppState *state);

#endif
