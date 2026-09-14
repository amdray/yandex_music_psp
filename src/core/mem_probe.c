#include "core/mem_probe.h"

#include <malloc.h>
#include <pspsysmem.h>

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
