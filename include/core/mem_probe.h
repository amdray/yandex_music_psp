#ifndef CORE_MEM_PROBE_H
#define CORE_MEM_PROBE_H

#include <stddef.h>
#include <psptypes.h>

#define MEM_PROBE_HISTORY_CAPACITY 224

typedef struct MemProbeSample {
    u32 heap_used;
    u32 heap_cap;
    u32 largest_free;
} MemProbeSample;

/* Heap vs partition — two different pools, measured by two different calls.
 *
 * All malloc/memalign/calloc draw from the newlib heap, whose ceiling is set by
 * PSP_HEAP_SIZE_KB. sceKernelMaxFreeMemSize() does NOT see inside that heap — it
 * reports the user partition OUTSIDE it. To watch how close the app is to running
 * out of dynamic memory, use the heap_* calls, not partition_free. */

/* Configured heap ceiling in bytes (from sce_newlib_heap_kb_size).
 * 0 when the heap is sized dynamically (negative PSP_HEAP_SIZE_KB): no fixed cap. */
size_t mem_heap_cap(void);

/* Bytes currently in use inside the heap (mallinfo uordblks). */
size_t mem_heap_used(void);

/* Bytes still allocatable from the heap before hitting the cap (cap - used).
 * 0 when cap is unknown (dynamic sizing). */
size_t mem_heap_free(void);

/* Largest free block in the user partition OUTSIDE the heap
 * (sceKernelMaxFreeMemSize). This is headroom for raising the cap, NOT memory
 * that malloc can reach at the current cap. */
size_t mem_partition_free(void);

/* Samples once per five seconds. Call only from the main thread. */
void mem_probe_update(u64 now_us);

/* Copies samples oldest first. No allocation and no PSP memory query here. */
int mem_probe_history_copy(MemProbeSample *out, int capacity);
int mem_probe_latest(MemProbeSample *out);

#endif /* CORE_MEM_PROBE_H */
