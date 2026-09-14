#ifndef CORE_MEM_PROBE_H
#define CORE_MEM_PROBE_H

#include <stddef.h>

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

#endif /* CORE_MEM_PROBE_H */
