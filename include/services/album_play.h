#ifndef YM_SERVICES_ALBUM_PLAY_H
#define YM_SERVICES_ALBUM_PLAY_H

// Открытие альбома: worker тянет треки (/albums/{id}/with-tracks),
// кладёт в очередь (SOURCE_ALBUM) и сообщает готовность. Экран сам
// ставит now_playing_track, пушит NOW_PLAYING и жмёт play_current.
// Один воркер на всех (экраны альбомов и меню исполнителя делят).

void album_play_start(const char *token, int album_id);
// 0 идёт, 1 готово (first_id заполнен), -1 провал.
int album_play_poll(char *out_first_id, int id_size);
int album_play_busy(void);
// Остановить/дождаться (выход с экрана). Не удаляет готовый результат.
void album_play_reset(void);

#endif
