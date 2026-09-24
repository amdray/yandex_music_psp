// Светящийся фон как в Яндекс Музыке (vibe). См. ui_glow_bg.h.
#include "ui/ui_glow_bg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "hal/hal_gpu.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define GLOW_W 240
#define GLOW_H 136
#define GLOW_PITCH 256
#define GLOW_TEX_H 256  // степень двойки для GU
#define GLOW_BLOBS 4

typedef struct {
    float cx, cy, r;
    unsigned char cr, cg, cb;
    float sx, sy, sr;   // скорость/период анимации
    float px, py, pr;   // фаза
} Blob;

static unsigned char *s_buf = NULL;
static unsigned s_frame = 0;
static int s_playing = 1;
static int s_liked = 0;
static unsigned s_pulse_until = 0;

static Blob s_blobs[GLOW_BLOBS] = {
    // Маджента — большое пятно сверху-слева
    { 80.0f,  45.0f, 78.0f, 255,  30, 150, 0.340f, 0.272f, 0.208f, 0.0f, 1.1f, 0.5f },
    // Жёлтый — среднее снизу-по-центру
    { 125.0f, 95.0f, 58.0f, 255, 210,  30, 0.240f, 0.304f, 0.176f, 2.0f, 0.3f, 1.7f },
    // Оранжевый — маленький акцент справа
    { 170.0f, 42.0f, 30.0f, 255, 140,  20, 0.384f, 0.208f, 0.272f, 4.0f, 2.2f, 0.9f },
    // Фиолетовая дымка — огромное слабое пятно
    { 120.0f, 68.0f, 115.0f, 110,  18,  95, 0.128f, 0.176f, 0.112f, 3.0f, 5.0f, 2.5f },
};

typedef struct {
    unsigned short u, v;
    short x, y, z;
} GlowVert;

void glow_bg_set_playing(int playing)
{
    s_playing = playing ? 1 : 0;
}

void glow_bg_set_liked(int liked)
{
    s_liked = liked ? 1 : 0;
}

void glow_bg_pulse(void)
{
    s_pulse_until = s_frame + 45;  // ~0.75 сек при 60fps
}

static void glow_update(void)
{
    float t = (float)s_frame * 0.016f * (s_playing ? 1.0f : 0.35f);
    float bx[GLOW_BLOBS], by[GLOW_BLOBS], br[GLOW_BLOBS];
    float inv_r2[GLOW_BLOBS];
    float bright = s_playing ? 1.0f : 0.55f;
    float pulse = 1.0f;
    int i, x, y;

    if (s_frame < s_pulse_until) {
        pulse = 1.0f + 0.45f * (float)(s_pulse_until - s_frame) / 45.0f;
    }
    bright *= pulse;

    for (i = 0; i < GLOW_BLOBS; i++) {
        bx[i] = s_blobs[i].cx + sinf(t * s_blobs[i].sx + s_blobs[i].px) * 65.0f;
        by[i] = s_blobs[i].cy + sinf(t * s_blobs[i].sy + s_blobs[i].py) * 45.0f;
        br[i] = s_blobs[i].r * pulse + sinf(t * s_blobs[i].sr + s_blobs[i].pr) * 22.0f;
        inv_r2[i] = 1.0f / (br[i] * br[i]);
    }

    for (y = 0; y < GLOW_H; y++) {
        for (x = 0; x < GLOW_W; x++) {
            float sw = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
            for (i = 0; i < GLOW_BLOBS; i++) {
                float dx = (float)x - bx[i];
                float dy = (float)y - by[i];
                float d2 = dx * dx + dy * dy;
                float r2 = 1.0f / inv_r2[i];
                if (d2 < r2) {
                    float tn = 1.0f - d2 * inv_r2[i];
                    float w = tn * tn;
                    sw += w;
                    sr += s_blobs[i].cr * w;
                    sg += s_blobs[i].cg * w;
                    sb += s_blobs[i].cb * w;
                }
            }
            unsigned char *p = s_buf + (size_t)(y * GLOW_PITCH + x) * 4;
            if (sw > 0.003f) {
                float inv_w = 1.0f / sw;
                float a = (sw > 1.0f) ? 1.0f : sw;
                float cr = sr * inv_w;
                float cg = sg * inv_w;
                float cb = sb * inv_w;
                // Лайк — тёплый оттенок (больше красного).
                if (s_liked) {
                    cr = cr * 1.0f + 30.0f * a;
                    cg = cg * 0.85f;
                    cb = cb * 0.75f;
                }
                a *= bright;
                if (a > 1.0f) a = 1.0f;
                p[0] = (unsigned char)cr;
                p[1] = (unsigned char)cg;
                p[2] = (unsigned char)cb;
                p[3] = (unsigned char)(a * 255.0f);
            } else {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
        }
    }
    // Правая колонка padding — реплика последней видимой (для LINEAR).
    for (y = 0; y < GLOW_H; y++) {
        memcpy(s_buf + (size_t)(y * GLOW_PITCH + GLOW_W) * 4,
               s_buf + (size_t)(y * GLOW_PITCH + GLOW_W - 1) * 4,
               (size_t)(GLOW_PITCH - GLOW_W) * 4);
    }
}

void glow_bg_init(void)
{
    if (s_buf) {
        return;
    }
    s_buf = (unsigned char *)memalign(64, (size_t)GLOW_PITCH * GLOW_TEX_H * 4);
    if (s_buf) {
        memset(s_buf, 0, (size_t)GLOW_PITCH * GLOW_TEX_H * 4);
    }
    s_frame = 0;
}

void glow_bg_free(void)
{
    if (s_buf) {
        free(s_buf);
        s_buf = NULL;
    }
}

void glow_bg_draw(float x, float y, float w, float h)
{
    (void)x; (void)y; (void)w; (void)h;
    if (!s_buf) {
        return;
    }
    // Обновление текстуры каждый кадр.
    glow_update();
    s_frame++;

    sceKernelDcacheWritebackRange(s_buf, (size_t)GLOW_PITCH * GLOW_TEX_H * 4);
    hal_gpu_set_texturing(1);
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, GLOW_PITCH, GLOW_TEX_H, GLOW_PITCH, s_buf);

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

    hal_gpu_set_blend(0, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    hal_gpu_set_texturing(0);
    hal_gpu_invalidate_tex_state();
}
