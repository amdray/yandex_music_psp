#include "core/mem_probe.h"

#include <malloc.h>
#include <pspsysmem.h>

#define MEM_PROBE_INTERVAL_US 5000000ULL

static MemProbeSample s_history[MEM_PROBE_HISTORY_CAPACITY];
static int s_history_start = 0;
static int s_history_count = 0;
static u64 s_last_sample_us = 0;

/* Defined by PSP_HEAP_SIZE_KB(...) in main.c (expands to
 * `int sce_newlib_heap_kb_size = <kb>`). Positive = fixed heap size in KB;
 * negative = "all free minus |value| KB" (dynamic, no fixed cap). */
extern int sce_newlib_heap_kb_size;

size_t mem_heap_cap(void)
{
    int kb = sce_newlib_heap_kb_size;
    if (kb <= 0) return 0;                 /* dynamic sizing: no fixed ceiling */
    return (size_t)kb * 1024u;
}

size_t mem_heap_used(void)
{
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks;
}

size_t mem_heap_free(void)
{
    size_t cap  = mem_heap_cap();
    size_t used = mem_heap_used();
    if (cap == 0) return 0;
    return cap > used ? cap - used : 0;
}

size_t mem_partition_free(void)
{
    int f = sceKernelMaxFreeMemSize();
    return f > 0 ? (size_t)f : 0;
}

void mem_probe_update(u64 now_us)
{
    MemProbeSample sample;
    int slot;

    if (s_history_count > 0 && now_us - s_last_sample_us < MEM_PROBE_INTERVAL_US) {
        return;
    }

    sample.heap_cap = (u32)mem_heap_cap();
    sample.heap_used = (u32)mem_heap_used();
    if (sample.heap_used > sample.heap_cap && sample.heap_cap != 0) {
        sample.heap_used = sample.heap_cap;
    }
    sample.largest_free = (u32)mem_partition_free();

    if (s_history_count < MEM_PROBE_HISTORY_CAPACITY) {
        slot = (s_history_start + s_history_count) % MEM_PROBE_HISTORY_CAPACITY;
        s_history_count++;
    } else {
        slot = s_history_start;
        s_history_start = (s_history_start + 1) % MEM_PROBE_HISTORY_CAPACITY;
    }
    s_history[slot] = sample;
    s_last_sample_us = now_us;
}

int mem_probe_history_copy(MemProbeSample *out, int capacity)
{
    int count;
    int i;

    if (!out || capacity <= 0) {
        return 0;
    }
    count = s_history_count < capacity ? s_history_count : capacity;
    for (i = 0; i < count; i++) {
        int source = (s_history_start + s_history_count - count + i) %
                     MEM_PROBE_HISTORY_CAPACITY;
        out[i] = s_history[source];
    }
    return count;
}

int mem_probe_latest(MemProbeSample *out)
{
    int slot;

    if (!out || s_history_count <= 0) {
        return 0;
    }
    slot = (s_history_start + s_history_count - 1) % MEM_PROBE_HISTORY_CAPACITY;
    *out = s_history[slot];
    return 1;
}
