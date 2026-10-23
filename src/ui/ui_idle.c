#include "ui/ui_idle.h"

#include <stdio.h>
#include <string.h>
#include <pspkernel.h>

#include "core/fs.h"
#include "core/logger.h"

#define IDLE_CFG_PATH "config/idle.cfg"

static const int kDelays[] = { 5, 10, 20, 30, 60, 120 };
#define DELAY_N ((int)(sizeof(kDelays) / sizeof(kDelays[0])))

static int s_mode = IDLE_DIM;
static int s_delay_idx = 1;             /* 10 с */
static unsigned long long s_last_activity_us = 0;
static unsigned long long s_last_step_us = 0;
static float s_k = 0.0f;

static void idle_save(void)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "mode=%d delay=%d\n", s_mode,
                     kDelays[s_delay_idx]);
    if (n > 0 && n < (int)sizeof(buf)) {
        fs_write_deferred(IDLE_CFG_PATH, buf, (size_t)n);
    }
    logLine("idle: mode=%d delay=%d queued\n", s_mode, kDelays[s_delay_idx]);
}

void idle_init(void)
{
    char buf[64];
    SceUID fd;
    int count, mode = -1, delay = -1, i;

    s_mode = IDLE_DIM;
    s_delay_idx = 1;
    fd = fs_open(IDLE_CFG_PATH, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        count = fs_read(fd, buf, sizeof(buf) - 1);
        fs_close(fd);
        if (count > 0) {
            buf[count] = '\0';
            if (sscanf(buf, "mode=%d delay=%d", &mode, &delay) == 2) {
                if (mode >= 0 && mode < IDLE_MODE_COUNT) s_mode = mode;
                for (i = 0; i < DELAY_N; i++) {
                    if (kDelays[i] == delay) { s_delay_idx = i; break; }
                }
            }
        }
    }
    logLine("idle: mode=%d delay=%d\n", s_mode, kDelays[s_delay_idx]);
}

int idle_get_mode(void) { return s_mode; }

void idle_cycle_mode(int dir)
{
    s_mode = (s_mode + (dir >= 0 ? 1 : IDLE_MODE_COUNT - 1)) % IDLE_MODE_COUNT;
    idle_save();
}

LocaleKey idle_mode_name_key(int mode)
{
    switch (mode) {
    case IDLE_DIM:   return LOCALE_IDLE_FADE;
    case IDLE_HIDE:  return LOCALE_IDLE_HIDE;
    case IDLE_COVER: return LOCALE_IDLE_COVER;
    default:         return LOCALE_SETTING_OFF;
    }
}

int idle_get_delay_s(void) { return kDelays[s_delay_idx]; }

void idle_cycle_delay(int dir)
{
    s_delay_idx = (s_delay_idx + (dir >= 0 ? 1 : DELAY_N - 1)) % DELAY_N;
    idle_save();
}

void idle_note_activity(void)
{
    s_last_activity_us = sceKernelGetSystemTimeWide();
}

float idle_step(void)
{
    unsigned long long now = sceKernelGetSystemTimeWide();
    float dt, target, rate;

    /* Экран не рисовался (были на другом экране) — считаем возвращение действием. */
    if (s_last_step_us == 0 || now - s_last_step_us > 300000ULL) {
        s_last_activity_us = now;
        s_k = 0.0f;
    }
    dt = (float)(now - s_last_step_us) * 1e-6f;
    if (s_last_step_us == 0 || dt > 0.1f) dt = 0.0166f;
    s_last_step_us = now;

    if (s_mode == IDLE_OFF) {
        s_k = 0.0f;
        return 0.0f;
    }
    target = (now - s_last_activity_us) >=
             (unsigned long long)kDelays[s_delay_idx] * 1000000ULL ? 1.0f : 0.0f;
    /* Уход в режим — плавно (~1.6 с), возврат — быстро (~0.25 с). */
    rate = target > s_k ? dt / 1.6f : dt / 0.25f;
    if (target > s_k) {
        s_k += rate;
        if (s_k > target) s_k = target;
    } else {
        s_k -= rate;
        if (s_k < target) s_k = target;
    }
    return s_k;
}
