#ifndef YM_SERVICES_WAVE_H
#define YM_SERVICES_WAVE_H

// Персональная волна: первая пачка (экран) + догрузка следующих пачек
// в фоне (монитор в now_playing update). Фидбэк на станцию v1 не шлём —
// просто берём свежие пачки, повторов почти нет.

void wave_start_first(const char *token);
// 0 идёт, 1 готово (first_id заполнен), -1 провал. Одноразовый.
int wave_poll_first(char *out_first_id, int id_size);
// Держать волну бесконечной: вызывать каждый кадр из now_playing.
void wave_service(void);
void wave_reset(void);

#endif
