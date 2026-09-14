#ifndef YM_SERVICES_COVER_STORAGE_H
#define YM_SERVICES_COVER_STORAGE_H

#include <stddef.h>

typedef enum {
    COVER_ENTITY_PLAYLIST,  // Обложка плейлиста
    COVER_ENTITY_ALBUM,     // Обложка альбома
    COVER_ENTITY_ARTIST     // Аватар исполнителя
} CoverEntityType;

/** Формирует относительный путь в кэше обложек (без расширения).
 * @param type Тип сущности (плейлист или альбом)
 * @param entity_id ID плейлиста или альбома
 * @param size Размер обложки (например "30x30" или "200x200"), может быть NULL для дефолтного
 * @param out_path Буфер для результата
 * @param out_size Размер буфера
 */
void cover_storage_build_path(CoverEntityType type, int entity_id, const char *size, 
                               char *out_path, size_t out_size);

#endif
