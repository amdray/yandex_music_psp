#ifndef COVER_NOW_PLAYING_H
#define COVER_NOW_PLAYING_H

typedef struct {
    void *rgba_data;
    int w;
    int h;
    int stride_bytes;
    int loaded;
    int album_id;
    char cover_uri[256];
} NowPlayingCover;

int cover_now_playing_init(void);
int cover_now_playing_shutdown(void);
int cover_now_playing_quiesce(void);

int cover_now_playing_request_load(int album_id, const char *cover_uri);
void cover_now_playing_prefetch(int album_id, const char *cover_uri);
void cover_now_playing_clear(void);

const NowPlayingCover *cover_now_playing_get_for(int album_id, const char *cover_uri);

int cover_now_playing_is_loading(void);
void cover_now_playing_process_pending(void);

#endif
