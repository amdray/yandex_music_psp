/* Проверен и зафиксирован: инициализация, захват и освобождение
 * общей блокировки операций libc. */
#include "core/libc_runtime.h"

#include <pspkernel.h>
#include <stddef.h>

static SceUID s_libc_sema = -1;

int ym_libc_guard_init(void)
{
    if (s_libc_sema < 0) {
        s_libc_sema = sceKernelCreateSema("ym_libc", 0, 1, 1, NULL);
    }
    return s_libc_sema < 0 ? s_libc_sema : 0;
}

int ym_libc_lock(void)
{
    if (s_libc_sema < 0) return s_libc_sema;
    return sceKernelWaitSema(s_libc_sema, 1, NULL);
}

int ym_libc_unlock(void)
{
    if (s_libc_sema < 0) return s_libc_sema;
    return sceKernelSignalSema(s_libc_sema, 1);
}


