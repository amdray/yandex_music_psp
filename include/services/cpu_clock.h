#ifndef YM_CPU_CLOCK_H
#define YM_CPU_CLOCK_H

/* Main/UI thread: load and apply, report the selected MHz, cycle and save. */
int cpu_clock_init(void);
int cpu_clock_mhz(void);
void cpu_clock_cycle(int direction);

#endif
