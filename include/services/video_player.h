#ifndef SERVICES_VIDEO_PLAYER_H
#define SERVICES_VIDEO_PLAYER_H

/* Cover-video playback through the standard sceMpeg PSMF ring-buffer API.
 * The proxy supplies 200x200 content centred in a 208x208 coded frame. The UI
 * masks the 4px transport border in GE and copies 1:1. No custom PRX, raw-NAL
 * API, CPU decode, scaling or repacking on PSP.
 *
 * Contract: this is a progressive enhancement over the static JPEG cover. Any
 * failure (missing file, ME init/decode error) returns NULL and the caller keeps
 * drawing the static cover — video must never be able to break now-playing. */

typedef struct VideoPlayer VideoPlayer;

/* Load PSMF from `path` and initialise the ME decoder. Returns NULL on any
 * failure (caller falls back to the static cover). */
VideoPlayer *video_player_open(const char *path);

/* Decode the next frame, looping at end-of-stream. Returns a pointer to a
 * width*height 8888 image (owned by the player, valid until the next call or
 * close), or NULL on decode failure. Frame size is written to out_w and out_h. */
const void *video_player_next_frame(VideoPlayer *vp, int *out_w, int *out_h);

/* Distinguishes an unavailable frame from a fatal stream/API error. Note that
 * sceMpegAvcDecode's iInit output is 0 on its first successful call; it is not
 * a picture-count or failure indicator. */
int video_player_failed(const VideoPlayer *vp);

/* Proxy-normalized frame rate (30000/1001). */
int video_player_fps(const VideoPlayer *vp);

/* Duration of a normalized frame. */
unsigned int video_player_frame_duration_us(const VideoPlayer *vp);

/* Texture-buffer width/stride in pixels. */
int video_player_tex_dim(const VideoPlayer *vp);

void video_player_close(VideoPlayer *vp);

#endif /* SERVICES_VIDEO_PLAYER_H */
