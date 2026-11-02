#ifndef YM_SERVICES_LAST_PLAY_H
#define YM_SERVICES_LAST_PLAY_H

#include "app/app_state.h"

// Запоминание последнего воспроизведения: очередь, ID сессии Волны, трек и позиция.
// Сохранение выполняется при старте каждого трека и на чистом выходе.

// Сохранить текущую очередь. Тихо ничего не делает, если играть нечего.
void last_play_save(void);
int last_play_quiesce(void);

/* Persistent startup playback setting. Missing config means disabled. */
int last_play_autostart_enabled(void);
void last_play_set_autostart(int enabled);

// Поднять сохранённую очередь независимо от автозапуска. Если autoplay != 0,
// запустить текущий трек с сохранённой позиции. 0 = очередь загружена.
int last_play_restore(AppState *state, int autoplay);

#endif
