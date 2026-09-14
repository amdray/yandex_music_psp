// 7-полосный эквалайзер + предусиление, биквады по RBJ cookbook. См. eq.h.
#include "services/eq.h"

#include <pspkernel.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "core/fs.h"
#include "core/logger.h"
#include "services/ym_api.h"

#define EQ_CFG_PATH "config/eq.cfg"
#define EQ_TOAST_US 2500000ULL
#define EQ_PEAK_Q 1.0f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float b0, b1, b2, a1, a2;  // a0 нормирован к 1
} EqBiquad;

typedef struct {
    float x1, x2, y1, y2;
} EqState;

// Порядок полос: LS, PK x5, HS.
static const float s_band_freq[EQ_BANDS] =
    { 60.0f, 150.0f, 400.0f, 1000.0f, 2400.0f, 6000.0f, 15000.0f };
static const int s_band_type[EQ_BANDS] = { 0, 1, 1, 1, 1, 1, 2 };  // 0=LS,1=PK,2=HS

static const float s_preset_gains[EQ_PRESET_COUNT][EQ_BANDS] = {
    [EQ_PRESET_OFF]       = {  0,  0,  0,  0,  0,  0,  0 },
    [EQ_PRESET_ROCK]      = { +5, +3, +1, -2,  0, +3, +4 },
    [EQ_PRESET_POP]       = { +3, +3, +2, +3, +2, +2, +1 },
    [EQ_PRESET_JAZZ]      = { +4, +3, +2, +1, +2, +3, +3 },
    [EQ_PRESET_CLASSICAL] = { +4, +3, +1,  0, +1, +3, +4 },
    [EQ_PRESET_BASS]      = { +7, +4, +1,  0, -1, -2, -3 },
    [EQ_PRESET_TREBLE]    = { -3, -2, -1,  0, +2, +4, +5 },
    [EQ_PRESET_VOCAL]     = { -2, -1,  0, +2, +5, +4, +2 },
    [EQ_PRESET_CUSTOM]    = {  0,  0,  0,  0,  0,  0,  0 },
};

static float s_active[EQ_BANDS];
static float s_custom[EQ_BANDS];
static int s_preset = EQ_PRESET_OFF;
static float s_preamp_db = 0.0f;
static int s_rate = 0;
static int s_active_flag = 0;
static int s_dirty = 1;
static EqBiquad s_coef[EQ_BANDS];
static EqState s_state[EQ_BANDS][2];  // [полоса][канал L/R]
static unsigned long long s_toast_until = 0;
static int s_toast_kind = 0;  // 1 пресет, 2 преамп

float eq_band_freq(int band)
{
    if (band < 0 || band >= EQ_BANDS) {
        return 0.0f;
    }
    return s_band_freq[band];
}

static float clamp_db(float db)
{
    if (db < EQ_GAIN_MIN_DB) return EQ_GAIN_MIN_DB;
    if (db > EQ_GAIN_MAX_DB) return EQ_GAIN_MAX_DB;
    return db;
}

static void recompute(int rate)
{
    int b;
    for (b = 0; b < EQ_BANDS; b++) {
        float A = powf(10.0f, s_active[b] / 40.0f);
        float w0 = 2.0f * (float)M_PI * s_band_freq[b] / (float)rate;
        float cw = cosf(w0);
        float sw = sinf(w0);
        float b0, b1, b2, a0, a1, a2;
        if (s_band_type[b] == 1) {
            float alpha = sw / (2.0f * EQ_PEAK_Q);
            b0 = 1.0f + alpha * A;
            b1 = -2.0f * cw;
            b2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            a1 = -2.0f * cw;
            a2 = 1.0f - alpha / A;
        } else {
            // Шелфы с S=1: alpha = sin(w0)/2*sqrt(2).
            float alpha = sw * 0.7071067811865476f;
            float sqA = sqrtf(A);
            float Ap1 = A + 1.0f;
            float Am1 = A - 1.0f;
            if (s_band_type[b] == 0) {
                b0 = A * (Ap1 - Am1 * cw + 2.0f * sqA * alpha);
                b1 = 2.0f * A * (Am1 - Ap1 * cw);
                b2 = A * (Ap1 - Am1 * cw - 2.0f * sqA * alpha);
                a0 = Ap1 + Am1 * cw + 2.0f * sqA * alpha;
                a1 = -2.0f * (Am1 + Ap1 * cw);
                a2 = Ap1 + Am1 * cw - 2.0f * sqA * alpha;
            } else {
                b0 = A * (Ap1 + Am1 * cw + 2.0f * sqA * alpha);
                b1 = -2.0f * A * (Am1 + Ap1 * cw);
                b2 = A * (Ap1 + Am1 * cw - 2.0f * sqA * alpha);
                a0 = Ap1 - Am1 * cw + 2.0f * sqA * alpha;
                a1 = 2.0f * (Am1 - Ap1 * cw);
                a2 = Ap1 - Am1 * cw - 2.0f * sqA * alpha;
            }
        }
        s_coef[b].b0 = b0 / a0;
        s_coef[b].b1 = b1 / a0;
        s_coef[b].b2 = b2 / a0;
        s_coef[b].a1 = a1 / a0;
        s_coef[b].a2 = a2 / a0;
    }
    s_rate = rate;
    s_dirty = 0;
}

void eq_reset(void)
{
    memset(s_state, 0, sizeof(s_state));
}

static void refresh_active_flag(void)
{
    int b;
    s_active_flag = (s_preamp_db > 0.01f) ? 1 : 0;
    if (!s_active_flag) {
        for (b = 0; b < EQ_BANDS; b++) {
            if (s_active[b] < -0.01f || s_active[b] > 0.01f) {
                s_active_flag = 1;
                break;
            }
        }
    }
    if (s_active_flag) {
        eq_reset();
    }
}

static void apply_preset_gains(int preset)
{
    int b;
    for (b = 0; b < EQ_BANDS; b++) {
        s_active[b] = (preset == EQ_PRESET_CUSTOM)
            ? clamp_db(s_custom[b])
            : s_preset_gains[preset][b];
    }
    s_dirty = 1;
    refresh_active_flag();
}

static void toast(int kind)
{
    s_toast_kind = kind;
    s_toast_until = sceKernelGetSystemTimeWide() + EQ_TOAST_US;
}

void eq_notify_quality(void)
{
    toast(3);
}

int eq_set_preset(int preset)
{
    if (preset < 0) preset = 0;
    if (preset >= EQ_PRESET_COUNT) preset = EQ_PRESET_COUNT - 1;
    s_preset = preset;
    apply_preset_gains(preset);
    toast(1);
    logLine("eq: preset %d\n", s_preset);
    logger_flush();
    return s_preset;
}

int eq_next_preset(void)
{
    return eq_set_preset((s_preset + 1) % EQ_PRESET_COUNT);
}

int eq_prev_preset(void)
{
    return eq_set_preset((s_preset + EQ_PRESET_COUNT - 1) % EQ_PRESET_COUNT);
}

int eq_get_preset(void)
{
    return s_preset;
}

void eq_set_custom_gain(int band, float db)
{
    if (band < 0 || band >= EQ_BANDS) {
        return;
    }
    s_custom[band] = clamp_db(db);
    if (s_preset == EQ_PRESET_CUSTOM) {
        s_active[band] = s_custom[band];
        s_dirty = 1;
        refresh_active_flag();
    }
}

float eq_get_custom_gain(int band)
{
    if (band < 0 || band >= EQ_BANDS) {
        return 0.0f;
    }
    return s_custom[band];
}

void eq_get_gains(float out_gains[EQ_BANDS])
{
    int b;
    if (!out_gains) {
        return;
    }
    for (b = 0; b < EQ_BANDS; b++) {
        out_gains[b] = s_active[b];
    }
}

void eq_set_preamp_db(float db)
{
    if (db < 0.0f) db = 0.0f;
    if (db > EQ_PREAMP_MAX_DB) db = EQ_PREAMP_MAX_DB;
    if (db != s_preamp_db) {
        s_preamp_db = db;
        refresh_active_flag();
        toast(2);
        logLine("eq: preamp %d dB\n", (int)db);
        logger_flush();
    }
}

float eq_get_preamp_db(void)
{
    return s_preamp_db;
}

int eq_is_active(void)
{
    return s_active_flag;
}

int eq_toast_kind(void)
{
    if (s_toast_until == 0 ||
        (unsigned long long)sceKernelGetSystemTimeWide() >= s_toast_until) {
        return 0;
    }
    return s_toast_kind;
}

int eq_toast_preset(void)
{
    return (eq_toast_kind() == 1) ? s_preset : -1;
}

void eq_process(short *pcm, int frames, int channels, int rate)
{
    int i, ch, b;
    float pre;

    if (!pcm || frames <= 0 || !s_active_flag) {
        return;
    }
    if (channels < 1) channels = 1;
    if (channels > 2) channels = 2;
    if (rate <= 0) {
        return;
    }
    if (s_dirty || rate != s_rate) {
        recompute(rate);
    }
    pre = powf(10.0f, s_preamp_db / 20.0f);
    for (i = 0; i < frames; i++) {
        for (ch = 0; ch < channels; ch++) {
            float y = (float)pcm[i * channels + ch] * pre;
            for (b = 0; b < EQ_BANDS; b++) {
                EqBiquad *c = &s_coef[b];
                EqState *s = &s_state[b][ch];
                float out = c->b0 * y + c->b1 * s->x1 + c->b2 * s->x2
                          - c->a1 * s->y1 - c->a2 * s->y2;
                s->x2 = s->x1;
                s->x1 = y;
                s->y2 = s->y1;
                s->y1 = out;
                y = out;
            }
            if (y > 32767.0f) y = 32767.0f;
            else if (y < -32768.0f) y = -32768.0f;
            pcm[i * channels + ch] = (short)y;
        }
    }
}

// --- persistence: config/eq.cfg ---
// Построчно, каждое поле независимо (старые файлы без новых строк читаются).
void eq_save(void)
{
    char buf[160];
    SceUID fd;
    int w, len;

    len = snprintf(buf, sizeof(buf),
                   "preset=%d\ncustom=%d,%d,%d,%d,%d,%d,%d\npreamp=%d\nquality=%s\n",
                   s_preset,
                   (int)s_custom[0], (int)s_custom[1], (int)s_custom[2],
                   (int)s_custom[3], (int)s_custom[4], (int)s_custom[5],
                   (int)s_custom[6], (int)s_preamp_db,
                   ym_api_download_quality());
    if (len <= 0 || len >= (int)sizeof(buf)) {
        return;
    }
    fd = fs_open(EQ_CFG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }
    w = fs_write(fd, buf, (size_t)len);
    fs_close(fd);
    if (w != len) {
        logLine("eq: save short %d/%d\n", w, len);
        logger_flush();
    }
}

void eq_init(void)
{
    char buf[192];
    SceUID fd;
    int n, p, pre, b;
    int g[EQ_BANDS];
    const char *pl;
    char q[8];

    for (b = 0; b < EQ_BANDS; b++) {
        s_custom[b] = 0.0f;
    }
    s_preset = EQ_PRESET_OFF;
    s_preamp_db = 0.0f;
    fd = fs_open(EQ_CFG_PATH, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        n = fs_read(fd, buf, sizeof(buf) - 1);
        fs_close(fd);
        if (n > 0) {
            buf[n] = '\0';
            pl = strstr(buf, "preset=");
            if (pl && sscanf(pl, "preset=%d", &p) == 1 &&
                p >= 0 && p < EQ_PRESET_COUNT) {
                s_preset = p;
            }
            pl = strstr(buf, "custom=");
            if (pl && sscanf(pl, "custom=%d,%d,%d,%d,%d,%d,%d",
                             &g[0], &g[1], &g[2], &g[3], &g[4], &g[5],
                             &g[6]) == 7) {
                for (b = 0; b < EQ_BANDS; b++) {
                    s_custom[b] = clamp_db((float)g[b]);
                }
            }
            pl = strstr(buf, "preamp=");
            if (pl && sscanf(pl, "preamp=%d", &pre) == 1 &&
                pre >= 0 && pre <= (int)EQ_PREAMP_MAX_DB) {
                s_preamp_db = (float)pre;
            }
            pl = strstr(buf, "quality=");
            if (pl && sscanf(pl, "quality=%7s", q) == 1) {
                ym_api_download_set_quality(q);
            }
            logLine("eq: loaded preset=%d\n", s_preset);
        }
    }
    // Применяем без тоста (молча).
    apply_preset_gains(s_preset);
    s_toast_until = 0;
    s_toast_kind = 0;
    logLine("eq: init preset=%d active=%d\n", s_preset, s_active_flag);
    logger_flush();
}
