#include "services/audio_analysis.h"
#include "services/audio_player.h"

#include <pspkernel.h>
#include <math.h>

/* Уровни звука для визуализации (свечение «Веб», как анализатор в вебе):
 * три полосы ~0-250 Гц / 0.25-2 кГц / >2 кГц. Считается на аудиопотоке по
 * готовому PCM, один проход с шагом 2 кадра, целочисленные однополюсные
 * фильтры; читает UI-поток (volatile int, MIPS — атомарно). */
static volatile int s_lvl_low = 0, s_lvl_mid = 0, s_lvl_high = 0;  /* 0..65535 */
static int s_lvl_lp1 = 0, s_lvl_lp2 = 0;

static void audio_analysis_feed(const short *pcm, int frames, int channels, int rate);
static int analysis_active(void);

void audio_analysis_feed_pcm(const short *pcm, int frames, int channels, int rate)
{
    int i, step;
    long long a_low = 0, a_mid = 0, a_high = 0;
    int n = 0;
    int low_alpha, mid_alpha;
    if (!pcm || frames <= 0 || rate <= 0) return;
    low_alpha = 18 * 44100 / rate;
    mid_alpha = 110 * 44100 / rate;
    if (low_alpha > 256) low_alpha = 256;
    if (mid_alpha > 256) mid_alpha = 256;
    if (channels < 1) channels = 1;
    if (analysis_active()) audio_analysis_feed(pcm, frames, channels, rate);
    step = channels * 2;
    for (i = 0; i + channels <= frames * channels; i += step) {
        int x = channels > 1 ? ((int)pcm[i] + (int)pcm[i + 1]) >> 1 : (int)pcm[i];
        int d1, m, h;
        s_lvl_lp1 += ((x - s_lvl_lp1) * low_alpha) >> 8;   /* fc≈250 Гц */
        s_lvl_lp2 += ((x - s_lvl_lp2) * mid_alpha) >> 8;  /* fc≈2 кГц */
        d1 = s_lvl_lp1;
        m = s_lvl_lp2 - s_lvl_lp1;
        h = x - s_lvl_lp2;
        a_low += d1 < 0 ? -d1 : d1;
        a_mid += m < 0 ? -m : m;
        a_high += h < 0 ? -h : h;
        n++;
    }
    if (n > 0) {
        int lo = (int)(a_low / n), mi = (int)(a_mid / n), hi = (int)(a_high / n);
        s_lvl_low = lo > 65535 ? 65535 : lo;
        s_lvl_mid = mi > 65535 ? 65535 : mi;
        s_lvl_high = hi > 65535 ? 65535 : hi;
    }
}

void audio_player_get_levels(int *low, int *mid, int *high)
{
    if (low) *low = s_lvl_low;
    if (mid) *mid = s_lvl_mid;
    if (high) *high = s_lvl_high;
}

/* Анализ для Winamp-визуализаций (спектр 16 полос + осциллограф 64 точки).
 * Включается ТОЛЬКО пока UI вызывает audio_player_set_analysis(1) (дедлайн
 * 500 мс продлевается каждым вызовом): в остальное время цена — ноль.
 * Спектр — банк Гёрцеля на моно-потоке, децимация x2 (среднее соседних
 * отсчётов = ФНЧ), окно Хэнна, N<=1024 отсчётов; значение полосы — амплитуда
 * в единицах int16 (0..65535). */
#define SPEC_N_MAX 1024
static const float kSpecHz[AUDIO_SPEC_BANDS] = {
    55, 80, 115, 165, 235, 335, 480, 690, 980, 1400, 2000, 2850, 4000, 5600, 7800, 10200
};
static volatile int s_spec_out[AUDIO_SPEC_BANDS];
static volatile short s_scope_out[AUDIO_SCOPE_POINTS];
static volatile unsigned int s_analysis_until_ms = 0;
static float s_spec_coef[AUDIO_SPEC_BANDS];
static float s_spec_win[SPEC_N_MAX];
static int s_spec_ready = 0;
static int s_spec_rate = 0;

void audio_player_set_analysis(int on)
{
    if (on) s_analysis_until_ms = sceKernelGetSystemTimeLow() / 1000u + 500u;
    else s_analysis_until_ms = 0;
}

static int analysis_active(void)
{
    unsigned int until = s_analysis_until_ms;
    if (until == 0) return 0;
    return (int)(until - sceKernelGetSystemTimeLow() / 1000u) > 0;
}

static void audio_analysis_feed(const short *pcm, int frames, int channels, int rate)
{
    float x[SPEC_N_MAX];
    float s1[AUDIO_SPEC_BANDS], s2[AUDIO_SPEC_BANDS];
    int n, i, b, step;

    if (!s_spec_ready) {
        for (i = 0; i < SPEC_N_MAX; i++) {
            s_spec_win[i] = 0.5f - 0.5f * cosf(6.2831853f * (float)i / (float)(SPEC_N_MAX - 1));
        }
        s_spec_ready = 1;
    }
    if (s_spec_rate != rate) {
        for (b = 0; b < AUDIO_SPEC_BANDS; b++) {
            s_spec_coef[b] = 2.0f * cosf(6.2831853f * kSpecHz[b] / ((float)rate * 0.5f));
        }
        s_spec_rate = rate;
    }
    if (channels < 1) channels = 1;

    /* Осциллограф: 64 равноотстоящих отсчёта блока (моно). */
    for (i = 0; i < AUDIO_SCOPE_POINTS; i++) {
        int f = (int)((long long)i * frames / AUDIO_SCOPE_POINTS);
        int v = channels > 1 ? ((int)pcm[f * channels] + (int)pcm[f * channels + 1]) >> 1
                             : (int)pcm[f];
        s_scope_out[i] = (short)v;
    }

    /* Децимация x2 с усреднением; окно по фактической длине блока. */
    n = frames / 2;
    if (n > SPEC_N_MAX) n = SPEC_N_MAX;
    if (n < 128) return;
    step = channels;
    for (i = 0; i < n; i++) {
        int a0 = channels > 1 ? ((int)pcm[(2 * i) * step] + (int)pcm[(2 * i) * step + 1])
                              : (int)pcm[2 * i] * 2;
        int a1 = channels > 1 ? ((int)pcm[(2 * i + 1) * step] + (int)pcm[(2 * i + 1) * step + 1])
                              : (int)pcm[2 * i + 1] * 2;
        x[i] = (float)(a0 + a1) * 0.25f * s_spec_win[(int)((long long)i * (SPEC_N_MAX - 1) / (n - 1))];
    }
    for (b = 0; b < AUDIO_SPEC_BANDS; b++) { s1[b] = 0.0f; s2[b] = 0.0f; }
    for (i = 0; i < n; i++) {
        float xi = x[i];
        for (b = 0; b < AUDIO_SPEC_BANDS; b++) {
            float s0 = xi + s_spec_coef[b] * s1[b] - s2[b];
            s2[b] = s1[b];
            s1[b] = s0;
        }
    }
    for (b = 0; b < AUDIO_SPEC_BANDS; b++) {
        float p = s1[b] * s1[b] + s2[b] * s2[b] - s_spec_coef[b] * s1[b] * s2[b];
        float amp;
        if (p < 0.0f) p = 0.0f;
        /* окно Хэнна: амплитуда синуса A даёт |X| ≈ A*N/4 */
        amp = sqrtf(p) * (4.0f / (float)n);
        if (amp > 65535.0f) amp = 65535.0f;
        s_spec_out[b] = kSpecHz[b] < (float)rate * 0.25f ? (int)amp : 0;
    }
}

void audio_player_get_spectrum(int *bands)
{
    int b;
    if (!bands) return;
    for (b = 0; b < AUDIO_SPEC_BANDS; b++) bands[b] = s_spec_out[b];
}

void audio_player_get_scope(short *points)
{
    int i;
    if (!points) return;
    for (i = 0; i < AUDIO_SCOPE_POINTS; i++) points[i] = s_scope_out[i];
}

