#include "services/net_speed.h"

#include "core/logger.h"

/* Пороги (КБ/с реальной скорости) для перехода НА уровень. Битрейт потока
 * hq=256 кбит/с ≈ 32 КБ/с, nq=192 ≈ 24, lq=64 ≈ 8; запас ~2.5–3x нужен на
 * докачку следующего трека, TLS-рукопожатия и просадки Wi-Fi 802.11b. */
#define SPEED_HQ_KBPS 90
#define SPEED_NQ_KBPS 45

static volatile float s_ema_kbps = 0.0f;
static volatile int s_samples = 0;
static volatile int s_level = 1;

void net_speed_note(int64_t bytes, uint64_t us)
{
    float kbps;
    if (bytes < 0 || us < 1500000ULL) return;
    kbps = (float)bytes * 1000000.0f / ((float)us * 1024.0f);
    if (s_samples == 0) {
        s_ema_kbps = kbps;
    } else {
        s_ema_kbps = s_ema_kbps + (kbps - s_ema_kbps) * 0.35f;
    }
    if (s_samples < 1000000) s_samples++;
}

int net_speed_kbps(void)
{
    return s_samples > 0 ? (int)(s_ema_kbps + 0.5f) : 0;
}

int net_speed_auto_level(void)
{
    float v;
    int lvl, prev;
    if (s_samples < 2) return s_level;
    v = s_ema_kbps;
    prev = lvl = s_level;
    /* Подъём — выше порога на 15%, спуск — ниже порога на 15%. */
    if (lvl < 2 && v >= SPEED_HQ_KBPS * 1.15f) lvl = 2;
    else if (lvl < 1 && v >= SPEED_NQ_KBPS * 1.15f) lvl = 1;
    if (lvl >= 2 && v < SPEED_HQ_KBPS * 0.85f) lvl = (v >= SPEED_NQ_KBPS) ? 1 : 0;
    if (lvl == 1 && v < SPEED_NQ_KBPS * 0.85f) lvl = 0;
    if (lvl != prev) {
        s_level = lvl;
        logLine("net_speed: auto level %d -> %d (ema=%d KB/s)\n", prev, lvl,
                (int)(v + 0.5f));
    }
    return s_level;
}
