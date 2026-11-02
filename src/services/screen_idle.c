#include "services/screen_idle.h"
#include "services/system_status.h"

#include "core/fs.h"
#include "core/logger.h"

#include <pspimpose.h>
#include <psppower.h>
#include <stdio.h>

#define SCREEN_CFG_PATH "config/screen.cfg"

static const int kDelays[] = { 0, 300, 180, 60, 30 };
#define DELAY_COUNT ((int)(sizeof(kDelays) / sizeof(kDelays[0])))
#define DEFAULT_DELAY_INDEX 3

static int s_delay_index = DEFAULT_DELAY_INDEX;
static int s_off_on_power;
static int s_tick_error;

static int apply_delay(int index)
{
    int delay = kDelays[index];
    int rc = delay == 0 ? scePowerTick(PSP_POWER_TICK_DISPLAY)
                        : sceImposeSetBacklightOffTime(delay);
    if (rc < 0) {
        logLine("screen_idle: apply delay_s=%d failed rc=0x%08X\n",
                delay, (unsigned int)rc);
        return rc;
    }
    s_delay_index = index;
    s_tick_error = 0;
    logLine("screen_idle: delay_s=%d applied\n", delay);
    return 0;
}

int screen_idle_init(void)
{
    char record[sizeof("delay=300\r\non_power=1\r\n")];
    int index = DEFAULT_DELAY_INDEX;
    s_off_on_power = 0;
    SceUID fd = fs_open(SCREEN_CFG_PATH, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        int count = fs_read(fd, record, sizeof(record) - 1);
        fs_close(fd);
        if (count > 0) {
            int delay, i, on_power = 0;
            record[count] = '\0';
            int fields = sscanf(record, "delay=%d on_power=%d", &delay, &on_power);
            if (fields >= 1) {
                for (i = 0; i < DELAY_COUNT; ++i) {
                    if (kDelays[i] == delay) {
                        index = i;
                        break;
                    }
                }
            }
            if (fields == 2 && (on_power == 0 || on_power == 1)) {
                s_off_on_power = on_power;
            }
        }
    }
    return apply_delay(index);
}

int screen_idle_delay_s(void)
{
    return kDelays[s_delay_index];
}

static void screen_idle_save(void)
{
    char record[sizeof("delay=300\r\non_power=1\r\n")];
    int length;
    length = snprintf(record, sizeof(record), "delay=%d\non_power=%d\n",
                      screen_idle_delay_s(), s_off_on_power);
    if (length > 0 && length < (int)sizeof(record)) {
        int rc = fs_write_deferred(SCREEN_CFG_PATH, record, (size_t)length);
        if (rc < 0) {
            logLine("screen_idle: save failed rc=0x%08X\n", (unsigned int)rc);
        }
    }
}

void screen_idle_cycle_delay(int direction)
{
    int index = (s_delay_index + (direction >= 0 ? 1 : DELAY_COUNT - 1))
                % DELAY_COUNT;
    if (apply_delay(index) < 0) return;
    screen_idle_save();
}

int screen_idle_off_on_power(void)
{
    return s_off_on_power;
}

void screen_idle_set_off_on_power(int enabled)
{
    enabled = enabled != 0;
    if (s_off_on_power == enabled) return;
    s_off_on_power = enabled;
    screen_idle_save();
}

void screen_idle_service(void)
{
    int rc;
    if (screen_idle_delay_s() != 0) {
        if (s_off_on_power) return;
        SystemStatusSnapshot status;
        system_status_get_snapshot(&status);
        if (status.battery_charging == 0 && status.power_online == 0) return;
    }
    rc = scePowerTick(PSP_POWER_TICK_DISPLAY);
    if (rc < 0 && rc != s_tick_error) {
        logLine("screen_idle: display tick failed rc=0x%08X\n", (unsigned int)rc);
    }
    s_tick_error = rc;
}
