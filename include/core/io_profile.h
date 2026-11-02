#ifndef YM_IO_PROFILE_H
#define YM_IO_PROFILE_H
#include <stdint.h>

enum { IP_AUDIO, IP_SOURCE_READER, IP_WRITER0, IP_WRITER1, IP_LANES };
enum { IP_OTHER, IP_WAIT, IP_DECODE, IP_PROCESS, IP_OUTPUT, IP_IO,
       IP_OVERHEAD, IP_PHASES, IP_FS_WAIT = IP_PHASES, IP_FS_HELD,
       IP_READ, IP_WRITE, IP_SEEK, IP_SOURCE_BUFFER, IP_WRITE_BUFFER, IP_AUDIO_SUBMIT };
typedef struct IoProfileLane {
    uint64_t start, last;
    uint32_t phase, lane;
} IoProfileLane;

void io_profile_begin(IoProfileLane *p, unsigned lane);
void io_profile_phase(IoProfileLane *p, unsigned phase);
void io_profile_end(IoProfileLane *p);
void io_profile_event(unsigned kind, int key, int value,
                      uint64_t start, uint64_t end);
/* Called only after every measured worker has joined. Freezes collection. */
int io_profile_save(void);
#endif
