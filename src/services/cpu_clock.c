#include "services/cpu_clock.h"

#include "core/fs.h"
#include "core/logger.h"

#include <psppower.h>
#include <stdio.h>

#define CPU_CLOCK_CFG_PATH "config/cpu.cfg"
static const int kClocks[] = { 111, 222, 333 };
#define CLOCK_COUNT ((int)(sizeof(kClocks) / sizeof(kClocks[0])))
#define DEFAULT_CLOCK_INDEX 1

static int s_clock_index = DEFAULT_CLOCK_INDEX;

static int apply_clock(int index)
{
    int mhz = kClocks[index];
    int previous_cpu = scePowerGetCpuClockFrequencyInt();
    int previous_bus = scePowerGetBusClockFrequencyInt();
    int rc = scePowerSetClockFrequency(mhz, mhz, mhz / 2);
    int current_cpu = scePowerGetCpuClockFrequencyInt();
    int current_bus = scePowerGetBusClockFrequencyInt();
    logLine("cpu_clock: cpu=%d->%d bus=%d->%d MHz requested=%d rc=0x%08X\n",
            previous_cpu, current_cpu, previous_bus, current_bus, mhz, (unsigned)rc);
    if (rc < 0) {
        return rc;
    }
    s_clock_index = index;
    return 0;
}

int cpu_clock_init(void)
{
    char record[sizeof("mhz=333\r\n")];
    int index = DEFAULT_CLOCK_INDEX;
    SceUID fd = fs_open(CPU_CLOCK_CFG_PATH, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        int count = fs_read(fd, record, sizeof(record) - 1);
        fs_close(fd);
        if (count > 0) {
            int mhz, i;
            record[count] = '\0';
            if (sscanf(record, "mhz=%d", &mhz) == 1) {
                for (i = 0; i < CLOCK_COUNT; ++i) {
                    if (kClocks[i] == mhz) {
                        index = i;
                        break;
                    }
                }
            }
        }
    }
    return apply_clock(index);
}

int cpu_clock_mhz(void)
{
    return kClocks[s_clock_index];
}

void cpu_clock_cycle(int direction)
{
    char record[sizeof("mhz=333\r\n")];
    int length;
    int index = (s_clock_index + (direction >= 0 ? 1 : CLOCK_COUNT - 1)) % CLOCK_COUNT;
    if (apply_clock(index) < 0) return;
    length = snprintf(record, sizeof(record), "mhz=%d\n", cpu_clock_mhz());
    if (length > 0 && length < (int)sizeof(record)) {
        int rc = fs_write_deferred(CPU_CLOCK_CFG_PATH, record, (size_t)length);
        if (rc < 0) logLine("cpu_clock: save failed rc=0x%08X\n", (unsigned)rc);
    }
}
