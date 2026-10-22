#ifndef YM_SERVICES_COVER_STORAGE_H
#define YM_SERVICES_COVER_STORAGE_H

#include <stddef.h>

typedef enum {
    COVER_ENTITY_PLAYLIST,  // Обложка плейлиста
    COVER_ENTITY_ALBUM,     // Обложка альбома
    COVER_ENTITY_ARTIST     // Аватар исполнителя
} CoverEntityType;

/* Диск cache/covers хранит не больше N самых свежих файлов: чистка
 * запускается после каждой новой загрузки (cover_manager /
 * cover_now_playing). Память под обложки ограничена отдельно (cover_cache). */
#define COVER_DISK_KEEP 64

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
