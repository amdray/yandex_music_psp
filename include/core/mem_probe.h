#ifndef CORE_MEM_PROBE_H
#define CORE_MEM_PROBE_H

#include <psptypes.h>

#define MEM_PROBE_HISTORY_CAPACITY 224

typedef struct MemProbeSample {
    u64 time_us;
    u32 heap_configured;
    u32 heap_used;
    u32 partition_free;
    u32 partition_largest_free;
} MemProbeSample;

/* Sizes are bytes. heap_configured is zero without a positive heap setting.
 * Partition queries exclude the allocated newlib heap.
 * time_us is the caller's main-loop timestamp, not an atomic snapshot time. */

/* Collects at most once per five seconds. All access is on the main thread. */
void mem_probe_update(u64 now_us);

/* Copies the newest samples that fit, oldest first; no memory queries. */
int mem_probe_history_copy(MemProbeSample *out, int capacity);

/* Returns one when a sample was copied, zero when unavailable. */
int mem_probe_latest(MemProbeSample *out);

#endif /* CORE_MEM_PROBE_H */
