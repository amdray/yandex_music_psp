#ifndef YM_CORE_FS_H
#define YM_CORE_FS_H

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stddef.h>
#include <stdint.h>

// Lifecycle management
int fs_init(void);
void fs_shutdown(void);

// From old fs.h
SceUID fs_open(const char *path, int flags, int mode);
int fs_close(SceUID fd);
int fs_read(SceUID fd, void *buf, size_t size);
int fs_write(SceUID fd, const void *buf, size_t size);
SceOff fs_lseek(SceUID fd, SceOff offset, int whence);
int fs_getstat(const char *path, SceIoStat *st);
int fs_remove(const char *path);
int fs_rename(const char *old_path, const char *new_path);
// Remove entries in `dir` whose name starts with `prefix`, except `keep_name`.
int fs_remove_siblings(const char *dir, const char *prefix, const char *keep_name);
int fs_mkdir(const char *path, int mode);
int fs_write_atomic(const char *path, const void *data, size_t size);
/* Отложенная запись небольших конфигов (<=256 Б): возвращается сразу, пишет
 * фоновый поток через ~1.2 с после последнего вызова (повторные записи того
 * же пути склеиваются). Запись на карту PSP занимает секунды — из UI-потока
 * её звать нельзя. fs_flush_deferred() — дописать всё синхронно (выход). */
int fs_write_deferred(const char *path, const void *data, size_t size);
void fs_flush_deferred(void);

// From new fs.h
int fs_get_cwd(char *out, size_t size);
int fs_make_abs_path(const char *path, char *out, size_t size);
void fs_log_open(const char *path);
int fs_ensure_dir(const char *path);
// Удалить самые старые файлы каталога, оставив keep самых свежих.
void fs_evict_oldest(const char *dir, int keep);
/* Удалить все файлы каталога, кроме тех, чьё имя (до первой точки)
 * совпадает с одним из keep_ids. Пустые keep_ids пропускаются.
 * Политика «текущий + предзагруженный». Вызывать из worker-потоков. */
void fs_evict_except(const char *dir, const char *const *keep_ids,
                     int keep_count);

#endif
