#include "plugin.h"

#include <pspgu.h>
#include "hal/hal_gpu.h"
#include "core/logger.h"

// Макс. сетка и уровни детализации (LOD). Уровень выбирается по замеру времени
// кадра (виртуальное время PPSSPP ≈ реальная PSP): 0 — 120x68, 1 — 88x50,
// 2 — 60x34. Стоимость вершинного цикла ~ числу вершин.
static const struct { int c, r; } kGridLv[3] = { {120, 68}, {88, 50}, {60, 34} };

// Цвета вершин (общий буфер всех сеточных стилей): stride = cols + 1.
static int s_lod = 1;                // индекс в kGridLv
static float s_lod_ema_ms = 0.0f;    // сглаженный минимум времени обновления
static float s_lod_win_min = 0.0f;
static int s_lod_win_n = 0;
static unsigned s_lod_since = 0;     // кадров с последней смены LOD
static unsigned s_lod_wait = 900;    // сколько ждать перед попыткой подъёма

// Адаптивный LOD сеточных стилей. Метрика — МИНИМУМ времени обновления в окне
// из 30 кадров (вытеснение потоками TLS/декодера раздувает отдельные кадры, но
// не минимум), затем EMA. Бюджет ~9 мс на кадр (остальное — UI, звук, сеть).
// Вниз — если EMA > бюджета; вверх — не раньше чем через s_lod_wait кадров и
// только если оценка (пропорционально числу вершин, с запасом) укладывается;
// если после подъёма скоро пришлось опуститься, ожидание удваивается.
#define LOD_BUDGET_MS 9.0f
static void grid_set_level(int lod)
{
    if (lod < 0) lod = 0;
    if (lod > 2) lod = 2;
    s_lod = lod;
    g_glow.cols = kGridLv[lod].c;
    g_glow.rows = kGridLv[lod].r;
}

void glow_grid_lod_feed(float ms)
{
    if (s_lod_win_n == 0 || ms < s_lod_win_min) s_lod_win_min = ms;
    s_lod_since++;
    if (++s_lod_win_n < 30) return;
    s_lod_ema_ms = s_lod_ema_ms == 0.0f ? s_lod_win_min
                 : s_lod_ema_ms + (s_lod_win_min - s_lod_ema_ms) * 0.4f;
    s_lod_win_n = 0;
    if (s_lod < 2 && s_lod_since > 90 && s_lod_ema_ms > LOD_BUDGET_MS) {
        if (s_lod_since < 900) {  // подъём не удержался — ждать дольше
            s_lod_wait = s_lod_wait < 7200u ? s_lod_wait * 2u : 7200u;
        }
        grid_set_level(s_lod + 1);
        logLine("glow: lod=%d %dx%d (min frame %d.%d ms > %d)\n", s_lod, g_glow.cols, g_glow.rows,
                (int)s_lod_ema_ms, (int)(s_lod_ema_ms * 10.0f) % 10,
                (int)LOD_BUDGET_MS);
        s_lod_since = 0;
        s_lod_ema_ms = 0.0f;
    } else if (s_lod > 0 && s_lod_since > s_lod_wait) {
        float cells_now = (float)((g_glow.cols + 1) * (g_glow.rows + 1));
        float cells_up = (float)((kGridLv[s_lod - 1].c + 1) * (kGridLv[s_lod - 1].r + 1));
        if (s_lod_ema_ms * (cells_up / cells_now) * 1.15f < LOD_BUDGET_MS * 0.8f) {
            grid_set_level(s_lod - 1);
            logLine("glow: lod=%d %dx%d (est ok)\n", s_lod, g_glow.cols, g_glow.rows);
            s_lod_since = 0;
            s_lod_ema_ms = 0.0f;
        } else {
            s_lod_since = s_lod_wait / 2;  // перепроверить позже
        }
    }
}

void glow_grid_draw(float x, float y, float w, float h)
{
    const unsigned int *rgb = g_glow.rgb;
    int cols = g_glow.draw_cols, rows = g_glow.draw_rows;
    short xs[WG_MAXC + 1], ys[WG_MAXR + 1];
    GlowMeshVert *base, *p;
    int stride = cols + 1;
    int r, c;

    if (cols > WG_MAXC || rows > WG_MAXR) return;
    for (c = 0; c <= cols; c++) {
        xs[c] = (short)(x + w * (float)c / (float)cols + 0.5f);
    }
    for (r = 0; r <= rows; r++) {
        ys[r] = (short)(y + h * (float)r / (float)rows + 0.5f);
    }
    base = (GlowMeshVert *)sceGuGetMemory(
        (unsigned)(rows * stride * 2) * sizeof(GlowMeshVert));
    if (!base) return;

    hal_gpu_set_texturing(0);
    hal_gpu_set_blend(0, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    sceGuShadeModel(GU_SMOOTH);

    p = base;
    for (r = 0; r < rows; r++) {
        GlowMeshVert *strip = p;
        for (c = 0; c <= cols; c++) {
            p->color = rgb[r * stride + c];
            p->x = xs[c]; p->y = ys[r]; p->z = 0; p->pad = 0;
            p++;
            p->color = rgb[(r + 1) * stride + c];
            p->x = xs[c]; p->y = ys[r + 1]; p->z = 0; p->pad = 0;
            p++;
        }
        sceGuDrawArray(GU_TRIANGLE_STRIP,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2 * stride, 0, strip);
    }

    // Базовое состояние приложения: blend включён (см. glow_bg_draw).
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    hal_gpu_invalidate_tex_state();
}

