/* Статически проверен и зафиксирован: сбор показателей памяти
 * и история отсчётов. */
#include "core/mem_probe.h"

#include <malloc.h>
#include <pspsysmem.h>

#define MEM_PROBE_INTERVAL_US 5000000ULL

static MemProbeSample s_history[MEM_PROBE_HISTORY_CAPACITY];
static int s_history_start = 0;
static int s_history_count = 0;
static u64 s_last_sample_us = 0;

/* Defined by PSP_HEAP_SIZE_KB(...) in main.c. */
extern int sce_newlib_heap_kb_size;

static u32 configured_heap_size(void)
{
    int kb = sce_newlib_heap_kb_size;
    return kb > 0 ? (u32)kb * 1024u : 0;
}

void mem_probe_update(u64 now_us)
{
    MemProbeSample sample;
    struct mallinfo mi;
    int slot;

    if (s_history_count > 0 && now_us - s_last_sample_us < MEM_PROBE_INTERVAL_US) {
        return;
    }

    sample.time_us = now_us;
    sample.heap_configured = configured_heap_size();
    mi = mallinfo();
    sample.heap_used = (u32)mi.uordblks;
    sample.partition_free = sceKernelTotalFreeMemSize();
    sample.partition_largest_free = sceKernelMaxFreeMemSize();

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
