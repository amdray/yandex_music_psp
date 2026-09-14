#ifndef YM_UI_SCREEN_SEARCH_H
#define YM_UI_SCREEN_SEARCH_H

#include "app/app_state.h"
#include "hal/hal_input.h"

// Экран поиска треков Яндекс Музыки.
// on_enter: если запроса ещё нет — один блокирующий вызов OSK
// (osk_input_text), затем фоновый воркер тянет id через
// ym_api_search_tracks. X на строке = играть с этой позиции
// (очередь + NOW_PLAYING, как топ треков артиста). O = назад
// (интегратор НЕ ставит owns_back — выход штатным pop).
// Квадрат на экране = новый запрос.
void ui_screen_search_on_enter(AppState *state);
void ui_screen_search_on_exit(AppState *state);
void ui_screen_search_update(AppState *state);
void ui_screen_search_handle_input(AppState *state, const InputState *input);
void ui_screen_search_render(const AppState *state);

#endif
