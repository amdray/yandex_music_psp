#include "plugin.h"

#include <math.h>
#include <string.h>
#include <pspgu.h>
#include <pspkernel.h>
#include "hal/hal_gpu.h"

#define GLOW_MASSES 2
#define FIELD_W 64
#define FIELD_H 36
#define SIN_N 1024
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float cx, cy, r;
    float sx, sy, sr;
    float px, py, pr;
} Mass;

static Mass s_masses[GLOW_MASSES] = {
    { 110.0f, 44.0f, 108.0f, 0.31f, 0.24f, 0.17f, 0.0f, 1.1f, 0.5f },
    { 120.0f, 128.0f, 130.0f, 0.16f, 0.10f, 0.11f, 2.0f, 0.3f, 1.7f },
};

static float s_field[FIELD_H][FIELD_W];
static float s_field2[FIELD_H][FIELD_W];
static float s_sin_tab[SIN_N + 1];
static int s_sin_ready = 0;

static float clampf0(float v)
{
    return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
}

static void sin_tab_init(void)
{
    int i;
    for (i = 0; i <= SIN_N; i++) {
        s_sin_tab[i] = sinf((float)i * (float)(2.0 * M_PI / SIN_N));
    }
    s_sin_ready = 1;
}

static float fsin(float a)
{
    float f;
    int i;
    f = a * (float)(SIN_N / (2.0 * M_PI));
    f -= floorf(f);
    f *= (float)SIN_N;
    i = (int)f;
    return s_sin_tab[i] + (s_sin_tab[i + 1] - s_sin_tab[i]) * (f - (float)i);
}

static float sample_field(const float *f, float u, float v)
{
    float fx = u * (FIELD_W - 1), fy = v * (FIELD_H - 1);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1, y1 = y0 + 1;
    float tx = fx - (float)x0, ty = fy - (float)y0;
    float a, b;
    if (x1 > FIELD_W - 1) x1 = FIELD_W - 1;
    if (y1 > FIELD_H - 1) y1 = FIELD_H - 1;
    a = f[y0 * FIELD_W + x0] + (f[y0 * FIELD_W + x1] - f[y0 * FIELD_W + x0]) * tx;
    b = f[y1 * FIELD_W + x0] + (f[y1 * FIELD_W + x1] - f[y1 * FIELD_W + x0]) * tx;
    return a + (b - a) * ty;
}

static void strands_build_fields(float t)
{
    int x, y;
    for (y = 0; y < FIELD_H; y++) {
        float v = (float)y * (1.0f / (FIELD_H - 1));
        for (x = 0; x < FIELD_W; x++) {
            float u = (float)x * (1.0f / (FIELD_W - 1));
            float uw = u + 0.15f * fsin(v * 6.2f + t * 0.70f + 1.3f);
            float vw = v + 0.11f * fsin(u * 5.1f - t * 0.55f + 0.4f);
            float f1 = 0.50f
                + 0.30f * fsin(uw * 6.5f + t * 0.60f)
                + 0.28f * fsin(vw * 5.0f - t * 0.45f + 2.1f)
                + 0.16f * fsin(uw * 3.0f + vw * 4.0f + t * 0.35f + 4.0f);
            float f2 = 0.50f
                + 0.32f * fsin(uw * 4.5f - vw * 3.5f + t * 0.50f + 5.2f)
                + 0.28f * fsin(vw * 6.5f + t * 0.62f + 1.1f);
            s_field[y][x] = f1;
            s_field2[y][x] = f2;
        }
    }
}

static void mix_white(const unsigned char *c, float k, float *out)
{
    out[0] = (float)c[0] + (255.0f - (float)c[0]) * k;
    out[1] = (float)c[1] + (255.0f - (float)c[1]) * k;
    out[2] = (float)c[2] + (255.0f - (float)c[2]) * k;
}

static void update(float dt)
{
    (void)dt;
    if (!s_sin_ready) sin_tab_init();
    float t = (float)g_glow.frame * 0.016f * (g_glow.playing ? 1.0f : 0.35f);
    float bright = g_glow.playing ? 1.0f : 0.55f;
    float pulse = 1.0f;
    float mx[GLOW_MASSES], my[GLOW_MASSES], mr[GLOW_MASSES];
    float inv_r2[GLOW_MASSES];
    float top[3], bot[3], up[3], pool[3], halo_col[3], core_col[3];
    int i, x, y;

    if (g_glow.frame < g_glow.pulse_until) {
        pulse = 1.0f + 0.45f * (float)(g_glow.pulse_until - g_glow.frame) / 45.0f;
    }
    bright *= pulse;

    strands_build_fields(t);

    for (i = 0; i < 3; i++) {
        float b = (float)g_glow.pal[3][i];
        float a1 = (float)g_glow.pal[0][i];
        float av = (float)g_glow.pal[2][i];
        top[i] = b * 0.60f + 4.0f;
        bot[i] = b * 1.70f + av * 0.25f + 10.0f;
        up[i] = a1 * 0.50f + (float)g_glow.pal[1][i] * 0.16f;
        pool[i] = clampf0(av * 1.15f + 38.0f);
    }
    mix_white(g_glow.pal[0], 0.30f, halo_col);
    mix_white(g_glow.pal[0], 0.62f, core_col);

    for (i = 0; i < GLOW_MASSES; i++) {
        mx[i] = s_masses[i].cx + sinf(t * s_masses[i].sx + s_masses[i].px) * 38.0f;
        my[i] = s_masses[i].cy + sinf(t * s_masses[i].sy + s_masses[i].py) * 26.0f;
        mr[i] = s_masses[i].r * pulse + sinf(t * s_masses[i].sr + s_masses[i].pr) * 16.0f;
        inv_r2[i] = 1.0f / (mr[i] * mr[i]);
    }

    for (y = 0; y < GLOW_H; y++) {
        float v = (float)y * (1.0f / (GLOW_H - 1));
        float ny = (v - 0.5f) * 2.0f;
        float ny2 = ny * ny;
        float row[3];
        float smask = (1.0f - 0.82f * v) * bright;
        for (i = 0; i < 3; i++) {
            row[i] = top[i] + (bot[i] - top[i]) * v;
        }

        for (x = 0; x < GLOW_W; x++) {
            float u = (float)x * (1.0f / (GLOW_W - 1));
            float nx = (u - 0.5f) * 2.0f;
            float f1, f2, d, q, halo, core, w[GLOW_MASSES];
            float cr, cg, cb, vig;
            unsigned char *p = g_glow.texture + (size_t)(y * GLOW_PITCH + x) * 4;

            for (i = 0; i < GLOW_MASSES; i++) {
                float dx = (float)x - mx[i];
                float dy = (float)y - my[i];
                float d2 = dx * dx + dy * dy;
                float r2 = 1.0f / inv_r2[i];
                w[i] = 0.0f;
                if (d2 < r2) {
                    float tn = 1.0f - d2 * inv_r2[i];
                    w[i] = tn * tn;
                }
            }

            f1 = sample_field(&s_field[0][0], u, v);
            d = f1 - 0.5f;
            if (d < 0.0f) d = -d;
            q = 1.0f / (1.0f + d * d * 200.0f);
            halo = q;
            core = q * q;
            core *= core;
            core *= core;

            f2 = sample_field(&s_field2[0][0], u, v);
            d = f2 - 0.5f;
            if (d < 0.0f) d = -d;
            q = 1.0f / (1.0f + d * d * 200.0f);
            if (q > halo) halo = q;
            {
                float c2 = q * q;
                c2 *= c2;
                c2 *= c2;
                core += 0.8f * c2;
            }

            cr = row[0] + w[0] * up[0] * 0.45f * bright
                       + w[1] * pool[0] * 0.45f * bright;
            cg = row[1] + w[0] * up[1] * 0.45f * bright
                       + w[1] * pool[1] * 0.45f * bright;
            cb = row[2] + w[0] * up[2] * 0.45f * bright
                       + w[1] * pool[2] * 0.45f * bright;

            cr += halo * halo_col[0] * 0.18f * smask + core * core_col[0] * 0.40f * smask;
            cg += halo * halo_col[1] * 0.18f * smask + core * core_col[1] * 0.40f * smask;
            cb += halo * halo_col[2] * 0.18f * smask + core * core_col[2] * 0.40f * smask;

            if (g_glow.liked) {
                cr += 10.0f;
                cg -= 4.0f;
                cb -= 10.0f;
            }

            vig = 1.0f - 0.55f * (nx * nx * 0.75f + ny2 * 0.45f);
            cr *= vig;
            cg *= vig;
            cb *= vig;

            p[0] = (unsigned char)clampf0(cr);
            p[1] = (unsigned char)clampf0(cg);
            p[2] = (unsigned char)clampf0(cb);
            p[3] = 255;
        }
    }
    // Padding для LINEAR-фильтра: реплика последнего столбца/строки (240x136).
    {
        for (y = 0; y < GLOW_H; y++) {
            for (x = GLOW_W; x < GLOW_PITCH; x++) {
                memcpy(g_glow.texture + (size_t)(y * GLOW_PITCH + x) * 4,
                       g_glow.texture + (size_t)(y * GLOW_PITCH + GLOW_W - 1) * 4, 4);
            }
        }
        memcpy(g_glow.texture + (size_t)(GLOW_H * GLOW_PITCH) * 4,
               g_glow.texture + (size_t)((GLOW_H - 1) * GLOW_PITCH) * 4,
               (size_t)GLOW_PITCH * 4);
    }
}

typedef struct {
    unsigned short u, v;
    short x, y, z;
} GlowVert;

static void draw(float x, float y, float w, float h)
{
    // Только заполненные строки (+1 строка паддинга), а не все 256 КБ.
    sceKernelDcacheWritebackRange(g_glow.texture, (size_t)GLOW_PITCH * (GLOW_H + 1) * 4);
    hal_gpu_set_texturing(1);
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    // Сброс кэша дескрипторов GE: без него GE может держать текстуру шрифта
    // от прошлых отрисовок и misread-ить g_glow.texture полосами (как в ui_draw.c).
    sceGuTexFlush();
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, GLOW_PITCH, GLOW_TEX_H, GLOW_PITCH, g_glow.texture);

    {
        GlowVert *v = (GlowVert *)sceGuGetMemory(2 * sizeof(GlowVert));
        v[0].u = 0;
        v[0].v = 0;
        v[0].x = (short)x;
        v[0].y = (short)y;
        v[0].z = 0;
        v[1].u = (unsigned short)GLOW_W;
        v[1].v = (unsigned short)GLOW_H;
        v[1].x = (short)(x + w);
        v[1].y = (short)(y + h);
        v[1].z = 0;
        sceGuDrawArray(GU_SPRITES,
                       GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2, 0, v);
    }

    /* Restore the app-wide baseline (blend enabled): leaving it disabled here
     * would drop anti-aliasing on all later text and break alpha overlays
     * (volume light) for every subsequent frame. */
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    hal_gpu_set_texturing(0);
    hal_gpu_invalidate_tex_state();
}

const GlowPlugin glow_strands = {
    LOCALE_GLOW_STYLE_STRANDS, 1, 0.16f, update, draw
};
