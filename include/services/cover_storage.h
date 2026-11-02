#ifndef YM_SERVICES_COVER_STORAGE_H
#define YM_SERVICES_COVER_STORAGE_H

#include <stddef.h>

typedef enum {
    COVER_ENTITY_PLAYLIST,  // Обложка плейлиста
    COVER_ENTITY_ALBUM,     // Обложка альбома
    COVER_ENTITY_ARTIST     // Аватар исполнителя
} CoverEntityType;

/* 1: readable file; 0: file not found; -1: storage access failed. */
int cover_storage_contains(const char *path);

/** Формирует относительный путь в кэше обложек (без расширения).
 * @param type Тип сущности (плейлист, альбом или исполнитель)
 * @param entity_id ID сущности
 * @param size Размер обложки (например "30x30" или "200x200"), может быть NULL для дефолтного
 * @param out_path Буфер для результата
 * @param out_size Размер буфера
 * @return 0 при успехе, -1 при неизвестном типе или недостаточном буфере.
 */
int cover_storage_build_path(CoverEntityType type, int entity_id, const char *size,
                               char *out_path, size_t out_size);

#endif
