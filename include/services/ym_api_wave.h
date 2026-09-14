#ifndef YM_SERVICES_YM_API_WAVE_H
#define YM_SERVICES_YM_API_WAVE_H

#include "services/ym_api_types.h"
#include "services/list_index.h"

// Персональная волна: GET /rotor/station/{id}/tracks.
// id волны пользователя: "user:onyourwave". Отдаёт пачку треков
// (sequence[].track), только id — метаданные подтянет гидратор.
// Фидбэк (лайки/скипы на станцию) v1 не шлём.
#define YM_API_WAVE_TRACKS_MAX 32
#define WAVE_STATION_MY "user:onyourwave"

int ym_api_wave_station_tracks(YmApiContext *ctx, const char *station_id,
                               ListIndexId *out_ids, int max_count);

#endif
