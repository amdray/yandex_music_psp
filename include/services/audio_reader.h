#ifndef YM_AUDIO_READER_H
#define YM_AUDIO_READER_H

#include "services/audio_cache.h"

typedef struct AudioReader AudioReader;
typedef void (*AudioReaderBuffering)(void *user);

/* Single active playback source. Mutex lives for the application lifetime. */
int audio_reader_init(void);
/* Takes over one acquired cache source, including on failure.
 * The source must specify its full content_length. */
AudioReader *audio_reader_open(const AudioSourceSnapshot *source,
                               AudioReaderBuffering buffering, void *user);
const AudioSourceSnapshot *audio_reader_source(const AudioReader *reader);
/* Full request or a short read at confirmed EOF; negative = error/cancel.
 * Only copies RAM and waits for the reader worker; no filesystem calls. */
int audio_reader_read_at(AudioReader *reader, int64_t offset, void *out, int count);
int audio_reader_error(AudioReader *reader);
int audio_reader_at_end(AudioReader *reader);
/* Wakes both worker and consumer. Safe while open/close is in progress. */
void audio_reader_cancel(void);
/* Call after the decoder has stopped. Joins worker before closing the file
 * and releasing the source. A failed cleanup retains ownership for retry. */
int audio_reader_close(void);

#endif
