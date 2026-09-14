#ifndef YM_SERVICES_COVER_MANAGER_H
#define YM_SERVICES_COVER_MANAGER_H

#include <psptypes.h>
#include "app/playlist.h"
#include "services/cover_storage.h"

#define PLAYLIST_VISIBLE_SLOTS 5

typedef enum {
    COVER_PRIORITY_VISIBLE = 0,
    COVER_PRIORITY_NEARBY = 1,
    COVER_PRIORITY_BACKGROUND = 2
} CoverPriority;

typedef void (*CoverLoadedCallback)(CoverEntityType type, int entity_id, void *user_data);

int cover_manager_init(void);
int cover_manager_shutdown(void);
int cover_manager_quiesce(void);

void cover_manager_set_visible_range(int start_idx, int end_idx,
                                     const PlaylistEntry *playlists, int playlist_count);

void cover_manager_request_cover(CoverEntityType type, int entity_id, const char *cover_uri,
                                  CoverPriority priority, const char *cover_size);

// Blit cover directly to the current draw framebuffer at (dst_x, dst_y),
// clamped to max_w x max_h. Returns 1 if drawn, 0 if not in cache.
int cover_manager_draw_cover(CoverEntityType type, int entity_id, const char *cover_size,
                              int dst_x, int dst_y, int max_w, int max_h);

// Copy cover pixel data into caller-owned buffer, row by row (handles stride mismatch).
// out_w/out_h/out_stride_bytes receive the actual dimensions written.
// Returns 1 if copied, 0 if not in cache or dst_buf_size too small.
int cover_manager_copy_cover(CoverEntityType type, int entity_id, const char *cover_size,
                              void *dst_buf, int dst_buf_size,
                              int *out_w, int *out_h, int *out_stride_bytes);

int cover_manager_is_loading(CoverEntityType type, int entity_id, const char *cover_size);

void cover_manager_process_pending(void);

void cover_manager_set_loaded_callback(CoverLoadedCallback callback, void *user_data);

// Browser-cover execution is allowed only in browsing mode.
void cover_manager_set_browsing_active(int active);
int cover_manager_is_browsing_active(void);

void cover_manager_pause_network(void);
void cover_manager_resume_network(void);

#endif
