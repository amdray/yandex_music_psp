#ifndef YM_AUDIO_STREAM_BUF_H
#define YM_AUDIO_STREAM_BUF_H

#include <stdint.h>
#include <pspthreadman.h>

/*
 * Linear buffer for live MP3 streaming.
 *
 * Producer (download thread): appends raw bytes from network without blocking.
 * Consumer (audio_player thread): reads by absolute source_pos.
 *
 * Buffer is allocated in asb_alloc() to exactly content_length bytes so the
 * producer never stalls — TCP window stays open, CDN never RSTs.
 *
 * Valid data: [0, write_pos). Bytes remain readable until reset/destroy so a
 * new decoder session can restart the same source from the beginning.
 * Thread safety: mutex guards all fields.
 */

typedef struct {
    uint8_t            *buf;
    int64_t             capacity;       /* allocated size = content_length */
    int64_t             write_pos;      /* abs offset of next byte to write */
    int64_t             content_length; /* -1 until known */
    int                 complete;       /* producer signalled end */
    int                 error;          /* producer signalled error */
    SceLwMutexWorkarea  mutex;
    int                 initialized;
} AudioStreamBuf;

typedef struct {
    int64_t capacity;
    int64_t write_pos;
    int64_t content_length;
    int     complete;
    int     error;
} AudioStreamBufSnapshot;

/* Lifecycle. */
void    asb_init(AudioStreamBuf *b);
/* Allocate buffer for a new download. Must be called before asb_append().
 * content_length must be > 0. Returns 0 on success, -1 if malloc failed. */
int     asb_alloc(AudioStreamBuf *b, int64_t content_length);
void    asb_reset(AudioStreamBuf *b);   /* free buf, clear state */
void    asb_destroy(AudioStreamBuf *b);

/* Producer API. */
/* Append `len` bytes. Never blocks. Returns 0 ok, -1 on overflow or error. */
int     asb_append(AudioStreamBuf *b, const uint8_t *data, int len);
void    asb_set_complete(AudioStreamBuf *b);
void    asb_set_error(AudioStreamBuf *b);
void    asb_set_cancelled(AudioStreamBuf *b);

/* Consumer API. */
/* Read up to `want` bytes starting at absolute `source_pos`.
 * Returns bytes copied (0 = not yet available, -1 = error). */
int     asb_read_at(AudioStreamBuf *b, int64_t source_pos,
                    uint8_t *dst, int want);
/* Compare already buffered bytes without exposing the backing pointer. */
int     asb_compare_at(AudioStreamBuf *b, int64_t source_pos,
                       const uint8_t *data, int len);

/* Coherent state for decisions that must not observe torn 64-bit fields. */
int     asb_get_snapshot(AudioStreamBuf *b, AudioStreamBufSnapshot *out);

/* Read-only accessors. */
int64_t asb_write_pos(AudioStreamBuf *b);
int64_t asb_content_length(AudioStreamBuf *b);
int     asb_is_complete(AudioStreamBuf *b);
int     asb_is_error(AudioStreamBuf *b);

#endif /* YM_AUDIO_STREAM_BUF_H */
