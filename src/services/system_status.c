#include "services/system_status.h"

#include "core/fs.h"
#include "core/logger.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psptypes.h>
#include <stdio.h>
#include <string.h>

#define IMPOSE_GET_PARAM_NID 0x531C9778u
#define IMPOSE_MAIN_VOLUME   0x1u
#define VOLUME_POLL_US       100000ULL
#define BATTERY_POLL_US      5000000ULL
#define BATTERY_LOG_US       30000000ULL
#define CPU_SAMPLE_US        1000000ULL
#define CPU_LOG_US           5000000ULL
#define CPU_LOG_FLUSH_US     30000000ULL
#define CPU_LOG_PATH         "data/logs/cpu.txt"
#define CPU_LOG_BUFFER_SIZE  (4 * 1024)

struct KernelCallArg {
    u32 arg1;
    u32 arg2;
    u32 arg3;
    u32 arg4;
    u32 arg5;
    u32 arg6;
    u32 arg7;
    u32 arg8;
    u32 arg9;
    u32 arg10;
    u32 arg11;
    u32 arg12;
    u32 ret1;
    u32 ret2;
};

extern u32 sctrlHENFindFunction(const char *module_name,
                                const char *library_name,
                                u32 nid);
extern int kuKernelCall(void *function, struct KernelCallArg *args);

static SystemStatusSnapshot s_status;
static u32 s_impose_get_param;
static u64 s_last_volume_us;
static u64 s_last_battery_us;
static u64 s_last_battery_log_us;
static u64 s_last_cpu_sample_us;
static u64 s_cpu_previous_idle_us;
static u64 s_cpu_window_wall_us;
static u64 s_cpu_window_idle_us;
static u32 s_cpu_window_start_idle_count;
static u32 s_cpu_window_start_thread_switches;
static u32 s_cpu_window_start_vfpu_switches;
static unsigned int s_cpu_sample_count;
static unsigned int s_cpu_busy_min_bp;
static unsigned int s_cpu_busy_max_bp;
static SceUID s_cpu_log_fd = -1;
static char s_cpu_log_buffer[CPU_LOG_BUFFER_SIZE];
static size_t s_cpu_log_used;
static unsigned int s_cpu_log_dropped;
static u64 s_last_cpu_log_flush_us;

static void cpu_log_disable(const char *operation, int rc)
{
    logLine("cpu_telemetry: %s failed rc=%d\n", operation, rc);
    if (s_cpu_log_fd >= 0) {
        fs_close(s_cpu_log_fd);
        s_cpu_log_fd = -1;
    }
    s_cpu_log_used = 0;
}

static void cpu_log_flush(void)
{
    int written;

    if (s_cpu_log_fd < 0 || s_cpu_log_used == 0) {
        return;
    }

    written = fs_write(s_cpu_log_fd, s_cpu_log_buffer, s_cpu_log_used);
    if (written != (int)s_cpu_log_used) {
        cpu_log_disable("write", written);
        return;
    }
    s_cpu_log_used = 0;

    if (s_cpu_log_dropped != 0) {
        char line[64];
        int len = snprintf(line, sizeof(line), "# dropped_rows=%u\n",
                           s_cpu_log_dropped);
        s_cpu_log_dropped = 0;
        if (len > 0) {
            written = fs_write(s_cpu_log_fd, line, (size_t)len);
            if (written != len) {
                cpu_log_disable("write dropped marker", written);
            }
        }
    }
}

static void cpu_log_append(const char *line, size_t len)
{
    if (s_cpu_log_fd < 0 || len == 0) {
        return;
    }
    if (len > CPU_LOG_BUFFER_SIZE - s_cpu_log_used) {
        s_cpu_log_dropped++;
        return;
    }
    memcpy(s_cpu_log_buffer + s_cpu_log_used, line, len);
    s_cpu_log_used += len;
}

static void cpu_log_init(void)
{
    static const char header[] =
        "ms\twindow_us\tidle_us\tactive_us\tstatus\tidle_wakeups\t"
        "thread_switches\tvfpu_switches\tcpu_mhz\tbus_mhz\tsamples\t"
        "busy_avg_bp\tbusy_min_bp\tbusy_max_bp\n";
    int written;

    s_cpu_log_used = 0;
    s_cpu_log_dropped = 0;
    s_last_cpu_log_flush_us = 0;
    s_cpu_log_fd = fs_open(CPU_LOG_PATH,
                           PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (s_cpu_log_fd < 0) {
        logLine("cpu_telemetry: open failed rc=%d path='%s'\n",
                s_cpu_log_fd, CPU_LOG_PATH);
        return;
    }
    written = fs_write(s_cpu_log_fd, header, sizeof(header) - 1);
    if (written != (int)(sizeof(header) - 1)) {
        cpu_log_disable("write header", written);
    }
}

static u32 find_impose_get_param(void)
{
    static const char *const module_names[] = {
        "sceImpose_Driver",
        "sceImpose_Service",
        "sceImpose"
    };
    unsigned int i;

    for (i = 0; i < sizeof(module_names) / sizeof(module_names[0]); ++i) {
        u32 address = sctrlHENFindFunction(module_names[i],
                                           "sceImpose_driver",
                                           IMPOSE_GET_PARAM_NID);
        if (address != 0) {
            return address;
        }
    }
    return 0;
}

static void update_volume(void)
{
    struct KernelCallArg args;
    int rc;
    int level;

    if (s_impose_get_param == 0) {
        s_impose_get_param = find_impose_get_param();
    }
    if (s_impose_get_param == 0) {
        s_status.volume_available = 0;
        return;
    }

    memset(&args, 0, sizeof(args));
    args.arg1 = IMPOSE_MAIN_VOLUME;
    rc = kuKernelCall((void *)s_impose_get_param, &args);
    level = (int)args.ret1;
    if (rc < 0 || level < 0 || level > 30) {
        s_status.volume_available = 0;
        return;
    }

    s_status.volume_level = level;
    s_status.volume_available = 1;
}

static void update_battery(void)
{
    int exists = scePowerIsBatteryExist();
    int remaining;
    int full;
    int percent;

    if (exists <= 0) {
        s_status.battery_available = 0;
        return;
    }

    remaining = scePowerGetBatteryRemainCapacity();
    full = scePowerGetBatteryFullCapacity();
    if (remaining >= 0 && full > 0) {
        percent = (int)(((long long)remaining * 100LL + full / 2) / full);
    } else {
        /* PPSSPP exposes battery presence but does not implement the capacity
         * calls. Its life-percent call is available, as it is on a real PSP. */
        percent = scePowerGetBatteryLifePercent();
        if (percent < 0 || percent > 100) {
            s_status.battery_available = 0;
            return;
        }
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    s_status.battery_percent = percent;
    s_status.battery_charging = scePowerIsBatteryCharging() > 0;
    s_status.battery_available = 1;
}

static void log_battery_telemetry(void)
{
    int online = scePowerIsPowerOnline();
    int exists = scePowerIsBatteryExist();
    int charging = scePowerIsBatteryCharging();
    int charging_status = scePowerGetBatteryChargingStatus();
    int low = scePowerIsLowBattery();
    int suspend_required = scePowerIsSuspendRequired();
    int remaining_mah = scePowerGetBatteryRemainCapacity();
    int full_mah = scePowerGetBatteryFullCapacity();
    int life_percent = scePowerGetBatteryLifePercent();
    int life_minutes = scePowerGetBatteryLifeTime();
    int temperature_c = scePowerGetBatteryTemp();
    int voltage_mv = scePowerGetBatteryVolt();
    int capacity_percent = -1;

    if (remaining_mah >= 0 && full_mah > 0) {
        capacity_percent =
            (int)(((long long)remaining_mah * 100LL + full_mah / 2) / full_mah);
        if (capacity_percent < 0) capacity_percent = 0;
        if (capacity_percent > 100) capacity_percent = 100;
    }

    /* scePowerGetBatteryElec() is deliberately excluded: PSPSDK documents its
     * meaning as unknown and warns that it crashes a PSP from user mode. */
    logLine("battery: raw online=%d exists=%d charging=%d charge_status=%d "
            "low=%d suspend_required=%d remain_mah=%d full_mah=%d "
            "life_percent=%d life_minutes=%d temp_c=%d voltage_mv=%d "
            "derived_capacity_percent=%d\n",
            online, exists, charging, charging_status, low, suspend_required,
            remaining_mah, full_mah, life_percent, life_minutes,
            temperature_c, voltage_mv, capacity_percent);
}

static u64 system_clock_to_u64(const SceKernelSysClock *clock)
{
    return ((u64)clock->hi << 32) | (u64)clock->low;
}

static void reset_cpu_window(const SceKernelSystemStatus *status)
{
    s_cpu_window_wall_us = 0;
    s_cpu_window_idle_us = 0;
    s_cpu_window_start_idle_count = status->comesOutOfIdleCount;
    s_cpu_window_start_thread_switches = status->threadSwitchCount;
    s_cpu_window_start_vfpu_switches = status->vfpuSwitchCount;
    s_cpu_sample_count = 0;
    s_cpu_busy_min_bp = 10000;
    s_cpu_busy_max_bp = 0;
}

static void update_cpu_telemetry(u64 now)
{
    SceKernelSystemStatus status;
    u64 idle_us;
    u64 wall_delta;
    u64 idle_delta;
    unsigned int busy_bp;
    unsigned int busy_avg_bp;
    char line[256];
    int line_len;
    int rc;

    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    rc = sceKernelReferSystemStatus(&status);
    if (rc < 0) {
        line_len = snprintf(line, sizeof(line), "# ms=%llu system_status_error=%d\n",
                            (unsigned long long)(now / 1000ULL), rc);
        if (line_len > 0 && line_len < (int)sizeof(line)) {
            cpu_log_append(line, (size_t)line_len);
        }
        s_last_cpu_sample_us = now;
        s_cpu_previous_idle_us = 0;
        s_cpu_sample_count = 0;
        return;
    }

    idle_us = system_clock_to_u64(&status.idleClocks);
    if (s_last_cpu_sample_us == 0 || s_cpu_previous_idle_us == 0 ||
        now <= s_last_cpu_sample_us || idle_us < s_cpu_previous_idle_us) {
        s_last_cpu_sample_us = now;
        s_cpu_previous_idle_us = idle_us;
        reset_cpu_window(&status);
        return;
    }

    wall_delta = now - s_last_cpu_sample_us;
    idle_delta = idle_us - s_cpu_previous_idle_us;
    if (idle_delta > wall_delta) {
        idle_delta = wall_delta;
    }

    busy_bp = (unsigned int)(((wall_delta - idle_delta) * 10000ULL) /
                             wall_delta);
    if (busy_bp < s_cpu_busy_min_bp) s_cpu_busy_min_bp = busy_bp;
    if (busy_bp > s_cpu_busy_max_bp) s_cpu_busy_max_bp = busy_bp;
    s_cpu_window_wall_us += wall_delta;
    s_cpu_window_idle_us += idle_delta;
    s_cpu_sample_count++;
    s_last_cpu_sample_us = now;
    s_cpu_previous_idle_us = idle_us;

    if (s_cpu_window_wall_us < CPU_LOG_US) {
        return;
    }

    busy_avg_bp = (unsigned int)(
        ((s_cpu_window_wall_us - s_cpu_window_idle_us) * 10000ULL) /
        s_cpu_window_wall_us);
    line_len = snprintf(line, sizeof(line),
            "%llu\t%llu\t%llu\t%llu\t0x%08X\t%u\t%u\t%u\t%d\t%d\t%u\t%u\t%u\t%u\n",
            (unsigned long long)(now / 1000ULL),
            (unsigned long long)s_cpu_window_wall_us,
            (unsigned long long)s_cpu_window_idle_us,
            (unsigned long long)(s_cpu_window_wall_us - s_cpu_window_idle_us),
            (unsigned int)status.status,
            (unsigned int)(status.comesOutOfIdleCount -
                           s_cpu_window_start_idle_count),
            (unsigned int)(status.threadSwitchCount -
                           s_cpu_window_start_thread_switches),
            (unsigned int)(status.vfpuSwitchCount -
                           s_cpu_window_start_vfpu_switches),
            scePowerGetCpuClockFrequencyInt(),
            scePowerGetBusClockFrequencyInt(),
            s_cpu_sample_count,
            busy_avg_bp, s_cpu_busy_min_bp, s_cpu_busy_max_bp);
    if (line_len > 0 && line_len < (int)sizeof(line)) {
        cpu_log_append(line, (size_t)line_len);
    }
    reset_cpu_window(&status);
}

void system_status_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_impose_get_param = 0;
    s_last_volume_us = 0;
    s_last_battery_us = 0;
    s_last_battery_log_us = 0;
    s_last_cpu_sample_us = 0;
    s_cpu_previous_idle_us = 0;
    s_cpu_window_wall_us = 0;
    s_cpu_window_idle_us = 0;
    s_cpu_sample_count = 0;
    cpu_log_init();
    system_status_update();
}

void system_status_shutdown(void)
{
    cpu_log_flush();
    if (s_cpu_log_fd >= 0) {
        fs_close(s_cpu_log_fd);
        s_cpu_log_fd = -1;
    }
}

void system_status_update(void)
{
    u64 now = sceKernelGetSystemTimeWide();

    if (s_last_volume_us == 0 || now - s_last_volume_us >= VOLUME_POLL_US) {
        update_volume();
        s_last_volume_us = now;
    }
    if (s_last_battery_us == 0 || now - s_last_battery_us >= BATTERY_POLL_US) {
        update_battery();
        s_last_battery_us = now;
    }
    if (s_last_battery_log_us == 0 ||
        now - s_last_battery_log_us >= BATTERY_LOG_US) {
        log_battery_telemetry();
        s_last_battery_log_us = now;
    }
    if (s_last_cpu_sample_us == 0 ||
        now - s_last_cpu_sample_us >= CPU_SAMPLE_US) {
        update_cpu_telemetry(now);
    }
    if (s_last_cpu_log_flush_us == 0) {
        s_last_cpu_log_flush_us = now;
    } else if (now - s_last_cpu_log_flush_us >= CPU_LOG_FLUSH_US) {
        cpu_log_flush();
        s_last_cpu_log_flush_us = now;
    }
}

void system_status_get_snapshot(SystemStatusSnapshot *out)
{
    if (out) {
        *out = s_status;
    }
}
