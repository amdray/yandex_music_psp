#include "ui/ui_glow_bg.h"
#include "glow/plugin.h"
#include "ui/ui_draw.h"
#include "ui/ui_theme.h"
#include "hal/hal_gpu.h"
#include "core/fs.h"
#include "core/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <math.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>

#define GLOW_STYLE_PATH "config/glow.cfg"

static unsigned int s_grid_rgb[(WG_MAXR + 1) * (WG_MAXC + 1)];
GlowState g_glow = {
    .playing = 1, .like_clock = -1.0f,
    .pal = {{255, 40, 190}, {255, 210, 30}, {140, 30, 160}, {36, 6, 48}},
    .avg = {140, 30, 160},
    .rgb = s_grid_rgb, .cols = 88, .rows = 50, .draw_cols = 88, .draw_rows = 50
};
static unsigned int s_accent_color;

void glow_bg_set_accent(unsigned int color)
{
    s_accent_color = color;
}

static void accent_draw(float x, float y, float w, float h)
{
    if (!s_accent_color) return;
    /* One grid cell keeps the server color independent of the UI theme. */
    g_glow.draw_cols = g_glow.draw_rows = 1;
    for (int i = 0; i < 4; ++i) g_glow.rgb[i] = s_accent_color;
    glow_grid_draw(x, y, w, h);
}

/* Static modes neither update animation nor sample audio. */
static const GlowPlugin glow_off = { LOCALE_GLOW_STYLE_OFF, 0, 0.0f, NULL, NULL };
static const GlowPlugin glow_accent = { LOCALE_GLOW_STYLE_ACCENT, 0, 0.0f, NULL, accent_draw };
static const GlowPlugin *const kPlugins[GLOW_STYLE_COUNT] = {
    &glow_classic, &glow_strands, &glow_web,
    &glow_spectrum, &glow_scope, &glow_radial, &glow_off, &glow_accent
};
static int s_style = GLOW_STYLE_DEFAULT;
static unsigned s_last_pulse_frame;
static unsigned long long s_last_us;

void glow_bg_set_playing(int playing)
{
    g_glow.playing = playing ? 1 : 0;
}

void glow_bg_set_liked(int liked)
{
    liked = liked ? 1 : 0;
    // Анимация лайка — только когда лайк поставили на ТОМ ЖЕ треке (как в
    // вебе: сравнивается id трека); смена трека/подгрузка статуса лайка
    // (pulse недавно) — без вспышки.
    if (liked && !g_glow.liked && g_glow.frame > 30 &&
        (g_glow.frame - s_last_pulse_frame) > 120u) {
        g_glow.like_clock = 0.0f;
    }
    g_glow.liked = liked;
}

void glow_bg_pulse(void)
{
    g_glow.pulse_until = g_glow.frame + 45;  // ~0.75 сек при 60fps
    s_last_pulse_frame = g_glow.frame;
}

int glow_bg_get_style(void)
{
    return s_style;
}

LocaleKey glow_bg_style_name_key(int style)
{
    return kPlugins[style >= 0 && style < GLOW_STYLE_COUNT ? style : GLOW_STYLE_DEFAULT]->name;
}

static void glow_config_load(void)
{
    char buffer[32];
    SceUID fd;
    int count, parsed;
    char trailing;

    s_style = GLOW_STYLE_DEFAULT;
    fd = fs_open(GLOW_STYLE_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    count = fs_read(fd, buffer, sizeof(buffer) - 1);
    fs_close(fd);
    if (count <= 0) return;
    buffer[count] = '\0';
    parsed = -1;
    if (sscanf(buffer, "style=%d %c", &parsed, &trailing) == 1 &&
        parsed >= 0 && parsed < GLOW_STYLE_COUNT) {
        s_style = parsed;
    }
    logLine("glow: style=%d source=config\n", s_style);
}

void glow_bg_set_style(int style)
{
    char buffer[24];
    int length;

    if (style < 0 || style >= GLOW_STYLE_COUNT) return;
    if (s_style == style) return;
    s_style = style;
    length = snprintf(buffer, sizeof(buffer), "style=%d\n", style);
    if (length <= 0 || length >= (int)sizeof(buffer)) return;
    fs_write_deferred(GLOW_STYLE_PATH, buffer, (size_t)length);  /* в фоне */
    logLine("glow: style=%d queued\n", style);
}

static void boost_saturation(unsigned char *c, float k)
{
    float r = (float)c[0], g = (float)c[1], b = (float)c[2];
    float mean = (r + g + b) * (1.0f / 3.0f);
    float v;
    v = mean + (r - mean) * k;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    c[0] = (unsigned char)v;
    v = mean + (g - mean) * k;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    c[1] = (unsigned char)v;
    v = mean + (b - mean) * k;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    c[2] = (unsigned char)v;
}

static void lift_value(unsigned char *c, int target)
{
    int mx = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    int i;
    if (mx <= 0 || mx >= target) return;
    for (i = 0; i < 3; i++) {
        int v = (int)c[i] * target / mx;
        c[i] = (unsigned char)(v > 255 ? 255 : v);
    }
}

void glow_bg_set_cover(const void *rgba, int w, int h, int stride_bytes)
{
    const unsigned char *p = (const unsigned char *)rgba;
    int step_x, step_y, x, y;
    long sr = 0, sg = 0, sb = 0, n = 0;
    unsigned char acc1[3] = {0, 0, 0}, acc2[3] = {0, 0, 0};
    long best1 = -1, best2 = -1;
    unsigned long avg[3];

    if (!p || w < 8 || h < 8 || stride_bytes < w * 4) return;
    step_x = w / 40; if (step_x < 1) step_x = 1;
    step_y = h / 40; if (step_y < 1) step_y = 1;

    /* Проход 1: средний цвет + самый насыщенно-яркий (акцент). */
    for (y = 0; y < h; y += step_y) {
        const unsigned char *row = p + (size_t)y * stride_bytes;
        for (x = 0; x < w; x += step_x) {
            int r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
            int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            long score;
            if (r + g + b < 70) continue;
            if (r > 244 && g > 244 && b > 244) continue;
            sr += r; sg += g; sb += b; n++;
            score = (long)(mx - mn) * mx;
            if (score > best1) {
                best1 = score;
                acc1[0] = (unsigned char)r;
                acc1[1] = (unsigned char)g;
                acc1[2] = (unsigned char)b;
            }
        }
    }
    if (n == 0) return;

    /* Проход 2: второй акцент — дальний по цвету от первого. */
    for (y = 0; y < h; y += step_y) {
        const unsigned char *row = p + (size_t)y * stride_bytes;
        for (x = 0; x < w; x += step_x) {
            int r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
            int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            int dist = abs(r - acc1[0]) + abs(g - acc1[1]) + abs(b - acc1[2]);
            long score;
            if (r + g + b < 70 || (r > 244 && g > 244 && b > 244)) continue;
            if (dist < 150) continue;
            score = (long)(mx - mn) * mx;
            if (score > best2) {
                best2 = score;
                acc2[0] = (unsigned char)r;
                acc2[1] = (unsigned char)g;
                acc2[2] = (unsigned char)b;
            }
        }
    }
    if (best2 < 0) {
        acc2[0] = acc1[0]; acc2[1] = acc1[1]; acc2[2] = acc1[2];
    }

    avg[0] = (unsigned long)(sr / n);
    avg[1] = (unsigned long)(sg / n);
    avg[2] = (unsigned long)(sb / n);

    {
        unsigned char avg_c[3];
        avg_c[0] = (unsigned char)avg[0]; avg_c[1] = (unsigned char)avg[1]; avg_c[2] = (unsigned char)avg[2];
        ui_theme_set_cover(acc1, acc2, avg_c);
    }
    g_glow.pal[0][0] = acc1[0]; g_glow.pal[0][1] = acc1[1]; g_glow.pal[0][2] = acc1[2];
    g_glow.pal[1][0] = acc2[0]; g_glow.pal[1][1] = acc2[1]; g_glow.pal[1][2] = acc2[2];
    g_glow.pal[2][0] = (unsigned char)avg[0];
    g_glow.pal[2][1] = (unsigned char)avg[1];
    g_glow.pal[2][2] = (unsigned char)avg[2];
    g_glow.avg[0] = (unsigned char)avg[0];
    g_glow.avg[1] = (unsigned char)avg[1];
    g_glow.avg[2] = (unsigned char)avg[2];
    {
        int i;
        for (i = 0; i < 3; i++) {
            int v = (int)acc1[i] * 16 / 100 + (int)avg[i] * 12 / 100;
            g_glow.pal[3][i] = (unsigned char)(v > 255 ? 255 : v);
        }
    }

    boost_saturation(g_glow.pal[0], 1.45f);
    boost_saturation(g_glow.pal[1], 1.35f);
    boost_saturation(g_glow.pal[2], 1.25f);
    lift_value(g_glow.pal[0], 215);
    lift_value(g_glow.pal[1], 205);
    lift_value(g_glow.pal[2], 180);
    glow_classic_set_palette();
}

/* Accent and classic palette colors gate the scrim by similarity to text.
 * Scrim strength follows sampled background brightness. */
static float s_scrim_a = 0.16f;

static float color_linear_channel(unsigned int channel)
{
    float value = (float)channel / 255.0f;
    return value <= 0.04045f ? value / 12.92f
                           : powf((value + 0.055f) / 1.055f, 2.4f);
}

/* Oklab matrices: https://bottosson.github.io/posts/oklab/ */
static void color_oklab(unsigned int color, float lab[3])
{
    static const float rgb_to_lms[3][3] = {
        {0.4122214708f, 0.5363325363f, 0.0514459929f},
        {0.2119034982f, 0.6806995451f, 0.1073969566f},
        {0.0883024619f, 0.2817188376f, 0.6299787005f}
    };
    static const float lms_to_lab[3][3] = {
        {0.2104542553f, 0.7936177850f, -0.0040720468f},
        {1.9779984951f, -2.4285922050f, 0.4505937099f},
        {0.0259040371f, 0.7827717662f, -0.8086757660f}
    };
    float rgb[3] = {
        color_linear_channel(color & 255u),
        color_linear_channel((color >> 8) & 255u),
        color_linear_channel((color >> 16) & 255u)
    };
    float lms[3];
    int row;
    for (row = 0; row < 3; ++row) {
        lms[row] = cbrtf(rgb_to_lms[row][0] * rgb[0] +
                         rgb_to_lms[row][1] * rgb[1] +
                         rgb_to_lms[row][2] * rgb[2]);
    }
    for (row = 0; row < 3; ++row) {
        lab[row] = lms_to_lab[row][0] * lms[0] +
                   lms_to_lab[row][1] * lms[1] +
                   lms_to_lab[row][2] * lms[2];
    }
}

static int color_needs_scrim(unsigned int color,
                             const unsigned int *text_colors, size_t count)
{
    /* CSS Color 4: one just noticeable Oklab difference is 0.02. */
    const float jnd = 0.02f;
    float background[3];
    size_t i;
    color_oklab(color, background);
    for (i = 0; i < count; ++i) {
        float text[3], dl, da, db;
        if ((text_colors[i] >> 24) == 0) continue;
        if (i > 0 && text_colors[i] == text_colors[i - 1]) continue;
        color_oklab(ui_theme_map(text_colors[i]), text);
        dl = text[0] - background[0];
        da = text[1] - background[1];
        db = text[2] - background[2];
        if (dl * dl + da * da + db * db < jnd * jnd) return 1;
    }
    return 0;
}

static int classic_needs_scrim(const unsigned int *text_colors, size_t count)
{
    int i;
    for (i = 0; i < GLOW_PALETTE; ++i) {
        unsigned int color = 0xFF000000u | (unsigned int)g_glow.pal[i][0] |
            ((unsigned int)g_glow.pal[i][1] << 8) |
            ((unsigned int)g_glow.pal[i][2] << 16);
        if (color_needs_scrim(color, text_colors, count)) return 1;
    }
    return 0;
}

static float glow_region_luma(float x0, float y0, float x1, float y1)
{
    unsigned hist[16];
    unsigned n = 0, acc = 0, want;
    int b;
    if (s_style == GLOW_STYLE_ACCENT) {
        unsigned int px = s_accent_color;
        return (float)((77u * (px & 255u) + 150u * ((px >> 8) & 255u) +
                        29u * ((px >> 16) & 255u)) >> 8);
    }
    memset(hist, 0, sizeof(hist));
    if (kPlugins[s_style]->textured) {
        int ix0 = (int)(x0 * 0.5f), ix1 = (int)(x1 * 0.5f);
        int iy0 = (int)(y0 * 0.5f), iy1 = (int)(y1 * 0.5f);
        int ix, iy;
        if (!g_glow.texture) return 0.0f;
        if (ix1 > GLOW_W - 1) ix1 = GLOW_W - 1;
        if (iy1 > GLOW_H - 1) iy1 = GLOW_H - 1;
        for (iy = iy0; iy <= iy1; iy += 6) {
            for (ix = ix0; ix <= ix1; ix += 6) {
                const unsigned char *p = g_glow.texture + (size_t)(iy * GLOW_PITCH + ix) * 4;
                unsigned l = (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
                hist[l >> 4]++;
                n++;
            }
        }
    } else {
        int gc = g_glow.draw_cols, gr = g_glow.draw_rows;
        int c0 = (int)(x0 * (float)gc / 480.0f), c1 = (int)(x1 * (float)gc / 480.0f + 0.999f);
        int r0 = (int)(y0 * (float)gr / 272.0f), r1 = (int)(y1 * (float)gr / 272.0f + 0.999f);
        int c, r;
        if (c0 < 0) c0 = 0;
        if (r0 < 0) r0 = 0;
        if (c1 > gc) c1 = gc;
        if (r1 > gr) r1 = gr;
        for (r = r0; r <= r1; r += 2) {
            for (c = c0; c <= c1; c += 2) {
                unsigned px = g_glow.rgb[r * (gc + 1) + c];
                unsigned l = (77u * (px & 255u) + 150u * ((px >> 8) & 255u) +
                              29u * ((px >> 16) & 255u)) >> 8;
                hist[l >> 4]++;
                n++;
            }
        }
    }
    if (n == 0) return 0.0f;
    want = (n * 85u + 99u) / 100u;
    for (b = 0; b < 16; b++) {
        acc += hist[b];
        if (acc >= want) return (float)(b * 16 + 8);
    }
    return 255.0f;
}

/* Same normalized smoothstep curve as wg_sstep; indices stay within the ramp. */
static float scrim_axis_weight(int index, int feather)
{
    float t;
    if (index <= feather) {
        t = (float)index / (float)feather;
    } else if (index == feather + 1) {
        return 1.0f;
    } else {
        t = (float)(2 * feather + 1 - index) / (float)feather;
    }
    return t * t * (3.0f - 2.0f * t);
}

static float scrim_axis_position(int index, int feather, float begin, float end)
{
    return index <= feather ? begin - (float)feather + (float)index
                            : end + (float)(index - feather - 1);
}

void glow_bg_text_scrim(const UiGlowTextRegion *region,
                       const unsigned int *text_colors, size_t color_count)
{
    static unsigned cnt = 0;
    static float luma = 0.0f;
    float want, a;
    GlowMeshVert *v, *p;
    int r, c, feather, stride, rows;

    if (s_style == GLOW_STYLE_OFF ||
        (s_style == GLOW_STYLE_ACCENT && !s_accent_color)) return;
    if (s_style == GLOW_STYLE_ACCENT &&
        !color_needs_scrim(s_accent_color, text_colors, color_count)) return;
    if (s_style == GLOW_STYLE_CLASSIC &&
        !classic_needs_scrim(text_colors, color_count)) return;
    if (!hal_gpu_in_frame()) return;
    feather = (int)ceilf(region->feather);
    if (feather <= 0) return;
    stride = 2 * feather + 2;
    rows = stride - 1;
    hal_gpu_set_texturing(0);
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    sceGuShadeModel(GU_SMOOTH);
    if (s_style == GLOW_STYLE_ACCENT || (cnt++ % 6u) == 0u)
        luma = glow_region_luma(region->left, region->top,
                                region->right, region->bottom);
    want = luma > 50.0f ? 1.0f - 50.0f / luma : 0.0f;
    if (want < kPlugins[s_style]->scrim_floor) want = kPlugins[s_style]->scrim_floor;
    if (want > 0.68f) want = 0.68f;
    s_scrim_a += (want - s_scrim_a) * 0.08f;
    a = s_scrim_a * 255.0f * ((float)ui_draw_get_alpha() * (1.0f / 255.0f));

    v = (GlowMeshVert *)sceGuGetMemory((unsigned)(rows * stride * 2) * sizeof(GlowMeshVert));
    if (!v) return;
    p = v;
    for (r = 0; r < rows; r++) {
        GlowMeshVert *strip = p;
        float y0 = scrim_axis_position(r, feather, region->top, region->bottom);
        float y1 = scrim_axis_position(r + 1, feather, region->top, region->bottom);
        float ay0 = scrim_axis_weight(r, feather);
        float ay1 = scrim_axis_weight(r + 1, feather);
        for (c = 0; c < stride; c++) {
            float x = scrim_axis_position(c, feather, region->left, region->right);
            float ax = scrim_axis_weight(c, feather);
            p->color = ((unsigned int)(a * ax * ay0 + 0.5f)) << 24;
            p->x = (short)(x + 0.5f); p->y = (short)(y0 + 0.5f); p->z = 0; p->pad = 0;
            p++;
            p->color = ((unsigned int)(a * ax * ay1 + 0.5f)) << 24;
            p->x = (short)(x + 0.5f); p->y = (short)(y1 + 0.5f); p->z = 0; p->pad = 0;
            p++;
        }
        sceGuDrawArray(GU_TRIANGLE_STRIP,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2 * stride, 0, strip);
    }
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    hal_gpu_invalidate_tex_state();
}

void glow_bg_init(void)
{
    if (g_glow.texture) {
        return;
    }
    g_glow.texture = (unsigned char *)memalign(64, (size_t)GLOW_PITCH * GLOW_TEX_H * 4);
    if (g_glow.texture) {
        memset(g_glow.texture, 0, (size_t)GLOW_PITCH * GLOW_TEX_H * 4);
    }
    g_glow.frame = 0;
    glow_config_load();
}

void glow_bg_free(void)
{
    if (g_glow.texture) {
        free(g_glow.texture);
        g_glow.texture = NULL;
    }
}

void glow_bg_draw(float x, float y, float w, float h)
{
    unsigned long long now;
    float dt;
    const GlowPlugin *plugin = kPlugins[s_style];
    if (!plugin->update) {
        if (plugin->draw) plugin->draw(x, y, w, h);
        return;
    }
    if (!g_glow.texture) return;
    now = sceKernelGetSystemTimeWide();
    dt = s_last_us ? (float)(now - s_last_us) / 1000000.0f : 0.033f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    s_last_us = now;
    plugin->update(dt);
    g_glow.frame++;
    plugin->draw(x, y, w, h);
}
