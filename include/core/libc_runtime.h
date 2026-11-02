#ifndef YM_CORE_LIBC_RUNTIME_H
#define YM_CORE_LIBC_RUNTIME_H

/* Initialize once in main, before starting workers. Negative values are errors. */
int ym_libc_guard_init(void);
int ym_libc_lock(void);
int ym_libc_unlock(void);

#endif
