// Winamp-подобные визуализации (спектр / осциллограф / круг). См. ui_viz.h.
#include "ui/ui_viz.h"

#include <math.h>
#include <string.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "hal/hal_gpu.h"
#include "services/audio_player.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VIZ_BARS 32     /* столбиков: 16 полос, между соседними — интерполяция */
#define VIZ_SPOKES 48

typedef struct {
    unsigned int color;
    short x, y, z, pad;   /* 12 байт: как MeshVert сетки свечения */
} VizVert;

static float s_lv[AUDIO_SPEC_BANDS];       /* сглаженные уровни 0..1 */
static float s_pk[AUDIO_SPEC_BANDS];       /* «колпачки» */
static int s_pk_hold[AUDIO_SPEC_BANDS];
static float s_sc[AUDIO_SCOPE_POINTS];     /* форма волны -1..1, сглаженная */
static unsigned s_vframe = 0;

static float viz_sat(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* Амплитуда полосы (int16-единицы) -> 0..1 по дБ с наклоном вверх по частоте
 * (высокие полосы в музыке тише — без наклона правая половина спектра мертва). */
static float band_norm(int amp, int band)
{
    float x = (float)amp * (1.0f / 32768.0f);
    float db = 20.0f * log10f(x + 1e-6f) + 1.1f * (float)band;
    return viz_sat((db + 64.0f) / 46.0f);
}

void viz_update(int playing)
{
    int bands[AUDIO_SPEC_BANDS];
    short sc[AUDIO_SCOPE_POINTS];
    int i;

    audio_player_set_analysis(1);
    audio_player_get_spectrum(bands);
    audio_player_get_scope(sc);
    for (i = 0; i < AUDIO_SPEC_BANDS; i++) {
        float t = playing ? band_norm(bands[i], i) : 0.0f;
        if (t > s_lv[i]) {
            s_lv[i] += (t - s_lv[i]) * 0.65f;
        } else {
            s_lv[i] -= 0.04f;          /* Winamp: быстрое падение столбика */
            if (s_lv[i] < t) s_lv[i] = t;
            if (s_lv[i] < 0.0f) s_lv[i] = 0.0f;
        }
        if (s_lv[i] >= s_pk[i]) {
            s_pk[i] = s_lv[i];
            s_pk_hold[i] = 18;
        } else if (s_pk_hold[i] > 0) {
            s_pk_hold[i]--;
        } else {
            s_pk[i] -= 0.012f;         /* колпачок падает медленнее столбика */
            if (s_pk[i] < s_lv[i]) s_pk[i] = s_lv[i];
        }
    }
    for (i = 0; i < AUDIO_SCOPE_POINTS; i++) {
        float v = playing ? (float)sc[i] * (1.0f / 14000.0f) : 0.0f;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        s_sc[i] += (v - s_sc[i]) * 0.55f;
    }
    s_vframe++;
}

static unsigned int pack(float r, float g, float b, float a)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    if (a < 0) a = 0; else if (a > 255) a = 255;
    return ((unsigned int)(a + 0.5f) << 24) | ((unsigned int)(b + 0.5f) << 16) |
           ((unsigned int)(g + 0.5f) << 8) | (unsigned int)(r + 0.5f);
}

/* Классическая шкала анализатора Winamp: зелёный -> жёлтый -> оранжевый -> красный. */
static unsigned int level_color(float f, float alpha)
{
    static const float kp[4] = { 0.0f, 0.5f, 0.8f, 1.0f };
    static const float kc[4][3] = { { 30, 205, 40 }, { 235, 230, 30 },
                                    { 255, 140, 0 }, { 255, 35, 30 } };
    int z = f < kp[1] ? 0 : (f < kp[2] ? 1 : 2);
    float t = (f - kp[z]) / (kp[z + 1] - kp[z]);
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return pack(kc[z][0] + (kc[z + 1][0] - kc[z][0]) * t,
                kc[z][1] + (kc[z + 1][1] - kc[z][1]) * t,
                kc[z][2] + (kc[z + 1][2] - kc[z][2]) * t, alpha);
}

static VizVert *s_vp;
static void vput(float x, float y, unsigned int col)
{
    s_vp->color = col;
    s_vp->x = (short)(x + (x >= 0 ? 0.5f : -0.5f));
    s_vp->y = (short)(y + (y >= 0 ? 0.5f : -0.5f));
    s_vp->z = 0;
    s_vp->pad = 0;
    s_vp++;
}

static void begin_state(void)
{
    hal_gpu_set_texturing(0);
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    sceGuShadeModel(GU_SMOOTH);
}

static void end_state(void)
{
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    hal_gpu_invalidate_tex_state();
}

static float bar_level(const float *arr, int bar)
{
    /* 32 столбика по 16 полосам: чётные — сама полоса, нечётные — среднее соседей. */
    int b = bar >> 1;
    if ((bar & 1) == 0) return arr[b];
    if (b + 1 >= AUDIO_SPEC_BANDS) return arr[b];
    return 0.5f * (arr[b] + arr[b + 1]);
}

static void draw_spectrum(float x, float y, float w, float h)
{
    const float base = y + h - 14.0f;          /* ниже нижней панели: она перекроет */
    const float maxh = h * 0.60f;
    const float pitch = w / (float)VIZ_BARS;
    const float bw = pitch - 3.0f;
    VizVert *start, *p0;
    int n = 0, i;
    static const float zk[4] = { 0.0f, 0.5f, 0.8f, 1.0f };

    start = (VizVert *)sceGuGetMemory((unsigned)(VIZ_BARS * (3 * 6 + 6)) * sizeof(VizVert));
    if (!start) return;
    s_vp = start;
    p0 = s_vp;
    for (i = 0; i < VIZ_BARS; i++) {
        float lv = bar_level(s_lv, i);
        float pk = bar_level(s_pk, i);
        float x0 = x + pitch * (float)i + 1.5f, x1 = x0 + bw;
        int z;
        for (z = 0; z < 3; z++) {
            float a = zk[z], b = zk[z + 1];
            float top = lv < b ? lv : b;
            float ya, yb;
            unsigned int ca, cb;
            if (lv <= a) break;
            ya = base - a * maxh;
            yb = base - top * maxh;
            ca = level_color(a, 235.0f);
            cb = level_color(top, 235.0f);
            vput(x0, ya, ca); vput(x1, ya, ca); vput(x0, yb, cb);
            vput(x1, ya, ca); vput(x1, yb, cb); vput(x0, yb, cb);
        }
        if (pk > 0.02f) {
            float yp = base - pk * maxh - 3.0f;
            unsigned int cc = pack(235, 235, 235, 235);
            vput(x0, yp, cc); vput(x1, yp, cc); vput(x0, yp + 2.0f, cc);
            vput(x1, yp, cc); vput(x1, yp + 2.0f, cc); vput(x0, yp + 2.0f, cc);
        }
    }
    n = (int)(s_vp - p0);
    if (n > 0) {
        begin_state();
        sceGuDrawArray(GU_TRIANGLES,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, n, 0, start);
        end_state();
    }
}

static void draw_scope(float x, float y, float w, float h,
                       unsigned char ar, unsigned char ag, unsigned char ab)
{
    const float cy = y + h * 0.5f;
    const float amp = h * 0.30f;
    VizVert *start;
    int pass, i;
    /* осветлённый акцент */
    float r = ar + (255.0f - ar) * 0.45f, g = ag + (255.0f - ag) * 0.45f,
          b = ab + (255.0f - ab) * 0.45f;

    start = (VizVert *)sceGuGetMemory((unsigned)(2 * AUDIO_SCOPE_POINTS * 2) * sizeof(VizVert));
    if (!start) return;
    begin_state();
    for (pass = 0; pass < 2; pass++) {
        float t = pass == 0 ? 7.0f : 1.6f;
        unsigned int col = pass == 0 ? pack(r, g, b, 60.0f) : pack(r, g, b, 245.0f);
        VizVert *p0;
        s_vp = start;
        p0 = s_vp;
        for (i = 0; i < AUDIO_SCOPE_POINTS; i++) {
            float px = x + w * (float)i / (float)(AUDIO_SCOPE_POINTS - 1);
            float py = cy - s_sc[i] * amp;
            vput(px, py - t, col);
            vput(px, py + t, col);
        }
        sceGuDrawArray(GU_TRIANGLE_STRIP,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       (int)(s_vp - p0), 0, start);
    }
    end_state();
}

static void draw_radial(float x, float y, float w, float h,
                        unsigned char ar, unsigned char ag, unsigned char ab)
{
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    const float r0 = h * 0.20f, maxlen = h * 0.36f;
    const float rot = (float)s_vframe * 0.004f;
    VizVert *start, *p0;
    int k;
    float tr = ar + (255.0f - ar) * 0.6f, tg = ag + (255.0f - ag) * 0.6f,
          tb = ab + (255.0f - ab) * 0.6f;

    start = (VizVert *)sceGuGetMemory((unsigned)(VIZ_SPOKES * 3 + VIZ_SPOKES * 6) * sizeof(VizVert));
    if (!start) return;
    s_vp = start;
    p0 = s_vp;
    for (k = 0; k < VIZ_SPOKES; k++) {
        int half = VIZ_SPOKES / 2;
        int m = k < half ? k : (VIZ_SPOKES - 1 - k);       /* зеркало по вертикали */
        float fb = (float)m * (float)(AUDIO_SPEC_BANDS - 1) / (float)(half - 1);
        int b0 = (int)fb, b1 = b0 + 1 > AUDIO_SPEC_BANDS - 1 ? b0 : b0 + 1;
        float lv = s_lv[b0] + (s_lv[b1] - s_lv[b0]) * (fb - (float)b0);
        float ang = rot + (float)k * (float)(2.0 * M_PI / VIZ_SPOKES);
        float ca = cosf(ang), sa = sinf(ang);
        float len = 4.0f + lv * maxlen;
        float hw = 0.040f;                                  /* полуширина основания, рад */
        float c1 = cosf(ang - hw), s1 = sinf(ang - hw);
        float c2 = cosf(ang + hw), s2 = sinf(ang + hw);
        unsigned int cb = pack(ar, ag, ab, 200.0f);
        unsigned int ct = pack(tr, tg, tb, 20.0f + 235.0f * lv);
        vput(cx + c1 * r0, cy + s1 * r0, cb);
        vput(cx + c2 * r0, cy + s2 * r0, cb);
        vput(cx + ca * (r0 + len), cy + sa * (r0 + len), ct);
    }
    /* Кольцо у основания. */
    for (k = 0; k < VIZ_SPOKES; k++) {
        float a0 = (float)k * (float)(2.0 * M_PI / VIZ_SPOKES) + rot;
        float a1 = (float)(k + 1) * (float)(2.0 * M_PI / VIZ_SPOKES) + rot;
        float ri = r0 - 3.0f, ro = r0 - 0.5f;
        unsigned int cc = pack(tr, tg, tb, 170.0f);
        vput(cx + cosf(a0) * ri, cy + sinf(a0) * ri, cc);
        vput(cx + cosf(a0) * ro, cy + sinf(a0) * ro, cc);
        vput(cx + cosf(a1) * ri, cy + sinf(a1) * ri, cc);
        vput(cx + cosf(a0) * ro, cy + sinf(a0) * ro, cc);
        vput(cx + cosf(a1) * ro, cy + sinf(a1) * ro, cc);
        vput(cx + cosf(a1) * ri, cy + sinf(a1) * ri, cc);
    }
    begin_state();
    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   (int)(s_vp - p0), 0, start);
    end_state();
}

void viz_draw(VizKind kind, float x, float y, float w, float h,
              unsigned char acc_r, unsigned char acc_g, unsigned char acc_b)
{
    switch (kind) {
    case VIZ_SCOPE:  draw_scope(x, y, w, h, acc_r, acc_g, acc_b); break;
    case VIZ_RADIAL: draw_radial(x, y, w, h, acc_r, acc_g, acc_b); break;
    default:         draw_spectrum(x, y, w, h); break;
    }
}
