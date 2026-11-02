/* Проверен и зафиксирован: формирование путей дискового кэша обложек
 * и проверка доступности файлов с различением отсутствия и ошибки доступа. */
#include "services/cover_storage.h"
#include "core/fs.h"

#include <stdio.h>

/* sceIoOpen: SCE_ERROR_ERRNO_FILE_NOT_FOUND. */
#define COVER_FILE_NOT_FOUND ((SceUID)0x80010002)

int cover_storage_contains(const char *path)
{
    SceUID fd = fs_open(path, PSP_O_RDONLY, 0);
    if (fd == COVER_FILE_NOT_FOUND) return 0;
    if (fd < 0) return -1;
    if (fs_close(fd) < 0) return -1;
    return 1;
}

int cover_storage_build_path(CoverEntityType type, int entity_id, const char *size,
                               char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0) {
        return -1;
    }
    
    // Префикс типа: 'p' для плейлиста, 'a' для альбома, 'r' для исполнителя
    const char *prefix;
    out_path[0] = '\0';
    switch (type) {
    case COVER_ENTITY_PLAYLIST: prefix = "p"; break;
    case COVER_ENTITY_ALBUM: prefix = "a"; break;
    case COVER_ENTITY_ARTIST: prefix = "r"; break;
    default: return -1;
    }
    
    // Размер по умолчанию
    const char *cover_size = (size && size[0]) ? size : "30x30";
    
    // Формат: data/cache/covers/p1068_30x30 или data/cache/covers/a6216838_200x200
    int length = snprintf(out_path, out_size, "data/cache/covers/%s%d_%s",
                          prefix, entity_id, cover_size);
    if (length < 0 || (size_t)length >= out_size) {
        out_path[0] = '\0';
        return -1;
    }
    return 0;
}
