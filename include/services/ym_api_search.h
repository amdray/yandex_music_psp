#ifndef YM_SERVICES_YM_API_SEARCH_H
#define YM_SERVICES_YM_API_SEARCH_H

#include "services/ym_api_types.h"
#include "services/list_index.h"

// Поиск треков: GET /search?text={urlencoded}&type=track&page=0
// -> result.tracks.results[].id (string или number).
// Только id (метаданные подтянет гидратор / очередь через PENDING).
// Возврат: число id (1..YM_API_SEARCH_MAX) или -1 при ошибке.
#define YM_API_SEARCH_MAX 64
int ym_api_search_tracks(YmApiContext *ctx, const char *query,
                         ListIndexId *out_ids, int max_ids);

#endif
