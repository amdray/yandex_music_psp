#ifndef YM_SERVICES_YM_API_GENRES_H
#define YM_SERVICES_YM_API_GENRES_H

#include <stddef.h>

/* Loads the localized Yandex genre catalog. The previous catalog, if any,
 * remains valid when loading fails. */
int ym_api_genres_load(const char *token, const char *language);

/* Copies the localized title for id into out. Returns 0 when found. */
int ym_api_genres_resolve(const char *id, char *out, size_t out_size);

#endif
