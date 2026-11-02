#include "plugin.h"

#include <math.h>
#include <pspkernel.h>

#define GLOW_BLOBS 4

typedef struct {
    float cx, cy, r;
    unsigned char cr, cg, cb;
    float sx, sy, sr;   // скорость/период анимации
    float px, py, pr;   // фаза
} Blob;

static Blob s_blobs[GLOW_BLOBS] = {
    { 80.0f,  45.0f, 88.0f, 255,  30, 150, 0.340f, 0.272f, 0.208f, 0.0f, 1.1f, 0.5f },
    { 125.0f, 95.0f, 66.0f, 255, 210,  30, 0.240f, 0.304f, 0.176f, 2.0f, 0.3f, 1.7f },
    { 170.0f, 42.0f, 38.0f, 255, 140,  20, 0.384f, 0.208f, 0.272f, 4.0f, 2.2f, 0.9f },
    { 120.0f, 68.0f, 130.0f, 110,  18,  95, 0.128f, 0.176f, 0.112f, 3.0f, 5.0f, 2.5f },
};

void glow_classic_set_palette(void)
{
    int i;
    for (i = 0; i < GLOW_BLOBS; i++) {
        const unsigned char *c = g_glow.pal[i % GLOW_PALETTE];
        s_blobs[i].cr = c[0];
        s_blobs[i].cg = c[1];
        s_blobs[i].cb = c[2];
    }
}

#define GLOW_BG_GRAY 26.0f

void glow_classic_update(float dim)
{
    float t = (float)g_glow.frame * 0.016f * (g_glow.playing ? 1.0f : 0.35f);
    float bx[GLOW_BLOBS], by[GLOW_BLOBS], br2[GLOW_BLOBS];
    float inv_r2[GLOW_BLOBS];
    float bright = g_glow.playing ? 1.0f : 0.55f;
    float pulse = 1.0f;
    float ar = (float)g_glow.pal[3][0], ag = (float)g_glow.pal[3][1], ab = (float)g_glow.pal[3][2];
    int gc = g_glow.cols, gr = g_glow.rows, stride = g_glow.cols + 1;
    float sx = (float)GLOW_W / (float)gc, sy = (float)GLOW_H / (float)gr;
    unsigned long long t_begin = sceKernelGetSystemTimeWide();
    int i, r, c;

    if (g_glow.frame < g_glow.pulse_until) {
        pulse = 1.0f + 0.45f * (float)(g_glow.pulse_until - g_glow.frame) / 45.0f;
    }
    bright *= pulse * dim;

    for (i = 0; i < GLOW_BLOBS; i++) {
        float rad;
        bx[i] = s_blobs[i].cx + sinf(t * s_blobs[i].sx + s_blobs[i].px) * 65.0f;
        by[i] = s_blobs[i].cy + sinf(t * s_blobs[i].sy + s_blobs[i].py) * 45.0f;
        rad = s_blobs[i].r * pulse + sinf(t * s_blobs[i].sr + s_blobs[i].pr) * 22.0f;
        br2[i] = rad * rad;
        inv_r2[i] = 1.0f / br2[i];
    }

    for (r = 0; r <= gr; r++) {
        float y = (float)r * sy;
        float ny = (y - GLOW_H * 0.5f) * (1.0f / (GLOW_H * 0.5f));
        for (c = 0; c <= gc; c++) {
            float x = (float)c * sx;
            float nx = (x - GLOW_W * 0.5f) * (1.0f / (GLOW_W * 0.5f));
            float sw = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
            float cov, a, cr, cg, cb, vig;

            for (i = 0; i < GLOW_BLOBS; i++) {
                float dx = x - bx[i];
                float dy = y - by[i];
                float d2 = dx * dx + dy * dy;
                if (d2 < br2[i]) {
                    float tn = 1.0f - d2 * inv_r2[i];
                    float w = tn * tn;
                    sw += w;
                    sr += s_blobs[i].cr * w;
                    sg += s_blobs[i].cg * w;
                    sb += s_blobs[i].cb * w;
                }
            }

            sw += 0.12f;
            sr += ar * 0.12f;
            sg += ag * 0.12f;
            sb += ab * 0.12f;

            cov = (sw > 1.6f) ? 1.6f : sw;
            cr = sr / sw;
            cg = sg / sw;
            cb = sb / sw;

            if (g_glow.liked) {
                cr += 28.0f;
                cg *= 0.87f;
                cb *= 0.78f;
            }

            vig = 1.0f - 0.35f * (nx * nx + ny * ny) * 0.5f;
            a = 0.10f + 0.90f * (cov * 0.625f);
            if (a > 1.0f) a = 1.0f;
            a *= vig * bright;
            if (a < 0.0f) a = 0.0f;
            if (cr > 255.0f) cr = 255.0f;
            if (cg > 255.0f) cg = 255.0f;
            if (cb > 255.0f) cb = 255.0f;

            /* alpha-композиция с фоном приложения */
            cr = cr * a + GLOW_BG_GRAY * (1.0f - a);
            cg = cg * a + GLOW_BG_GRAY * (1.0f - a);
            cb = cb * a + GLOW_BG_GRAY * (1.0f - a);
            g_glow.rgb[r * stride + c] = 0xFF000000u |
                ((unsigned int)(cb + 0.5f) << 16) |
                ((unsigned int)(cg + 0.5f) << 8) |
                (unsigned int)(cr + 0.5f);
        }
    }
    g_glow.draw_cols = gc;
    g_glow.draw_rows = gr;
    glow_grid_lod_feed((float)(sceKernelGetSystemTimeWide() - t_begin) * 0.001f);
}

static void update(float dt)
{
    (void)dt;
    glow_classic_update(1.0f);
}

const GlowPlugin glow_classic = {
    LOCALE_GLOW_STYLE_CLASSIC, 0, 0.16f, update, glow_grid_draw
};
