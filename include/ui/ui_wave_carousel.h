#ifndef YM_UI_WAVE_CAROUSEL_H
#define YM_UI_WAVE_CAROUSEL_H

#include "app/app_state.h"
#include "hal/hal_input.h"

// Карусель волн поверх Now Playing (спека 2.1/2.2): карточки контекста
// альбома + псевдо-карточка «Волна по треку». X открывает/стартует,
// O закрывает. Выбор карточки завершает текущий трек одним skip и
// запускает новую Rotor-сессию по её wave.seeds.
void carousel_open(AppState *state);
void carousel_close(void);
void carousel_update(AppState *state);
void carousel_input(AppState *state, const InputState *input);
void carousel_render(const AppState *state);
int carousel_is_open(void);

#endif
