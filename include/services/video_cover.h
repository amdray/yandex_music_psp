#ifndef YM_SERVICES_VIDEO_COVER_H
#define YM_SERVICES_VIDEO_COVER_H

#include <stddef.h>

typedef enum {
    VIDEO_COVER_IDLE = 0,
    VIDEO_COVER_CONVERTING,
    VIDEO_COVER_DOWNLOADING,
    VIDEO_COVER_READY,
    VIDEO_COVER_ERROR
} VideoCoverState;

int video_cover_init(void);
int video_cover_shutdown(void);
int video_cover_quiesce(void);

/* Latest request wins. Empty video_uri cancels the active request. */
int video_cover_request(const char *track_id, const char *video_uri);
void video_cover_clear(void);

VideoCoverState video_cover_state_for(const char *track_id, const char *video_uri);
int video_cover_ready_path(const char *track_id, const char *video_uri,
                           char *out, size_t out_size);

#endif
