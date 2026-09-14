#include "services/cover_storage.h"

#include <stdio.h>
#include <string.h>

void cover_storage_build_path(CoverEntityType type, int entity_id, const char *size,
                               char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0) {
        return;
    }
    
    // Префикс типа: 'p' для плейлиста, 'a' для альбома, 'r' для исполнителя
    const char *prefix = (type == COVER_ENTITY_PLAYLIST) ? "p" :
                         (type == COVER_ENTITY_ARTIST)   ? "r" : "a";
    
    // Размер по умолчанию
    const char *cover_size = (size && size[0]) ? size : "30x30";
    
    // Формат: data/cache/covers/p1068_30x30 или data/cache/covers/a6216838_200x200
    snprintf(out_path, out_size, "data/cache/covers/%s%d_%s", prefix, entity_id, cover_size);
    out_path[out_size - 1] = '\0';
}
