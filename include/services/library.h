#ifndef YM_SERVICES_LIBRARY_H
#define YM_SERVICES_LIBRARY_H

#include "app/app_state.h"
#include "app/playlist.h"

/* library — фасад между UI-экранами и сетевыми/стор-сервисами.
 *
 * Экраны НЕ должны напрямую брать net_client_track_store_lock(), читать
 * track_store, дёргать generation воркеров или читать токен. Всё это спрятано
 * здесь. Экран остаётся при отрисовке + вызовах интентов ниже.
 */

/* Обслужить загрузку плейлистов: poll, обновить freshness-метки, и, если
 * загрузка вкладки `tab` (0=личные, 1=лайкнутые) простаивает — запустить её.
 * Возвращает текущий статус этой вкладки. Токен читается внутри. */
PlaylistLoadStatus library_playlists_service(AppState *s, int tab);

/* При (повторном) входе на экран: сбросить устаревшие/ошибочные загрузки в IDLE,
 * чтобы они перезапросились (ловит правки, сделанные на другом устройстве). */
void library_playlists_refresh_on_enter(AppState *s);

/* Открыть плейлист — начать стриминг его треков. Прячет лок стора, бамп
 * generation, сброс bootstrap, чтение токена и старт воркера. `selected_index` —
 * позиция строки в текущей вкладке. Возвращает 0 при успехе, <0 при ошибке
 * (track_boot.status выставляется в ERROR внутри). */
int library_open_playlist(AppState *s, const PlaylistEntry *entry, int tab, int selected_index);

/* Гейт перехода в track_list: 1, когда видимое окно у разрешённой входной
 * позиции гидрировано (заполняет *out_pos и *out_count); 0, пока грузится или
 * bootstrap не идёт. Прячет лок стора, разрешение anchor и подкачку окна
 * (токен читается внутри). Аргументы могут быть NULL. */
int library_track_entry_ready(AppState *s, int *out_pos, int *out_count);

#endif
