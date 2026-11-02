#include "core/io_profile.h"
#include "core/fs.h"
#include <pspthreadman.h>
#include <string.h>

/* Diagnostic budget: 1 MiB, ~3.4% of the 31 MiB free in the captured run.
 * No allocation or card writes on the measured path. Saturation is explicit. */
typedef struct Event {
    uint64_t start, end;
    uint32_t kind;
    int32_t key, value;
    volatile uint32_t committed;
} Event;
#define EVENT_CAP ((1024 * 1024) / sizeof(Event))
typedef struct Totals {
    uint64_t wall, time[IP_PHASES], maximum[IP_PHASES];
    uint32_t count[IP_PHASES];
} Totals;
static Event s_events[EVENT_CAP];
static Totals s_totals[IP_LANES]; /* each lane has exactly one writer */
static volatile unsigned s_next;
#define FROZEN (1U << 31)

void io_profile_event(unsigned kind, int key, int value,
                      uint64_t start, uint64_t end)
{
    unsigned index;
    Event *e;
    do {
        index = s_next;
        if (index & FROZEN) return;
    } while (!__sync_bool_compare_and_swap(&s_next, index, index + 1));
    if (index >= EVENT_CAP) return;
    e = &s_events[index];
    e->start = start;
    e->end = end;
    e->kind = kind;
    e->key = key;
    e->value = value;
    __sync_synchronize();
    e->committed = 1;
}

static void account(IoProfileLane *p, unsigned phase, uint64_t duration)
{
    Totals *t = &s_totals[p->lane];
    t->time[phase] += duration;
    if (duration > t->maximum[phase]) t->maximum[phase] = duration;
    t->count[phase]++;
}

void io_profile_begin(IoProfileLane *p, unsigned lane)
{
    p->lane = lane;
    p->phase = IP_OTHER;
    p->start = p->last = sceKernelGetSystemTimeWide();
}

void io_profile_phase(IoProfileLane *p, unsigned phase)
{
    uint64_t now = sceKernelGetSystemTimeWide();
    uint64_t end;
    account(p, p->phase, now - p->last);
    io_profile_event(p->phase, (int)p->lane, 0, p->last, now);
    end = sceKernelGetSystemTimeWide();
    account(p, IP_OVERHEAD, end - now);
    p->last = end;
    p->phase = phase;
}

void io_profile_end(IoProfileLane *p)
{
    io_profile_phase(p, IP_OTHER);
    s_totals[p->lane].wall += p->last - p->start;
}

static int write_all(int fd, const void *data, unsigned size)
{
    const char *p = data;
    while (size) {
        int n = fs_write(fd, p, size);
        if (n <= 0) return -1;
        p += n;
        size -= (unsigned)n;
    }
    return 0;
}

int io_profile_save(void)
{
    /* Binary LE format: eight u32 header words, Totals[lanes], Event[count].
     * Totals have only 8-byte-aligned u64 arrays followed by u32 counts;
     * sizes in the header include trailing ABI padding. */
    uint32_t header[8];
    unsigned count, total;
    int fd, rc;
    total = __sync_fetch_and_or(&s_next, FROZEN);
    count = total < EVENT_CAP ? total : EVENT_CAP;
    header[0] = 0x49504d59; /* YMPI */
    header[1] = 1;
    header[2] = sizeof(Totals);
    header[3] = IP_LANES;
    header[4] = sizeof(Event);
    header[5] = count;
    header[6] = total - count;
    header[7] = IP_PHASES;
    fd = fs_open("data/io_profile.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return fd;
    rc = write_all(fd, header, sizeof(header));
    if (rc == 0) rc = write_all(fd, s_totals, sizeof(s_totals));
    /* Only committed records are read. A concurrent non-audio FS operation
     * may still be finishing when recording is frozen: save a zero slot. */
    /* Same 16 KiB output batch as logger.c; no per-event card writes. */
    for (unsigned i = 0; rc == 0 && i < count;) {
        static Event batch[(16 * 1024) / sizeof(Event)];
        unsigned n = count - i;
        if (n > sizeof(batch) / sizeof(batch[0])) n = sizeof(batch) / sizeof(batch[0]);
        memset(batch, 0, n * sizeof(batch[0]));
        for (unsigned j = 0; j < n; ++j) {
            if (s_events[i + j].committed) {
                __sync_synchronize();
                batch[j] = s_events[i + j];
            }
        }
        rc = write_all(fd, batch, n * sizeof(batch[0]));
        i += n;
    }
    if (fs_close(fd) < 0) rc = -1;
    return rc;
}
