#ifndef YM_SERVICES_YM_API_LIKE_H
#define YM_SERVICES_YM_API_LIKE_H

#include "services/ym_api_types.h"

// Лайк/анлайк трека (эндпоинт проверен в psp_yandex):
// POST /users/{uid}/likes/tracks/{add-multiple|remove}?track-ids={id}
// с пустым телом. Возврат 0 = сервер принял.
int ym_api_track_like(YmApiContext *ctx, int uid,
                      const char *track_id, int like);

#endif
