// Светящийся фон как в Яндекс Музыке (vibe-визуализация). См. ui_glow_bg.h.
//
// Три стиля (переключение в настройках, выбор в config/glow.cfg);
// WEB — по умолчанию:
//  CLASSIC — 4 мягких пятна (оригинал);
//  STRANDS — текучие нити (оригинал);
//  WEB — порт веб-вайба Яндекса (music.yandex.ru, v4.1657):
//    фон — 16-стоповый градиент бакета палитры averageColor обложки
//    (угол 270°, на паузе — чёрный, как PRIMARY_DARK_IDLE_STOPS),
//    поверх — 3 концентрических шумовых кольца из canvas-шейдера:
//    яркая сердцевина на радиусе layerNoise + угловой спарк,
//    цвета top/middle/bottom из HSL среднего цвета обложки.
//
// Производительность (на PSP нет FPU, float — софтовый):
//  веб-стиль: fixed-point 16.16 весь hot path; расчёт в half-res +
//  гладкий билинейный апскейл (точечная репликация давала бы мозаику);
//  обновление КАЖДЫЙ кадр целиком (шум/спарк/цвета/фон — всё вместе,
//  иначе ступени морфинга видны как рывки); угол спарка и UV-таблицы
//  предрасчитаны разово.
#include "ui/ui_glow_bg.h"
#include "ui/ui_draw.h"
#include "ui/ui_theme.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>

#include "hal/hal_gpu.h"
#include "services/audio_player.h"
#include "ui/ui_viz.h"
#include "core/fs.h"
#include "core/logger.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define GLOW_W 240
#define GLOW_H 136
#define GLOW_PITCH 256
#define GLOW_TEX_H 256  // степень двойки для GU
#define GLOW_BLOBS 4
#define GLOW_MASSES 2
#define GLOW_PALETTE 4  // 0 акцент, 1 вторичный, 2 средний, 3 база(фон)

#define FIELD_W 64
#define FIELD_H 36
#define SIN_N 1024

// Сетка веб-шума (билинейка по кэшу). Измерение на хосте (PSNR к эталону
// 240x136/128x72): кэш 32x18 — 38.7 дБ, 48x27 — 43.1, 64x36 — 44.8 дБ; именно
// он, а не частота вершин, ограничивал точность колец. Дорогое построение шума
// размазано равномерно на WG_NOISE_P кадров (по WG_NOISE_CELLS/P ячеек в кадр).
#define NF_W 64
#define NF_H 36
#define WG_NOISE_P 16

#define GLOW_STYLE_PATH "config/glow.cfg"

// Веб-стиль: константы canvas-шейдера Яндекса.
#define WEB_STOPS 16
#define WEB_CIRCLES 3
#define SPARK_N 256
#define WEB_EX 2.0f        // видимый диапазон effect-пространства по X
#define WEB_EY 1.13f       // ... по Y (изотропно: EY=EX*136/240)
#define WEB_NOISE_ZOOM 1.43f // шум в единицах десктоп-эффекта веба (2.86/2.0)

// Бакет палитры веба: диапазон hue + 16 стопов градиента.
typedef struct {
    float hue_start, hue_end;  // градусы; start>end = переход через 0
    unsigned char stops[WEB_STOPS][3];
} WebBucket;

static const WebBucket kWebBuckets[16] = {
    { 2.0f, 19.0f, {{0xF5,0x37,0x00},{0xCF,0x2F,0x00},{0xAF,0x27,0x00},{0x94,0x21,0x00},{0x7C,0x1C,0x00},{0x69,0x18,0x00},{0x59,0x14,0x00},{0x4B,0x11,0x00},{0x40,0x0E,0x00},{0x36,0x0C,0x00},{0x2D,0x0A,0x00},{0x25,0x08,0x00},{0x1D,0x07,0x00},{0x14,0x04,0x00},{0x0B,0x02,0x00},{0x00,0x00,0x00}} },  // coral
    { 20.0f, 64.0f, {{0xF5,0x6E,0x00},{0xCF,0x5D,0x00},{0xAF,0x4F,0x00},{0x94,0x42,0x00},{0x7C,0x38,0x00},{0x69,0x2F,0x00},{0x59,0x28,0x00},{0x4B,0x22,0x00},{0x40,0x1D,0x00},{0x36,0x18,0x00},{0x2D,0x14,0x00},{0x25,0x11,0x00},{0x1D,0x0D,0x00},{0x14,0x09,0x00},{0x0B,0x05,0x00},{0x00,0x00,0x00}} },  // tangerine
    { 65.0f, 149.0f, {{0x34,0xC0,0x3E},{0x2C,0xA3,0x34},{0x25,0x89,0x2C},{0x1F,0x74,0x25},{0x1A,0x62,0x1F},{0x16,0x52,0x1B},{0x13,0x45,0x16},{0x10,0x3B,0x13},{0x0D,0x32,0x10},{0x0B,0x2A,0x0E},{0x0A,0x23,0x0B},{0x08,0x1D,0x09},{0x06,0x17,0x07},{0x04,0x10,0x05},{0x02,0x09,0x03},{0x00,0x00,0x00}} },  // clover
    { 150.0f, 164.0f, {{0x00,0xC7,0x89},{0x00,0xA8,0x74},{0x00,0x8E,0x62},{0x00,0x78,0x53},{0x00,0x65,0x45},{0x00,0x55,0x3B},{0x00,0x48,0x32},{0x00,0x3D,0x2A},{0x00,0x34,0x24},{0x00,0x2C,0x1E},{0x00,0x25,0x19},{0x00,0x1E,0x15},{0x00,0x18,0x10},{0x00,0x10,0x0B},{0x00,0x09,0x06},{0x00,0x00,0x00}} },  // emerald
    { 165.0f, 179.0f, {{0x00,0xC7,0xA6},{0x00,0xA8,0x8C},{0x00,0x8E,0x77},{0x00,0x78,0x64},{0x00,0x65,0x54},{0x00,0x55,0x47},{0x00,0x48,0x3C},{0x00,0x3D,0x33},{0x00,0x34,0x2B},{0x00,0x2C,0x25},{0x00,0x25,0x1E},{0x00,0x1E,0x19},{0x00,0x18,0x14},{0x00,0x10,0x0E},{0x00,0x09,0x07},{0x00,0x00,0x00}} },  // turquoise
    { 180.0f, 189.0f, {{0x00,0xB2,0xCC},{0x00,0x96,0xAC},{0x00,0x7F,0x92},{0x00,0x6C,0x7B},{0x00,0x5A,0x67},{0x00,0x4C,0x57},{0x00,0x41,0x4A},{0x00,0x36,0x3E},{0x00,0x2E,0x35},{0x00,0x27,0x2D},{0x00,0x21,0x25},{0x00,0x1B,0x1F},{0x00,0x15,0x18},{0x00,0x0F,0x11},{0x00,0x08,0x09},{0x00,0x00,0x00}} },  // aquamarine
    { 190.0f, 204.0f, {{0x5C,0x8E,0x9B},{0x4E,0x78,0x83},{0x42,0x66,0x6F},{0x37,0x56,0x5D},{0x2F,0x48,0x4F},{0x27,0x3D,0x42},{0x21,0x33,0x38},{0x1C,0x2B,0x2F},{0x18,0x25,0x28},{0x14,0x1F,0x22},{0x11,0x1A,0x1C},{0x0E,0x15,0x17},{0x0B,0x11,0x12},{0x08,0x0C,0x0D},{0x04,0x06,0x07},{0x00,0x00,0x00}} },  // glacier
    { 205.0f, 219.0f, {{0x4F,0x6C,0x9B},{0x43,0x5B,0x83},{0x38,0x4D,0x6F},{0x30,0x41,0x5D},{0x28,0x37,0x4F},{0x22,0x2E,0x42},{0x1D,0x27,0x38},{0x18,0x21,0x2F},{0x14,0x1C,0x28},{0x11,0x18,0x22},{0x0F,0x14,0x1C},{0x0C,0x10,0x17},{0x09,0x0D,0x12},{0x07,0x09,0x0D},{0x04,0x05,0x07},{0x00,0x00,0x00}} },  // slate (fallback)
    { 220.0f, 234.0f, {{0x0C,0x41,0xE8},{0x0A,0x37,0xC4},{0x09,0x2E,0xA6},{0x07,0x27,0x8C},{0x06,0x21,0x76},{0x05,0x1C,0x63},{0x04,0x18,0x54},{0x04,0x14,0x47},{0x03,0x11,0x3C},{0x03,0x0E,0x33},{0x02,0x0C,0x2B},{0x02,0x0A,0x23},{0x01,0x08,0x1B},{0x01,0x05,0x13},{0x01,0x03,0x0A},{0x00,0x00,0x00}} },  // sapphire
    { 235.0f, 249.0f, {{0x16,0x0C,0xE8},{0x13,0x0A,0xC4},{0x10,0x09,0xA6},{0x0D,0x07,0x8C},{0x0B,0x06,0x76},{0x09,0x05,0x63},{0x08,0x04,0x54},{0x07,0x04,0x47},{0x06,0x03,0x3C},{0x05,0x03,0x33},{0x04,0x02,0x2B},{0x03,0x02,0x23},{0x03,0x01,0x1B},{0x02,0x01,0x13},{0x01,0x01,0x0A},{0x00,0x00,0x00}} },  // indigo
    { 250.0f, 259.0f, {{0x50,0x35,0xC0},{0x44,0x2D,0xA3},{0x39,0x26,0x89},{0x30,0x20,0x74},{0x29,0x1B,0x62},{0x22,0x17,0x52},{0x1D,0x13,0x45},{0x18,0x10,0x3B},{0x15,0x0E,0x32},{0x12,0x0C,0x2A},{0x0F,0x0A,0x23},{0x0C,0x08,0x1D},{0x09,0x06,0x17},{0x07,0x04,0x10},{0x04,0x02,0x09},{0x00,0x00,0x00}} },  // amethyst
    { 260.0f, 269.0f, {{0x7C,0x35,0xC0},{0x69,0x2D,0xA3},{0x59,0x26,0x89},{0x4B,0x20,0x74},{0x3F,0x1B,0x62},{0x35,0x17,0x52},{0x2D,0x13,0x45},{0x26,0x10,0x3B},{0x20,0x0E,0x32},{0x1B,0x0C,0x2A},{0x17,0x0A,0x23},{0x13,0x08,0x1D},{0x0F,0x06,0x17},{0x0A,0x04,0x10},{0x06,0x02,0x09},{0x00,0x00,0x00}} },  // plum
    { 270.0f, 299.0f, {{0xBB,0x1A,0xDB},{0x9E,0x16,0xB9},{0x86,0x13,0x9D},{0x71,0x10,0x84},{0x5F,0x0D,0x6F},{0x50,0x0B,0x5E},{0x44,0x09,0x4F},{0x39,0x08,0x43},{0x31,0x07,0x39},{0x29,0x06,0x30},{0x22,0x05,0x28},{0x1C,0x04,0x21},{0x16,0x03,0x1A},{0x10,0x02,0x12},{0x08,0x01,0x0A},{0x00,0x00,0x00}} },  // orchid
    { 300.0f, 329.0f, {{0xDB,0x1A,0x7D},{0xB9,0x16,0x6A},{0x9D,0x13,0x59},{0x84,0x10,0x4B},{0x6F,0x0D,0x40},{0x5E,0x0B,0x36},{0x4F,0x09,0x2D},{0x43,0x08,0x26},{0x39,0x07,0x20},{0x30,0x06,0x1B},{0x28,0x05,0x17},{0x21,0x04,0x13},{0x1A,0x03,0x0F},{0x12,0x02,0x0A},{0x0A,0x01,0x06},{0x00,0x00,0x00}} },  // raspberry
    { 330.0f, 339.0f, {{0xF5,0x00,0x7C},{0xCF,0x00,0x69},{0xAF,0x00,0x59},{0x94,0x00,0x4B},{0x7C,0x00,0x3F},{0x69,0x00,0x35},{0x59,0x00,0x2D},{0x4B,0x00,0x26},{0x40,0x00,0x20},{0x36,0x00,0x1B},{0x2D,0x00,0x17},{0x25,0x00,0x13},{0x1D,0x00,0x0F},{0x14,0x00,0x0A},{0x0B,0x00,0x06},{0x00,0x00,0x00}} },  // fuchsia
    { 340.0f, 1.0f, {{0xF5,0x00,0x2E},{0xCF,0x00,0x27},{0xAF,0x00,0x21},{0x94,0x00,0x1C},{0x7C,0x00,0x17},{0x69,0x00,0x14},{0x59,0x00,0x11},{0x4B,0x00,0x0E},{0x40,0x00,0x0C},{0x36,0x00,0x0A},{0x2D,0x00,0x08},{0x25,0x00,0x07},{0x1D,0x00,0x05},{0x14,0x00,0x04},{0x0B,0x00,0x02},{0x00,0x00,0x00}} },   // carmine (переход через 0)
};

// Бакеты по lightness (l в 0..100) для серых обложек: индекс в kWebBuckets.
static const struct { float start, end; int bucket; } kWebLightBuckets[5] = {
    { 0.0f, 22.0f, 10 },   // amethyst
    { 22.0f, 32.0f, 9 },   // indigo
    { 32.0f, 42.0f, 2 },   // clover
    { 42.0f, 51.0f, 13 },  // raspberry
    { 51.0f, 100.0f, 5 },  // aquamarine
};

// Позиции стопов градиента веба (%, угол 270°: 0% — правый край).
static const float kWebStopPos[WEB_STOPS] = {
    0.0f, 10.0f, 19.0f, 25.0f, 30.0f, 34.0f, 37.0f, 40.0f,
    42.0f, 44.0f, 47.0f, 50.0f, 54.0f, 60.0f, 67.0f, 75.0f
};

typedef struct {
    float cx, cy, r;
    unsigned char cr, cg, cb;
    float sx, sy, sr;   // скорость/период анимации
    float px, py, pr;   // фаза
} Blob;

typedef struct {
    float cx, cy, r;
    float sx, sy, sr;
    float px, py, pr;
} Mass;

// Fixed-point 16.16 для hot path веб-стиля (та же математика — те же пиксели).
typedef int32_t sfix;
static sfix fx_mul(sfix a, sfix b) { return (sfix)(((int64_t)a * (int64_t)b) >> 16); }
// Деление 16.16 через инструкцию div MIPS вместо медленного __divdi3
// (64-битное деление софтом, сотни циклов на вызов в hot path).
// Та же математика, точность та же (ошибка < 2 ulp — невидимо).
// Ограничения (все точки вызова им удовлетворяют):
//   |a| < 8M, b != 0. При b>>8 == 0 делитель заменяется на 1
//   (случай крошечного спарка в gate — результат всё равно clamится).
static inline sfix fx_div(sfix a, sfix b)
{
    sfix n = a << 8;
    sfix d = b >> 8;
    sfix q;
    if (d == 0) d = 1;
    // div без аппаратного интерлока: NOPы до чтения LO обязательны.
    __asm__ volatile ("div %1, %2\n\tnop\n\tnop\n\tmflo %0"
                      : "=r" (q) : "r" (n), "r" (d) : "hi", "lo");
    return q;
}
static int fx_floor(sfix x) { return x >= 0 ? (int)(x >> 16) : -((int)((65535 - x) >> 16)); }
static sfix fx_abs(sfix x) { return x < 0 ? -x : x; }
static int mod289(int x) { int r = x % 289; return r < 0 ? r + 289 : r; }
static int permute_i(int x) { int y = (x * 34 + 1) * x; return mod289(y); }

static unsigned char *s_buf = NULL;
static unsigned s_frame = 0;
static int s_playing = 1;
static int s_liked = 0;
static unsigned s_pulse_until = 0;
static unsigned s_last_pulse_frame = 0;
static float s_like_clock = -1.0f;   // секунды с момента лайка (<0 — нет)
static int s_style = GLOW_STYLE_DEFAULT;

// Палитра: до загрузки обложки — дефолтная «вайбовая», после — из обложки.
static unsigned char s_pal[GLOW_PALETTE][3] = {
    { 255,  40, 190},
    { 255, 210,  30},
    { 140,  30, 160},
    {  36,   6,  48},
};
static unsigned char s_avg[3] = { 140, 30, 160 };

static Blob s_blobs[GLOW_BLOBS] = {
    { 80.0f,  45.0f, 88.0f, 255,  30, 150, 0.340f, 0.272f, 0.208f, 0.0f, 1.1f, 0.5f },
    { 125.0f, 95.0f, 66.0f, 255, 210,  30, 0.240f, 0.304f, 0.176f, 2.0f, 0.3f, 1.7f },
    { 170.0f, 42.0f, 38.0f, 255, 140,  20, 0.384f, 0.208f, 0.272f, 4.0f, 2.2f, 0.9f },
    { 120.0f, 68.0f, 130.0f, 110,  18,  95, 0.128f, 0.176f, 0.112f, 3.0f, 5.0f, 2.5f },
};

static Mass s_masses[GLOW_MASSES] = {
    { 110.0f, 44.0f, 108.0f, 0.31f, 0.24f, 0.17f, 0.0f, 1.1f, 0.5f },
    { 120.0f, 128.0f, 130.0f, 0.16f, 0.10f, 0.11f, 2.0f, 0.3f, 1.7f },
};

static float s_field[FIELD_H][FIELD_W];
static float s_field2[FIELD_H][FIELD_W];
static float s_sin_tab[SIN_N + 1];
static int s_sin_ready = 0;

// Веб-стиль: состояние.
static int s_web_bucket = 7;  // slate
static float s_web_stops[WEB_STOPS][3];
static float s_web_stops_t[WEB_STOPS][3];
static float s_web_col[WEB_CIRCLES * 2][3];    // h(deg), s(0..1), l(0..1)
static float s_web_col_t[WEB_CIRCLES * 2][3];

static float s_vtime = 0.0f;
static unsigned long long s_last_us = 0;
// Сколько строк текстуры заполнено (классика/нити).
static int s_tex_rows = GLOW_H;
static unsigned s_web_tick = 0;
static int s_web_init = 0;

// Макс. сетка и уровни детализации (LOD). Уровень выбирается по замеру времени
// кадра (виртуальное время PPSSPP ≈ реальная PSP): 0 — 120x68, 1 — 88x50,
// 2 — 60x34. Стоимость вершинного цикла ~ числу вершин.
#define WG_MAXC 120
#define WG_MAXR 68
static const struct { int c, r; } kGridLv[3] = { {120, 68}, {88, 50}, {60, 34} };

// Цвета вершин (общий буфер всех сеточных стилей): stride = cols + 1.
static unsigned int s_wg_rgb[(WG_MAXR + 1) * (WG_MAXC + 1)];
static int s_gc = 88, s_gr = 50;     // текущая сетка (по уровню LOD)
static int s_lod = 1;                // индекс в kGridLv
static int s_draw_c = 88, s_draw_r = 50;   // сетка, которой заполнен буфер этого кадра
static float s_lod_ema_ms = 0.0f;    // сглаженный минимум времени обновления
static float s_lod_win_min = 0.0f;
static int s_lod_win_n = 0;
static unsigned s_lod_since = 0;     // кадров с последней смены LOD
static unsigned s_lod_wait = 900;    // сколько ждать перед попыткой подъёма
static void web_geom_init(void);

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
    s_gc = kGridLv[lod].c;
    s_gr = kGridLv[lod].r;
    web_geom_init();
}

static void grid_lod_feed(float ms)
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
        logLine("glow: lod=%d %dx%d (min frame %d.%d ms > %d)\n", s_lod, s_gc, s_gr,
                (int)s_lod_ema_ms, (int)(s_lod_ema_ms * 10.0f) % 10,
                (int)LOD_BUDGET_MS);
        s_lod_since = 0;
        s_lod_ema_ms = 0.0f;
    } else if (s_lod > 0 && s_lod_since > s_lod_wait) {
        float cells_now = (float)((s_gc + 1) * (s_gr + 1));
        float cells_up = (float)((kGridLv[s_lod - 1].c + 1) * (kGridLv[s_lod - 1].r + 1));
        if (s_lod_ema_ms * (cells_up / cells_now) * 1.15f < LOD_BUDGET_MS * 0.8f) {
            grid_set_level(s_lod - 1);
            logLine("glow: lod=%d %dx%d (est ok)\n", s_lod, s_gc, s_gr);
            s_lod_since = 0;
            s_lod_ema_ms = 0.0f;
        } else {
            s_lod_since = s_lod_wait / 2;  // перепроверить позже
        }
    }
}

typedef struct {
    unsigned short u, v;
    short x, y, z;
} GlowVert;

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

// Быстрый atan2 (-pi..pi), погрешность ~0.005 рад — для угла спарка хватает.
static float fast_atan2(float y, float x)
{
    float ax = x < 0.0f ? -x : x;
    float ay = y < 0.0f ? -y : y;
    float a, r;
    if (ay > ax) {
        a = ax / (ay + 1e-9f);
        r = 1.57079637f - a / (1.0f + 0.28f * a * a);
    } else {
        a = ay / (ax + 1e-9f);
        r = a / (1.0f + 0.28f * a * a);
    }
    if (x < 0.0f) r = 3.14159274f - r;
    if (y < 0.0f) r = -r;
    return r;
}

// Simplex-шум 3D в fixed (Ashima snoise, как в шейдере Яндекса).
// Вход 16.16 (эффект-координаты), выход 16.16 ~[-1,1].
static sfix snoise3_fx(sfix x, sfix y, sfix z)
{
    sfix s3 = fx_mul(x + y + z, 21845);
    int i = fx_floor(x + s3), j = fx_floor(y + s3), k = fx_floor(z + s3);
    sfix t = (sfix)(i + j + k) * 10923;
    sfix x0 = x - (((sfix)i << 16) - t);
    sfix y0 = y - (((sfix)j << 16) - t);
    sfix z0 = z - (((sfix)k << 16) - t);
    int gx = x0 >= y0 ? 1 : 0, gy = y0 >= z0 ? 1 : 0, gz = z0 >= x0 ? 1 : 0;
    int i1x = gx < 1 - gz ? gx : 1 - gz, i1y = gy < 1 - gx ? gy : 1 - gx;
    int i1z = gz < 1 - gy ? gz : 1 - gy;
    int i2x = gx > 1 - gz ? gx : 1 - gz, i2y = gy > 1 - gx ? gy : 1 - gx;
    int i2z = gz > 1 - gy ? gz : 1 - gy;
    sfix x1 = x0 - ((sfix)i1x << 16) + 10923;
    sfix y1 = y0 - ((sfix)i1y << 16) + 10923;
    sfix z1 = z0 - ((sfix)i1z << 16) + 10923;
    sfix x2 = x0 - ((sfix)i2x << 16) + 21845;
    sfix y2 = y0 - ((sfix)i2y << 16) + 21845;
    sfix z2 = z0 - ((sfix)i2z << 16) + 21845;
    sfix x3 = x0 - 65536 + 32768;
    sfix y3 = y0 - 65536 + 32768;
    sfix z3 = z0 - 65536 + 32768;
    int ix = mod289(i), iy = mod289(j), iz = mod289(k);
    int p0 = permute_i(permute_i(permute_i(iz) + iy) + ix);
    int p1 = permute_i(permute_i(permute_i(iz + i1z) + iy + i1y) + ix + i1x);
    int p2 = permute_i(permute_i(permute_i(iz + i2z) + iy + i2y) + ix + i2x);
    int p3 = permute_i(permute_i(permute_i(iz + 1) + iy + 1) + ix + 1);
    sfix r;
    {
        int ps[4];
        sfix xs[4], ys[4], zs[4];
        sfix acc = 0;
        int c;
        ps[0] = p0; ps[1] = p1; ps[2] = p2; ps[3] = p3;
        xs[0] = x0; xs[1] = x1; xs[2] = x2; xs[3] = x3;
        ys[0] = y0; ys[1] = y1; ys[2] = y2; ys[3] = y3;
        zs[0] = z0; zs[1] = z1; zs[2] = z2; zs[3] = z3;
        for (c = 0; c < 4; c++) {
            sfix pf = (sfix)ps[c] << 16;
            sfix tt = fx_mul(fx_mul(pf, 9362), 9362);
            sfix jx = pf - ((sfix)(49 * fx_floor(tt)) << 16);
            int xx = fx_floor(fx_mul(jx, 9362));
            int yy = fx_floor(jx - ((sfix)(7 * xx) << 16));
            sfix xc = (sfix)xx * 18725 - 60855;
            sfix yc = (sfix)yy * 18725 - 60855;
            sfix h = 65536 - fx_abs(xc) - fx_abs(yc);
            sfix s0x = (sfix)(fx_floor(xc) * 2 + 1);
            sfix s0y = (sfix)(fx_floor(yc) * 2 + 1);
            int sh = h > 0 ? 0 : -1;
            sfix ax = xc + s0x * sh;
            sfix ay = yc + s0y * sh;
            sfix d = fx_mul(ax, ax) + fx_mul(ay, ay) + fx_mul(h, h);
            sfix nn = 117496 - fx_mul(55950, d);
            sfix dot = fx_mul(fx_mul(ax, nn), xs[c]) +
                       fx_mul(fx_mul(ay, nn), ys[c]) +
                       fx_mul(fx_mul(h, nn), zs[c]);
            sfix m = 39322 - (fx_mul(xs[c], xs[c]) +
                              fx_mul(ys[c], ys[c]) + fx_mul(zs[c], zs[c]));
            if (m < 0) m = 0;
            m = fx_mul(m, m);
            acc += fx_mul(fx_mul(m, m), dot);
        }
        r = acc * 42;
    }
    return r;
}

// Треугольный шум спарка в fixed (triNoise3D шейдера, spd=0.1).
static sfix tri_fx(sfix x)
{
    sfix f = x & 0xFFFF;
    sfix d = f - 32768;
    return d < 0 ? -d : d;
}

static sfix trinoise3d_fx(sfix x, sfix y, sfix z, sfix vtime)
{
    sfix zz = 26214;  // 0.4
    sfix rz = 6554;   // 0.1
    sfix bx = x, by = y, bz = z;
    int o;
    for (o = 0; o <= 4; o++) {
        sfix txx = fx_mul(bx, 655), tyy = fx_mul(by, 655), tzz = fx_mul(bz, 655);
        sfix a0 = tzz + tri_fx(fx_mul(tyy, 1310720));
        sfix a1 = tzz + tri_fx(txx);
        sfix a2 = tyy + tri_fx(txx);
        sfix dx = tri_fx(a0), dy = tri_fx(a1), dz = tri_fx(a2);
        sfix vt = fx_mul(vtime, 655);  // vtime*0.01 (spd=0.1)
        sfix inner, tt, add;
        x += dx + vt; y += dy + vt; z += dz + vt;
        bx <<= 2; by <<= 2; bz <<= 2;
        zz = fx_mul(zz, 58982);  // *0.9
        x = fx_mul(x, 104858); y = fx_mul(y, 104858); z = fx_mul(z, 104858);  // *1.6
        inner = fx_mul(x, 39322) + fx_mul(6554, tri_fx(y));  // 0.6x+0.1*tri(y)
        tt = tri_fx(inner);
        add = tri_fx(z + tt);
        rz += fx_div(add, zz);
    }
    {
        float rzf = (float)rz / 65536.0f, zzf = (float)zz / 65536.0f;
        float v = rzf + sinf(rzf + sinf(zzf) * 2.8f) * 2.2f;  /* именно sinf: fsin() — табличный и не годится для больших аргументов */
        float tt = v / 8.0f;
        float s;
        if (tt < 0.0f) tt = 0.0f;
        if (tt > 1.0f) tt = 1.0f;
        s = tt * tt * (3.0f - 2.0f * tt);
        return (sfix)(s * 65536.0f);
    }
}

static sfix raw_spark_fx(sfix afx, sfix vtime)
{
    sfix angle = fx_mul(afx, 411775);  // *2pi
    sfix shifted = angle + 205887;     // +pi
    sfix shifted_index, seam_angle, seam_w, spark;
    if (shifted > 205887) shifted -= 411775;
    shifted_index = fx_mul(shifted, 10430);  // /2pi
    seam_angle = fx_mul(shifted + 205887, 32768);  // *0.5
    {
        float sf = sinf((float)seam_angle / 65536.0f);
        float tt = (sf - 0.9f) / 0.1f;
        if (tt < 0.0f) tt = 0.0f;
        if (tt > 1.0f) tt = 1.0f;
        seam_w = (sfix)(tt * tt * (3.0f - 2.0f * tt) * 65536.0f);
    }
    spark = trinoise3d_fx(afx, 0, 0, vtime);
    if (seam_w > 0) {
        sfix ss = trinoise3d_fx(shifted_index, 0, shifted_index, vtime);
        spark += fx_mul(ss - spark, seam_w);
    }
    return spark;
}

// RGB(0..255) -> HSL: h градусы 0..360, s/l проценты 0..100 (как в вебе).
static void rgb_to_hsl(float r, float g, float b, float *h, float *s, float *l)
{
    float mx, mn, d;
    r /= 255.0f; g /= 255.0f; b /= 255.0f;
    mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    *l = (mx + mn) * 50.0f;
    if (mx == mn) {
        *h = 0.0f; *s = 0.0f;
        return;
    }
    d = mx - mn;
    *s = *l > 50.0f ? d / (2.0f - mx - mn) * 100.0f : d / (mx + mn) * 100.0f;
    if (mx == r) {
        *h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    } else if (mx == g) {
        *h = (b - r) / d + 2.0f;
    } else {
        *h = (r - g) / d + 4.0f;
    }
    *h *= 60.0f;
}

// HSL (h градусы, s/l 0..1) -> RGB 0..255.
static void hsl_to_rgb(float h, float s, float l, float *r, float *g, float *b)
{
    float c, x, m, hh, q;
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    if (l < 0.0f) l = 0.0f;
    if (l > 1.0f) l = 1.0f;
    c = (1.0f - (l > 0.5f ? 2.0f * l - 1.0f : 1.0f - 2.0f * l)) * s;
    hh = h / 60.0f;
    while (hh < 0.0f) hh += 6.0f;
    while (hh >= 6.0f) hh -= 6.0f;
    q = hh - 2.0f * floorf(hh / 2.0f) - 1.0f;
    if (q < 0.0f) q = -q;
    x = c * (1.0f - q);
    if (hh < 1.0f) { *r = c; *g = x; *b = 0.0f; }
    else if (hh < 2.0f) { *r = x; *g = c; *b = 0.0f; }
    else if (hh < 3.0f) { *r = 0.0f; *g = c; *b = x; }
    else if (hh < 4.0f) { *r = 0.0f; *g = x; *b = c; }
    else if (hh < 5.0f) { *r = x; *g = 0.0f; *b = c; }
    else { *r = c; *g = 0.0f; *b = x; }
    m = l - c * 0.5f;
    *r = (*r + m) * 255.0f;
    *g = (*g + m) * 255.0f;
    *b = (*b + m) * 255.0f;
}

static void apply_palette_to_blobs(void)
{
    int i;
    for (i = 0; i < GLOW_BLOBS; i++) {
        const unsigned char *c = s_pal[i % GLOW_PALETTE];
        s_blobs[i].cr = c[0];
        s_blobs[i].cg = c[1];
        s_blobs[i].cb = c[2];
    }
}

void glow_bg_set_playing(int playing)
{
    s_playing = playing ? 1 : 0;
}

void glow_bg_set_liked(int liked)
{
    liked = liked ? 1 : 0;
    // Анимация лайка — только когда лайк поставили на ТОМ ЖЕ треке (как в
    // вебе: сравнивается id трека); смена трека/подгрузка статуса лайка
    // (pulse недавно) — без вспышки.
    if (liked && !s_liked && s_frame > 30 &&
        (s_frame - s_last_pulse_frame) > 120u) {
        s_like_clock = 0.0f;
    }
    s_liked = liked;
}

void glow_bg_pulse(void)
{
    s_pulse_until = s_frame + 45;  // ~0.75 сек при 60fps
    s_last_pulse_frame = s_frame;
}

int glow_bg_get_style(void)
{
    return s_style;
}

LocaleKey glow_bg_style_name_key(int style)
{
    switch (style) {
    case GLOW_STYLE_CLASSIC: return LOCALE_GLOW_STYLE_CLASSIC;
    case GLOW_STYLE_STRANDS: return LOCALE_GLOW_STYLE_STRANDS;
    case GLOW_STYLE_SPECTRUM: return LOCALE_GLOW_STYLE_SPECTRUM;
    case GLOW_STYLE_SCOPE: return LOCALE_GLOW_STYLE_SCOPE;
    case GLOW_STYLE_RADIAL: return LOCALE_GLOW_STYLE_RADIAL;
    default: return LOCALE_GLOW_STYLE_WEB;
    }
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
    SceUID fd;
    int length, written;

    if (style < 0 || style >= GLOW_STYLE_COUNT) return;
    if (s_style == style) return;
    s_style = style;
    length = snprintf(buffer, sizeof(buffer), "style=%d\n", style);
    if (length <= 0 || length >= (int)sizeof(buffer)) return;
    (void)fd; (void)written;
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

// Бакет веба по HSL среднего цвета (s/l в процентах).
static int web_bucket_from_hsl(float h, float s, float l)
{
    int i;
    (void)l;
    if (s > 0.0f) {
        for (i = 0; i < 16; i++) {
            float a = kWebBuckets[i].hue_start, b = kWebBuckets[i].hue_end;
            if (a > b) {
                if (h >= a || h <= b) return i;
            } else if (h >= a && h <= b) {
                return i;
            }
        }
        return 7;  // slate
    }
    for (i = 0; i < 5; i++) {
        if (i == 4) {
            if (l >= kWebLightBuckets[i].start && l <= kWebLightBuckets[i].end)
                return kWebLightBuckets[i].bucket;
        } else if (l >= kWebLightBuckets[i].start && l < kWebLightBuckets[i].end) {
            return kWebLightBuckets[i].bucket;
        }
    }
    return 7;
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
    s_pal[0][0] = acc1[0]; s_pal[0][1] = acc1[1]; s_pal[0][2] = acc1[2];
    s_pal[1][0] = acc2[0]; s_pal[1][1] = acc2[1]; s_pal[1][2] = acc2[2];
    s_pal[2][0] = (unsigned char)avg[0];
    s_pal[2][1] = (unsigned char)avg[1];
    s_pal[2][2] = (unsigned char)avg[2];
    s_avg[0] = (unsigned char)avg[0];
    s_avg[1] = (unsigned char)avg[1];
    s_avg[2] = (unsigned char)avg[2];
    {
        int i;
        for (i = 0; i < 3; i++) {
            int v = (int)acc1[i] * 16 / 100 + (int)avg[i] * 12 / 100;
            s_pal[3][i] = (unsigned char)(v > 255 ? 255 : v);
        }
    }

    boost_saturation(s_pal[0], 1.45f);
    boost_saturation(s_pal[1], 1.35f);
    boost_saturation(s_pal[2], 1.25f);
    lift_value(s_pal[0], 215);
    lift_value(s_pal[1], 205);
    lift_value(s_pal[2], 180);
    apply_palette_to_blobs();
}

#ifdef GLOW_REF_TEX
// Эталон для сравнения на хосте: старая попиксельная классика (текстура 240x136).
// Классика: 4 мягких пятна (оригинал, каждый кадр, полное разрешение).
static void classic_update_tex(void)
{
    float t = (float)s_frame * 0.016f * (s_playing ? 1.0f : 0.35f);
    float bx[GLOW_BLOBS], by[GLOW_BLOBS], br[GLOW_BLOBS];
    float inv_r2[GLOW_BLOBS];
    float bright = s_playing ? 1.0f : 0.55f;
    float pulse = 1.0f;
    float ar = (float)s_pal[3][0], ag = (float)s_pal[3][1], ab = (float)s_pal[3][2];
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
        float ny = ((float)y - GLOW_H * 0.5f) * (1.0f / (GLOW_H * 0.5f));
        for (x = 0; x < GLOW_W; x++) {
            float sw = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
            float nx = ((float)x - GLOW_W * 0.5f) * (1.0f / (GLOW_W * 0.5f));
            float cov, a, cr, cg, cb, vig;
            unsigned char *p = s_buf + (size_t)(y * GLOW_PITCH + x) * 4;

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

            sw += 0.12f;
            sr += ar * 0.12f;
            sg += ag * 0.12f;
            sb += ab * 0.12f;

            cov = (sw > 1.6f) ? 1.6f : sw;
            cr = sr / sw;
            cg = sg / sw;
            cb = sb / sw;

            if (s_liked) {
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

            p[0] = (unsigned char)cr;
            p[1] = (unsigned char)cg;
            p[2] = (unsigned char)cb;
            p[3] = (unsigned char)(a * 255.0f);
        }
    }
}

#endif

// Классика: 4 мягких пятна. Считается в вершинах сетки (тот же LOD, что у «Веба»),
// цвет с учётом прозрачности сразу смешан с фоном приложения (0x1A1A1A) — GPU
// только интерполирует (Gouraud) без blend. Пятна гладкие (радиус ≥ 38 px в
// пространстве 240x136), поэтому сетка 60x34..120x68 не отличима от попиксельной
// (сравнение — см. MEMORY_BANK_RU.md).
#define GLOW_BG_GRAY 26.0f
static float s_bg_dim = 1.0f;   // яркость фона (Winamp-стили — приглушённый)
static void classic_update(void)
{
    float t = (float)s_frame * 0.016f * (s_playing ? 1.0f : 0.35f);
    float bx[GLOW_BLOBS], by[GLOW_BLOBS], br2[GLOW_BLOBS];
    float inv_r2[GLOW_BLOBS];
    float bright = s_playing ? 1.0f : 0.55f;
    float pulse = 1.0f;
    float ar = (float)s_pal[3][0], ag = (float)s_pal[3][1], ab = (float)s_pal[3][2];
    int gc = s_gc, gr = s_gr, stride = s_gc + 1;
    float sx = (float)GLOW_W / (float)gc, sy = (float)GLOW_H / (float)gr;
    unsigned long long t_begin = sceKernelGetSystemTimeWide();
    int i, r, c;

    if (s_frame < s_pulse_until) {
        pulse = 1.0f + 0.45f * (float)(s_pulse_until - s_frame) / 45.0f;
    }
    bright *= pulse * s_bg_dim;

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

            if (s_liked) {
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
            s_wg_rgb[r * stride + c] = 0xFF000000u |
                ((unsigned int)(cb + 0.5f) << 16) |
                ((unsigned int)(cg + 0.5f) << 8) |
                (unsigned int)(cr + 0.5f);
        }
    }
    s_draw_c = gc;
    s_draw_r = gr;
    grid_lod_feed((float)(sceKernelGetSystemTimeWide() - t_begin) * 0.001f);
}

// Нити: поля для изолиний (оригинал, каждый кадр, полное разрешение).
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

// Нити остаются на попиксельной текстуре 240x136: рисунок — тонкие изолинии
// билинейных полей, и на сетке вершин (проверено: PSNR 26-29 дБ к текущему
// виду, линии превращаются в мозаику) он заметно ухудшается.
static void strands_update(void)
{
    float t = (float)s_frame * 0.016f * (s_playing ? 1.0f : 0.35f);
    float bright = s_playing ? 1.0f : 0.55f;
    float pulse = 1.0f;
    float mx[GLOW_MASSES], my[GLOW_MASSES], mr[GLOW_MASSES];
    float inv_r2[GLOW_MASSES];
    float top[3], bot[3], up[3], pool[3], halo_col[3], core_col[3];
    int i, x, y;

    if (s_frame < s_pulse_until) {
        pulse = 1.0f + 0.45f * (float)(s_pulse_until - s_frame) / 45.0f;
    }
    bright *= pulse;

    strands_build_fields(t);

    for (i = 0; i < 3; i++) {
        float b = (float)s_pal[3][i];
        float a1 = (float)s_pal[0][i];
        float av = (float)s_pal[2][i];
        top[i] = b * 0.60f + 4.0f;
        bot[i] = b * 1.70f + av * 0.25f + 10.0f;
        up[i] = a1 * 0.50f + (float)s_pal[1][i] * 0.16f;
        pool[i] = clampf0(av * 1.15f + 38.0f);
    }
    mix_white(s_pal[0], 0.30f, halo_col);
    mix_white(s_pal[0], 0.62f, core_col);

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
            unsigned char *p = s_buf + (size_t)(y * GLOW_PITCH + x) * 4;

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

            if (s_liked) {
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
}

// Веб-стиль: поля simplex-шума для трёх колец (fixed).
// Веб-стиль: цели цветов из среднего цвета обложки (customColors шейдера:
// top=(h,s+15,l+10), middle=(h,s+15,l+5), bottom=(h,s,l); на паузе — темно).
static void web_update_targets(int playing, float k)
{
    float h, s, l;
    float th, ts, tl, mh, ms, ml, bh, bs, bl;
    int i, j;

    rgb_to_hsl((float)s_avg[0], (float)s_avg[1], (float)s_avg[2], &h, &s, &l);
    if (playing) {
        th = h; ts = s + 15.0f; tl = l + 10.0f;
        mh = h; ms = s + 15.0f; ml = l + 5.0f;
        bh = h; bs = s;         bl = l;
    } else {
        th = h; ts = s * 0.5f; tl = l * 0.25f;
        mh = h; ms = s * 0.5f; ml = l * 0.25f;
        bh = h; bs = s * 0.5f; bl = l * 0.25f;
    }

    s_web_col_t[0][0] = bh; s_web_col_t[0][1] = bs / 100.0f; s_web_col_t[0][2] = bl / 100.0f;
    s_web_col_t[1][0] = mh; s_web_col_t[1][1] = ms / 100.0f; s_web_col_t[1][2] = ml / 100.0f;
    s_web_col_t[2][0] = th; s_web_col_t[2][1] = ts / 100.0f; s_web_col_t[2][2] = tl / 100.0f;
    s_web_col_t[3][0] = bh; s_web_col_t[3][1] = bs / 100.0f; s_web_col_t[3][2] = bl / 100.0f;
    s_web_col_t[4][0] = mh; s_web_col_t[4][1] = ms / 100.0f; s_web_col_t[4][2] = ml / 100.0f;
    s_web_col_t[5][0] = th; s_web_col_t[5][1] = ts / 100.0f; s_web_col_t[5][2] = tl / 100.0f;

    s_web_bucket = web_bucket_from_hsl(h, s, l);
    for (i = 0; i < WEB_STOPS; i++) {
        for (j = 0; j < 3; j++) {
            float target = playing ? (float)kWebBuckets[s_web_bucket].stops[i][j] : 0.0f;
            s_web_stops_t[i][j] = target;
            s_web_stops[i][j] += (target - s_web_stops[i][j]) * k;
        }
    }
    for (i = 0; i < WEB_CIRCLES * 2; i++) {
        for (j = 0; j < 3; j++) {
            s_web_col[i][j] += (s_web_col_t[i][j] - s_web_col[i][j]) * k;
        }
    }
}

static void web_init_state(void)
{
    int i, j;
    static const float init_h[6] = { 290.0f, 300.0f, 50.0f, 325.0f, 320.0f, 50.0f };
    for (i = 0; i < 6; i++) {
        s_web_col[i][0] = init_h[i];
        s_web_col[i][1] = 1.0f;
        s_web_col[i][2] = 0.5f;
        s_web_col_t[i][0] = init_h[i];
        s_web_col_t[i][1] = 1.0f;
        s_web_col_t[i][2] = 0.5f;
    }
    for (i = 0; i < WEB_STOPS; i++) {
        for (j = 0; j < 3; j++) {
            s_web_stops[i][j] = (float)kWebBuckets[7].stops[i][j];
            s_web_stops_t[i][j] = (float)kWebBuckets[7].stops[i][j];
        }
    }
    s_web_init = 1;
}

/* ------------------------------------------------------------------------
 * WEB (порт веб-вайба Яндекса) на сетке вершин GPU.
 * Та же математика, что в шейдере/старом попиксельном пути (фон-градиент
 * бакета, 3 шумовых кольца со спарком, виньетка, пауза/лайк/пульс), но
 * считается в вершинах сетки 87x49 (шаг ~5.5 px; шум — билинейка по сетке
 * 32x18), а GPU интерполирует цвет (Gouraud). Вместо ~16 тыс. пикселей —
 * ~4,3 тыс. вершин;
 * без текстуры, без VFPU-асма и без записи/сброса кэша: на железе нет
 * проблем когерентности и порчи VFPU-контекста.
 * ------------------------------------------------------------------------ */
static float s_wg_ux[WG_MAXC + 1], s_wg_nx2[WG_MAXC + 1], s_wg_nx[WG_MAXC + 1];
static float s_wg_uy[WG_MAXR + 1], s_wg_ny2[WG_MAXR + 1], s_wg_ny[WG_MAXR + 1];
static float s_wg_blen[WG_MAXR + 1][WG_MAXC + 1];
static float s_wg_ft[WG_MAXR + 1][WG_MAXC + 1];       // доля LUT спарка
static unsigned char s_wg_i0[WG_MAXR + 1][WG_MAXC + 1];
static unsigned char s_wg_ci0[WG_MAXC + 1], s_wg_rj0[WG_MAXR + 1];
static float s_wg_ctx[WG_MAXC + 1], s_wg_rty[WG_MAXR + 1];
// Сжатые (лайк: uv*(1-like*0.5)) индексы шума по кольцам.
static unsigned char s_wg_sci0[3][WG_MAXC + 1], s_wg_srj0[3][WG_MAXR + 1];
static float s_wg_sctx[3][WG_MAXC + 1], s_wg_srty[3][WG_MAXR + 1];
static float s_wg_bg[WG_MAXC + 1][3];                 // фон по колонкам 0..255
// Шумовой кэш: тройной буфер [buf][поле][NF_H][NF_W]; поле f — шум кольца f
// (f=0 ещё и «общий»). gridL — линейная смесь prev→cur текущего кадра.
static float s_wg_grid[3][3][NF_H][NF_W];
static float s_wg_gridL[3][NF_H][NF_W];
static int s_wg_prev = 0, s_wg_cur = 1, s_wg_build = 2;
static int s_wg_ready = 0;
static sfix s_wg_build_vt = 0;
static float s_wg_lut[SPARK_N];        // спарк-LUT (запечён shapeSpark)
static float s_wg_lut_next[SPARK_N];   // строится по третям за 3 кадра
static sfix s_wg_lut_vt = 0;

static float wg_len(float x, float y)  // быстрая длина (+0.41*min), как в вебе
{
    float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y;
    return ax > ay ? ax + 0.41f * ay : ay + 0.41f * ax;
}

static float wg_sat(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Индекс/доля билинейки шумовой сетки для нормализованной координаты n∈[-1,1].
static void wg_noise_idx(float n, int cells, int *i0, float *t)
{
    float f = (n * 0.5f + 0.5f) * (float)(cells - 1);
    int i;
    if (f < 0.0f) f = 0.0f;
    if (f > (float)(cells - 1)) f = (float)(cells - 1);
    i = (int)f;
    if (i > cells - 2) i = cells - 2;
    *i0 = i;
    *t = f - (float)i;
}

// Статическая геометрия вершин: один раз при инициализации.
static void web_geom_init(void)
{
    int r, c;
    for (c = 0; c <= s_gc; c++) {
        float nx = ((float)c * (1.0f / (float)s_gc)) * 2.0f - 1.0f;
        int i0;
        float tx;
        wg_noise_idx(nx, NF_W, &i0, &tx);
        s_wg_nx[c] = nx;
        s_wg_ux[c] = nx * WEB_EX;
        s_wg_nx2[c] = nx * nx;
        s_wg_ci0[c] = (unsigned char)i0;
        s_wg_ctx[c] = tx;
    }
    for (r = 0; r <= s_gr; r++) {
        float ny = ((float)r * (1.0f / (float)s_gr)) * 2.0f - 1.0f;
        int j0;
        float ty;
        wg_noise_idx(ny, NF_H, &j0, &ty);
        s_wg_ny[r] = ny;
        s_wg_uy[r] = ny * WEB_EY;
        s_wg_ny2[r] = ny * ny;
        s_wg_rj0[r] = (unsigned char)j0;
        s_wg_rty[r] = ty;
    }
    for (r = 0; r <= s_gr; r++) {
        for (c = 0; c <= s_gc; c++) {
            float ang = fast_atan2(s_wg_uy[r], s_wg_ux[c]) * 0.15915494f + 0.5f;
            float fi;
            int i0;
            if (ang < 0.0f) ang = 0.0f;
            if (ang > 1.0f) ang = 1.0f;
            fi = ang * (float)(SPARK_N - 1);
            i0 = (int)fi;
            if (i0 > SPARK_N - 2) i0 = SPARK_N - 2;
            s_wg_i0[r][c] = (unsigned char)i0;
            s_wg_ft[r][c] = fi - (float)i0;
            s_wg_blen[r][c] = wg_len(s_wg_ux[c], s_wg_uy[r]);
        }
    }
}

// Ячейки [from, to) шумового кэша буфера buf (индекс = f*NF_H*NF_W + j*NF_W + i;
// та же математика, что в вебе). Размазывание по кадрам — см. web_update.
#define WG_NOISE_CELLS (3 * NF_H * NF_W)
static void web_build_noise_slice(int buf, int from, int to, sfix vt)
{
    sfix kx = (sfix)(WEB_EX * 1.2f * WEB_NOISE_ZOOM * 65536.0f);
    sfix ky = (sfix)(WEB_EY * 1.2f * WEB_NOISE_ZOOM * 65536.0f);
    int idx;
    for (idx = from; idx < to; idx++) {
        int f = idx / (NF_H * NF_W);
        int rem = idx - f * (NF_H * NF_W);
        int j = rem / NF_W, i = rem - j * NF_W;
        sfix off = f == 0 ? 0 : (f == 1 ? 102944 : 205887);
        sfix vf = ((sfix)j << 16) / (NF_H - 1);
        sfix uf = ((sfix)i << 16) / (NF_W - 1);
        sfix evy = fx_mul(vf + vf - 65536, ky);
        sfix eux = fx_mul(uf + uf - 65536, kx);
        sfix rr = snoise3_fx(eux + off, evy + off, vt + off);
        s_wg_grid[buf][f][j][i] = ((float)rr * (1.0f / 65536.0f) + 1.0f) * 0.5f;
    }
}

// Спарк-LUT: диапазон [from, to). Без деления на 3 кадра — нет пика.
static void web_build_lut_range(int from, int to, sfix vtf)
{
    int i;
    for (i = from; i < to; i++) {
        sfix a = ((sfix)i * 65536) / (SPARK_N - 1) - 32768;
        sfix v = raw_spark_fx(a, vtf);
        sfix s2, sp, gi, gate;
        if (v < 0) v = 0;
        if (v > 65536) v = 65536;
        s2 = fx_mul(v, v);
        sp = fx_mul(v, 13107) + fx_mul(fx_mul(fx_mul(s2, s2), s2), s2);
        gi = fx_div(19661, sp + 1);
        if (gi > 65536) gi = 65536;
        gate = fx_mul(fx_mul(gi, gi), 196608 - 2 * gi);
        s_wg_lut_next[i] = (float)fx_mul(sp, gate) * (1.0f / 65536.0f);
    }
}

// ---- Реакция на лайк и на звук (как в вебе) ----------------------------
// Лайк (web: likeAnimation): react кольца 0/1/2 = 0.7 в моменты 0 / 0.10 /
// 0.15 с и 0 в 0.85 / 0.95 / 1.05 с; значение сглаживается как Tt(600 мс):
// cur += (target-cur)*clamp(elapsed/0.6). Звук: уровни трёх полос
// (низ/середина/верх) — по кольцам 0/1/2; растёт мгновенно, падает на 0.02
// за шаг 40 мс; общий множитель audioRatio плавно 0..1 за 1 с (играет/пауза).
#define WG_STEP 0.04f
static float s_react[3], s_react_el[3], s_react_tgt[3];
static float s_aud_raw[3], s_aud[3];
static float s_aud_ratio = 0.0f, s_aud_ratio_el = 0.0f, s_aud_ratio_tgt = 0.0f;
static float s_step_acc = 0.0f;

static float wg_level_norm(int v, float db0, float range)
{
    float x = (float)v * (1.0f / 32768.0f);
    float db = 20.0f * log10f(x + 1e-6f);
    return wg_sat((db - db0) / range);
}

static void web_react_step(float dt)
{
    static const float kS[3] = { 0.0f, 0.10f, 0.15f };
    static const float kE[3] = { 0.85f, 0.95f, 1.05f };
    int i;
    if (s_like_clock >= 0.0f) {
        s_like_clock += dt;
        if (s_like_clock > 1.5f) s_like_clock = -1.0f;
    }
    s_step_acc += dt;
    if (s_step_acc > 0.25f) s_step_acc = 0.25f;
    while (s_step_acc >= WG_STEP) {
        int lo = 0, mi = 0, hi = 0;
        float tl[3];
        s_step_acc -= WG_STEP;
        for (i = 0; i < 3; i++) {
            float tgt = (s_like_clock >= kS[i] && s_like_clock < kE[i]) ? 0.7f : 0.0f;
            float e = s_react_el[i] / 0.6f;
            if (tgt != s_react_tgt[i]) {
                s_react_tgt[i] = tgt;
                s_react_el[i] = 0.0f;
                e = 0.0f;
            }
            if (e > 1.0f) e = 1.0f;
            s_react_el[i] += WG_STEP;
            s_react[i] += (tgt - s_react[i]) * e;
            if (s_react[i] < 0.0005f && tgt == 0.0f) s_react[i] = 0.0f;
        }
        if (s_playing) {
            audio_player_get_levels(&lo, &mi, &hi);
        }
        tl[0] = wg_level_norm(lo, -28.0f, 24.0f);
        tl[1] = wg_level_norm(mi, -40.0f, 26.0f);
        tl[2] = wg_level_norm(hi, -48.0f, 30.0f);
        {
            float tgt = s_playing ? 1.0f : 0.0f;
            float e = s_aud_ratio_el / 1.0f;
            if (tgt != s_aud_ratio_tgt) {
                s_aud_ratio_tgt = tgt;
                s_aud_ratio_el = 0.0f;
                e = 0.0f;
            }
            if (e > 1.0f) e = 1.0f;
            s_aud_ratio_el += WG_STEP;
            s_aud_ratio += (tgt - s_aud_ratio) * e;
        }
        for (i = 0; i < 3; i++) {
            float prev = s_aud_raw[i];
            if (tl[i] > prev) {
                prev = tl[i];
            } else {
                float d = prev - tl[i];
                prev -= d < 0.02f ? d : 0.02f;
            }
            s_aud_raw[i] = prev;
            s_aud[i] = prev * s_aud_ratio;
        }
    }
}

static float wg_sstep(float x)  // smoothstep по уже нормированному t
{
    float t2 = wg_sat(x);
    return t2 * t2 * (3.0f - 2.0f * t2);
}

// Веб-стиль: фон + кольца в вершинах сетки.
static void web_update(float dt)
{
    static const float kRot[3][3] = {
        { -0.3f, 0.3f, 0.2f }, { -0.3f, -0.3f, -0.2f }, { -0.3f, -0.3f, 0.2f }
    };
    float bright = s_playing ? 1.0f : 0.55f;
    float pulse = 1.0f;
    float offx[3], offy[3], inv_edge[3], inten[3], brad0[3], bwidH[3], bmul[3];
    float c1[3][3], c2[3][3];
    float scl[3], rea[3], lk[3];
    int lk_on[3];
    int phase, i, r, c;
    int gc = s_gc, gr = s_gr, ostride = s_gc + 1;
    unsigned long long t_begin = sceKernelGetSystemTimeWide();

    if (!s_web_init) {
        web_init_state();
    }
    if (s_frame < s_pulse_until) {
        pulse = 1.0f + 0.45f * (float)(s_pulse_until - s_frame) / 45.0f;
    }
    bright *= pulse;

    s_vtime += dt * (s_playing ? 0.2f : 0.06f);
    if (s_vtime > 86400.0f) s_vtime -= 86400.0f;
    web_update_targets(s_playing, dt > 1.0f ? 1.0f : dt);
    web_react_step(dt);

    // --- Шум и спарк: тяжёлая часть, размазана по кадрам ---
    // Шум (кэш 64x36 x3 поля) достраивается равными долями за WG_NOISE_P кадров
    // в буфер build; на экране — смесь prev→cur (k растёт за период). Время
    // шума идёт медленно (vtime 0.2/с), поэтому период 16 кадров незаметен.
    // Спарк-LUT меняется быстрее — по третям за 3 кадра, как раньше.
    phase = (int)(s_web_tick % 3);
    {
        int np = (int)(s_web_tick % WG_NOISE_P);
        int per = (WG_NOISE_CELLS + WG_NOISE_P - 1) / WG_NOISE_P;
        int from = np * per, to = from + per;
        float k = (float)(np + 1) * (1.0f / (float)WG_NOISE_P);
        const float *pv, *cu;
        float *dl;
        int n;

        if (!s_wg_ready) {
            // Первый кадр: весь кэш сразу (prev=cur — один снимок), LUT целиком.
            sfix vt0 = (sfix)(s_vtime * 32768.0f);
            unsigned long long t0 = sceKernelGetSystemTimeWide();
            web_build_noise_slice(s_wg_cur, 0, WG_NOISE_CELLS, vt0);
            memcpy(s_wg_grid[s_wg_prev], s_wg_grid[s_wg_cur],
                   sizeof(s_wg_grid[0]));
            s_wg_build_vt = vt0;
            s_wg_lut_vt = (sfix)(s_vtime * 65536.0f);
            web_build_lut_range(0, SPARK_N, s_wg_lut_vt);
            memcpy(s_wg_lut, s_wg_lut_next, sizeof(s_wg_lut));
            s_wg_ready = 1;
            logLine("glow: web init noise %dx%d took %u ms\n", NF_W, NF_H,
                    (unsigned)((sceKernelGetSystemTimeWide() - t0) / 1000));
        }
        if (np == 0) s_wg_build_vt = (sfix)(s_vtime * 32768.0f);
        if (to > WG_NOISE_CELLS) to = WG_NOISE_CELLS;
        if (from < to) web_build_noise_slice(s_wg_build, from, to, s_wg_build_vt);

        if (phase == 0) s_wg_lut_vt = (sfix)(s_vtime * 65536.0f);
        {
            int lf = phase * ((SPARK_N + 2) / 3);
            int lt = lf + (SPARK_N + 2) / 3;
            if (lt > SPARK_N) lt = SPARK_N;
            web_build_lut_range(lf, lt, s_wg_lut_vt);
            if (phase == 2) {
                memcpy(s_wg_lut, s_wg_lut_next, sizeof(s_wg_lut));
            }
        }

        pv = &s_wg_grid[s_wg_prev][0][0][0];
        cu = &s_wg_grid[s_wg_cur][0][0][0];
        dl = &s_wg_gridL[0][0][0];
        for (n = 0; n < WG_NOISE_CELLS; n++) {
            float a = pv[n];
            dl[n] = a + (cu[n] - a) * k;
        }
        if (np == WG_NOISE_P - 1) {
            int old_prev = s_wg_prev;
            s_wg_prev = s_wg_cur;
            s_wg_cur = s_wg_build;
            s_wg_build = old_prev;
        }
    }
    s_web_tick++;

    // --- Параметры колец и цвета кадра ---
    for (i = 0; i < WEB_CIRCLES; i++) {
        float ra = s_vtime * kRot[i][2];
        float cs = cosf(ra), sn = sinf(ra);
        float edge = 1.1f + sinf(s_vtime + 1.57f * (float)i);
        float au = s_aud[i];
        float strength;
        lk[i] = s_react[i];
        lk_on[i] = lk[i] > 0.002f;
        scl[i] = 1.0f - 0.5f * lk[i];                 // uv*(1-like*0.5)
        rea[i] = lk[i] > au * 0.6f ? lk[i] : au * 0.6f;
        strength = lk[i] > au ? lk[i] : au;
        offx[i] = kRot[i][0] * cs - kRot[i][1] * sn;
        offy[i] = kRot[i][0] * sn + kRot[i][1] * cs;
        inv_edge[i] = 1.0f / edge;
        inten[i] = 0.15f * (1.0f - 0.75f * sinf(s_vtime * 2.0f + 0.785f * (float)i)) +
                   0.3f * strength;
        brad0[i] = 0.95f - 0.15f * (float)i;
        bwidH[i] = (0.8f - 0.2f * (float)i) * 0.5f;
        bmul[i] = 1.0f - (float)i * 0.3f;
        hsl_to_rgb(s_web_col[i][0], s_web_col[i][1], s_web_col[i][2],
                   &c1[i][0], &c1[i][1], &c1[i][2]);
        hsl_to_rgb(s_web_col[i + 3][0], s_web_col[i + 3][1], s_web_col[i + 3][2],
                   &c2[i][0], &c2[i][1], &c2[i][2]);
        if (lk_on[i]) {
            for (c = 0; c <= gc; c++) {
                int i0; float tx;
                wg_noise_idx(s_wg_nx[c] * scl[i], NF_W, &i0, &tx);
                s_wg_sci0[i][c] = (unsigned char)i0;
                s_wg_sctx[i][c] = tx;
            }
            for (r = 0; r <= gr; r++) {
                int j0; float ty;
                wg_noise_idx(s_wg_ny[r] * scl[i], NF_H, &j0, &ty);
                s_wg_srj0[i][r] = (unsigned char)j0;
                s_wg_srty[i][r] = ty;
            }
        }
    }
    // Фон: градиент бакета по колонкам (угол 270°: свет справа).
    for (c = 0; c <= gc; c++) {
        float pos = (1.0f - (float)c * (1.0f / (float)gc)) * 100.0f;
        int s = 0;
        float f;
        while (s < WEB_STOPS - 2 && kWebStopPos[s + 1] < pos) s++;
        f = wg_sat((pos - kWebStopPos[s]) /
                   (kWebStopPos[s + 1] - kWebStopPos[s] + 1e-6f));
        for (i = 0; i < 3; i++) {
            s_wg_bg[c][i] = (s_web_stops[s][i] +
                (s_web_stops[s + 1][i] - s_web_stops[s][i]) * f) * 0.62f;
        }
    }

    // --- Вершины ---
    for (r = 0; r <= gr; r++) {
        int j0 = s_wg_rj0[r];
        float ty = s_wg_rty[r];
        float uvy = s_wg_uy[r];
        for (c = 0; c <= gc; c++) {
            int i0 = s_wg_ci0[c];
            float tx = s_wg_ctx[c];
            float uvx = s_wg_ux[c];
            float blen = s_wg_blen[r][c];
            float w00 = (1.0f - tx) * (1.0f - ty), w10 = tx * (1.0f - ty);
            float w01 = (1.0f - tx) * ty, w11 = tx * ty;
            float ch[3];
            float spark, vig, cr, cg, cb, bgr, bgg, bgb;
            int i1;

            for (i1 = 0; i1 < 3; i1++) {
                ch[i1] = s_wg_gridL[i1][j0][i0] * w00 +
                         s_wg_gridL[i1][j0][i0 + 1] * w10 +
                         s_wg_gridL[i1][j0 + 1][i0] * w01 +
                         s_wg_gridL[i1][j0 + 1][i0 + 1] * w11;
            }
            {
                int k0 = s_wg_i0[r][c];
                spark = s_wg_lut[k0] +
                        (s_wg_lut[k0 + 1] - s_wg_lut[k0]) * s_wg_ft[r][c];
            }
            bgr = s_wg_bg[c][0]; bgg = s_wg_bg[c][1]; bgb = s_wg_bg[c][2];
            cr = bgr; cg = bgg; cb = bgb;

            for (i = 0; i < WEB_CIRCLES; i++) {
                // Радиус: blob = base - step*n + 0.3*sharedNoise (shared ∈ [-1,1]);
                // при лайке/звуке спарк раздувает внешний радиус (лучи).
                float brad = brad0[i] + 0.3f * (2.0f * ch[0] - 1.0f);
                float br = bmul[i] * spark;
                float outer = brad + bwidH[i] + br * (1.0f + rea[i] * 50.0f * br);
                float dx, dy, llen, lay, dn, alpha, om, t2, glow, tt, d;
                float rr, gg, bb;
                if (blen >= outer) continue;
                dx = uvx * scl[i] + offx[i];
                dy = uvy * scl[i] + offy[i];
                llen = wg_len(dx, dy);
                if (lk_on[i]) {
                    int sj = s_wg_srj0[i][r], si = s_wg_sci0[i][c];
                    float sx = s_wg_sctx[i][c], sy = s_wg_srty[i][r];
                    lay = s_wg_gridL[i][sj][si] * ((1.0f - sx) * (1.0f - sy)) +
                          s_wg_gridL[i][sj][si + 1] * (sx * (1.0f - sy)) +
                          s_wg_gridL[i][sj + 1][si] * ((1.0f - sx) * sy) +
                          s_wg_gridL[i][sj + 1][si + 1] * (sx * sy);
                } else {
                    lay = ch[i];
                }
                dn = llen >= lay ? llen - lay : lay - llen;
                alpha = 1.0f - wg_sstep((llen - lay) * inv_edge[i]);
                d = outer - 0.5f;
                if (d < 0.05f) d = 0.05f;
                t2 = wg_sat((blen - 0.5f) / d);
                om = 1.0f - t2 * t2 * (3.0f - 2.0f * t2);
                alpha *= om;
                if (alpha <= 0.0f) continue;
                glow = inten[i] / (1.0f + 11.0f * dn) * 255.0f;
                tt = wg_sat(dy * 2.0f);
                rr = c1[i][0] + (c2[i][0] - c1[i][0]) * tt + glow;
                gg = c1[i][1] + (c2[i][1] - c1[i][1]) * tt + glow;
                bb = c1[i][2] + (c2[i][2] - c1[i][2]) * tt + glow;
                if (rr > 255.0f) rr = 255.0f;
                if (gg > 255.0f) gg = 255.0f;
                if (bb > 255.0f) bb = 255.0f;
                if (lk_on[i]) {
                    // Белая вспышка в центре кольца при лайке (+0.6*like).
                    float fl = 153.0f * lk[i] *
                               (1.0f - wg_sstep((blen - 0.2f) /
                                                (outer * 0.8f - 0.2f + 1e-4f)));
                    rr += fl; gg += fl; bb += fl;
                }
                if (rr < 0.0f) rr = 0.0f; else if (rr > 255.0f) rr = 255.0f;
                if (gg < 0.0f) gg = 0.0f; else if (gg > 255.0f) gg = 255.0f;
                if (bb < 0.0f) bb = 0.0f; else if (bb > 255.0f) bb = 255.0f;
                cr += (rr - cr) * alpha;
                cg += (gg - cg) * alpha;
                cb += (bb - cb) * alpha;
            }

            // На паузе кольца гаснут к фону; виньетка.
            cr = bgr + (cr - bgr) * bright;
            cg = bgg + (cg - bgg) * bright;
            cb = bgb + (cb - bgb) * bright;
            vig = 1.0f - 0.3f * (s_wg_nx2[c] * 0.7f + s_wg_ny2[r] * 0.5f);
            cr *= vig; cg *= vig; cb *= vig;
            if (cr < 0.0f) cr = 0.0f; else if (cr > 255.0f) cr = 255.0f;
            if (cg < 0.0f) cg = 0.0f; else if (cg > 255.0f) cg = 255.0f;
            if (cb < 0.0f) cb = 0.0f; else if (cb > 255.0f) cb = 255.0f;
            s_wg_rgb[r * ostride + c] = 0xFF000000u | ((unsigned int)(cb + 0.5f) << 16) |
                             ((unsigned int)(cg + 0.5f) << 8) |
                             (unsigned int)(cr + 0.5f);
        }
    }

    s_draw_c = gc;
    s_draw_r = gr;
    grid_lod_feed((float)(sceKernelGetSystemTimeWide() - t_begin) * 0.001f);
}

typedef struct {
    unsigned int color;
    short x, y, z, pad;   /* 12 байт: выравнивание вершины GU по цвету (4) */
} MeshVert;
typedef MeshVert VizScrimVert;

// Рисует сетку цветов вершин (rows+1 строк по cols+1 цветов ABGR) полосами
// треугольников с Gouraud-интерполяцией. Путь стиля WEB.
static void grid_draw(const unsigned int *rgb, int cols, int rows,
                      float x, float y, float w, float h)
{
    short xs[WG_MAXC + 1], ys[WG_MAXR + 1];
    MeshVert *base, *p;
    int stride = cols + 1;
    int r, c;

    if (cols > WG_MAXC || rows > WG_MAXR) return;
    for (c = 0; c <= cols; c++) {
        xs[c] = (short)(x + w * (float)c / (float)cols + 0.5f);
    }
    for (r = 0; r <= rows; r++) {
        ys[r] = (short)(y + h * (float)r / (float)rows + 0.5f);
    }
    base = (MeshVert *)sceGuGetMemory(
        (unsigned)(rows * stride * 2) * sizeof(MeshVert));
    if (!base) return;

    hal_gpu_set_texturing(0);
    hal_gpu_set_blend(0, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    sceGuShadeModel(GU_SMOOTH);

    p = base;
    for (r = 0; r < rows; r++) {
        MeshVert *strip = p;
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

/* ---- Подложка под текст -------------------------------------------------
 * Свечение бывает светлым и серым, а текст интерфейса — белый/серый/тёмно-серый:
 * при совпадении яркости он сливается. Меряем яркость (85-й процентиль, раз в
 * 6 кадров) под областью текста в уже посчитанном буфере вершин (или в текстуре
 * «Нитей») и кладём чёрную плашку с мягким краем; альфа подобрана так, чтобы
 * фон под текстом оставался темнее ~46/255 (серый CCCCCC читается, контраст
 * около 5:1), и плавно следует за яркостью — без мерцания. */
static float s_scrim_a = 0.16f;

static float glow_region_luma(float x0, float y0, float x1, float y1)
{
    unsigned hist[16];
    unsigned n = 0, acc = 0, want;
    int b;
    memset(hist, 0, sizeof(hist));
    if (s_style == GLOW_STYLE_STRANDS) {
        int ix0 = (int)(x0 * 0.5f), ix1 = (int)(x1 * 0.5f);
        int iy0 = (int)(y0 * 0.5f), iy1 = (int)(y1 * 0.5f);
        int ix, iy;
        if (!s_buf) return 0.0f;
        if (ix1 > GLOW_W - 1) ix1 = GLOW_W - 1;
        if (iy1 > GLOW_H - 1) iy1 = GLOW_H - 1;
        for (iy = iy0; iy <= iy1; iy += 6) {
            for (ix = ix0; ix <= ix1; ix += 6) {
                const unsigned char *p = s_buf + (size_t)(iy * GLOW_PITCH + ix) * 4;
                unsigned l = (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
                hist[l >> 4]++;
                n++;
            }
        }
    } else {
        int gc = s_draw_c, gr = s_draw_r;
        int c0 = (int)(x0 * (float)gc / 480.0f), c1 = (int)(x1 * (float)gc / 480.0f + 0.999f);
        int r0 = (int)(y0 * (float)gr / 272.0f), r1 = (int)(y1 * (float)gr / 272.0f + 0.999f);
        int c, r;
        if (c0 < 0) c0 = 0;
        if (r0 < 0) r0 = 0;
        if (c1 > gc) c1 = gc;
        if (r1 > gr) r1 = gr;
        for (r = r0; r <= r1; r += 2) {
            for (c = c0; c <= c1; c += 2) {
                unsigned px = s_wg_rgb[r * (gc + 1) + c];
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

void glow_bg_text_scrim(float x, float y, float w, float h)
{
    static unsigned cnt = 0;
    static float luma = 0.0f;
    const float fx = 26.0f, fy = 16.0f;
    float xs[3], ys[4], want, a;
    VizScrimVert *v, *p;
    int r, c;

    if (!hal_gpu_in_frame()) return;
    hal_gpu_set_texturing(0);
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    sceGuShadeModel(GU_SMOOTH);
    if ((cnt++ % 6u) == 0u) luma = glow_region_luma(x, y, x + w, y + h);
    want = luma > 50.0f ? 1.0f - 50.0f / luma : 0.0f;
    if (want < 0.16f) want = 0.16f;       // лёгкое затемнение всегда: тени текста мало
    if (want > 0.68f) want = 0.68f;
    /* Winamp-стили: яркие столбики/линии рисуются поверх сетки и в замер яркости
     * не попадают — под текстом держим плотную плашку всегда. */
    if (s_style >= GLOW_STYLE_SPECTRUM && want < 0.62f) want = 0.62f;
    s_scrim_a += (want - s_scrim_a) * 0.08f;
    a = s_scrim_a * 255.0f * ((float)ui_draw_get_alpha() * (1.0f / 255.0f));

    xs[0] = x; xs[1] = x + fx; xs[2] = x + w;
    ys[0] = y; ys[1] = y + fy; ys[2] = y + h - fy; ys[3] = y + h;
    v = (VizScrimVert *)sceGuGetMemory((unsigned)(3 * 6) * sizeof(VizScrimVert));
    if (!v) return;
    p = v;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            int k0 = (c == 0) ? 0 : 1;
            int ka = (r == 0) ? 0 : 1, kb = (r + 1 == 3) ? 0 : 1;
            p->color = ((unsigned int)(a * (float)(k0 * ka) + 0.5f)) << 24;
            p->x = (short)(xs[c] + 0.5f); p->y = (short)(ys[r] + 0.5f); p->z = 0; p->pad = 0;
            p++;
            p->color = ((unsigned int)(a * (float)(k0 * kb) + 0.5f)) << 24;
            p->x = (short)(xs[c] + 0.5f); p->y = (short)(ys[r + 1] + 0.5f); p->z = 0; p->pad = 0;
            p++;
        }
        sceGuDrawArray(GU_TRIANGLE_STRIP,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       6, 0, p - 6);
    }
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    hal_gpu_invalidate_tex_state();
}

// Обновление текстуры: каждый стиль — каждый кадр (плавность без рывков).
static void glow_update(void)
{
    unsigned long long now;
    float dt;

    if (!s_sin_ready) {
        sin_tab_init();
    }

    now = sceKernelGetSystemTimeWide();
    dt = s_last_us ? (float)(now - s_last_us) / 1000000.0f : 0.033f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    s_last_us = now;

    if (s_style == GLOW_STYLE_WEB) {
        web_update(dt);
        return;  // то же: сетка вершин, без текстуры
    }
    if (s_style == GLOW_STYLE_CLASSIC) {
        s_bg_dim = 1.0f;
        classic_update();   // сетка вершин, как «Веб»: текстуру не трогаем
        return;
    }
    if (s_style >= GLOW_STYLE_SPECTRUM) {
        s_bg_dim = 0.42f;   // фон приглушён: визуализация должна читаться
        classic_update();
        viz_update(s_playing);
        return;
    }
    strands_update();

    // Padding для LINEAR-фильтра: реплика последнего столбца/строки (240x136).
    {
        int y;
        for (y = 0; y < GLOW_H; y++) {
            memcpy(s_buf + (size_t)(y * GLOW_PITCH + GLOW_W) * 4,
                   s_buf + (size_t)(y * GLOW_PITCH + GLOW_W - 1) * 4,
                   (size_t)(GLOW_PITCH - GLOW_W) * 4);
        }
        memcpy(s_buf + (size_t)(GLOW_H * GLOW_PITCH) * 4,
               s_buf + (size_t)((GLOW_H - 1) * GLOW_PITCH) * 4,
               (size_t)GLOW_PITCH * 4);
        s_tex_rows = GLOW_H;
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
    glow_config_load();
    if (!s_sin_ready) {
        sin_tab_init();
    }
    grid_set_level(s_lod);  // статическая геометрия вершин сетки: разово
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

    if (s_style == GLOW_STYLE_WEB || s_style == GLOW_STYLE_CLASSIC ||
        s_style >= GLOW_STYLE_SPECTRUM) {
        grid_draw(s_wg_rgb, s_draw_c, s_draw_r,
                 x, y, w, h);
        if (s_style >= GLOW_STYLE_SPECTRUM) {
            viz_draw(s_style == GLOW_STYLE_SPECTRUM ? VIZ_SPECTRUM :
                     (s_style == GLOW_STYLE_SCOPE ? VIZ_SCOPE : VIZ_RADIAL),
                     x, y, w, h, s_pal[0][0], s_pal[0][1], s_pal[0][2]);
        }
        return;
    }

    // Только заполненные строки (+1 строка паддинга), а не все 256 КБ.
    sceKernelDcacheWritebackRange(s_buf, (size_t)GLOW_PITCH * (s_tex_rows + 1) * 4);
    hal_gpu_set_texturing(1);
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    // Сброс кэша дескрипторов GE: без него GE может держать текстуру шрифта
    // от прошлых отрисовок и misread-ить s_buf полосами (как в ui_draw.c).
    sceGuTexFlush();
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
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
        v[1].v = (unsigned short)s_tex_rows;
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
