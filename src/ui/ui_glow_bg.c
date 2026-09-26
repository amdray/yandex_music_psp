// Светящийся фон как в Яндекс Музыке (vibe-визуализация). См. ui_glow_bg.h.
//
// Три стиля (переключение в настройках, выбор в config/glow.cfg):
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

// Сетка веб-шума (грубее — билинейка сгладит; семплируется попиксельно).
#define NF_W 32
#define NF_H 18

#define GLOW_STYLE_PATH "config/glow.cfg"

// Веб-стиль: константы canvas-шейдера Яндекса.
#define WEB_STOPS 16
#define WEB_CIRCLES 3
#define SPARK_N 256
#define WEB_EX 2.0f        // видимый диапазон effect-пространства по X
#define WEB_EY 1.13f       // ... по Y (изотропно: EY=EX*136/240)
#define WEB_NOISE_ZOOM 1.43f // шум в единицах десктоп-эффекта веба (2.86/2.0)
#define WEB_RW 120          // фон-градиент: достаточно 120 колонок
// Веб-стиль рендерится в половинном разрешении (120x68): содержимое —
// мягкие градиенты, GPU с LINEAR-фильтром растягивает его бесплатно и гладко
// (так делают bloom во всех играх). В 4 раза меньше попиксельной математики.
#define WEB_W2 120
#define WEB_H2 68
// LUT вместо двух попиксельных делений (div ~40 тактов, 6 шт/пиксель):
// GLOWFALL: 1/(1+11*dn) при инициализации; RECIP: 1/(outer-0.5), та же математика.
#define GLOWFALL_N 256
#define RECIP_N 57

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
static sfix fx_clamp01(sfix x) { return x < 0 ? 0 : (x > 65536 ? 65536 : x); }
static sfix fx_abs(sfix x) { return x < 0 ? -x : x; }
static int mod289(int x) { int r = x % 289; return r < 0 ? r + 289 : r; }
static int permute_i(int x) { int y = (x * 34 + 1) * x; return mod289(y); }

static unsigned char *s_buf = NULL;
static unsigned s_frame = 0;
static int s_playing = 1;
static int s_liked = 0;
static unsigned s_pulse_until = 0;
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
static sfix s_wbg[WEB_RW][3];                  // фон-градиент, ед. цвета*256
static sfix s_nfield[3][NF_H][NF_W];
static sfix s_spark[SPARK_N];
static unsigned char s_spark_x[GLOW_H][GLOW_W];
static sfix s_uu[GLOW_W];
static sfix s_vv[GLOW_H];
static sfix s_uu2[WEB_W2];                    // половинная сетка веб-стиля
static sfix s_vv2[WEB_H2];
static unsigned char s_spark_x2[WEB_H2][WEB_W2];
static sfix s_glowfall[GLOWFALL_N];           // 1/(1+11*dn), init-once
static sfix s_recip[RECIP_N];                 // 1/(0.25+q/32), init-once

// VFPU micro-probe: 1 = прогнать раз при старте и вывести биты в лог.
// Проверяет недоказанные инструкции перед написанием ядра.
#define VFPU_PROBE 0
#if VFPU_PROBE
static void vfpu_probe_run(void)
{
    static float pin[4] __attribute__((aligned(16)));
    static float pin2[4] __attribute__((aligned(16)));
    static float pout[64] __attribute__((aligned(16)));
    unsigned *b;
    int i;
    pin[0] = 2.0f; pin[1] = 4.0f; pin[2] = 6.0f; pin[3] = 8.0f;
    pin2[0] = 1.0f; pin2[1] = 2.0f; pin2[2] = 2.0f; pin2[3] = 4.0f;
    __asm__ volatile (
        "lv.q R000, 0(%[pin])\n\t"
        "lv.q R001, 0(%[pin2])\n\t"
        "vdiv.q R002, R000, R001\n\t"
        "sv.q R002, 16(%[pout])\n\t"
        : : [pin]"r"(pin), [pin2]"r"(pin2), [pout]"r"(pout) : "memory");
    b = (unsigned *)pout;
    for (i = 0; i < 2; i++) {
        logLine("vfdivord %d: %08x %08x %08x %08x\n",
                i, b[i * 4], b[i * 4 + 1], b[i * 4 + 2], b[i * 4 + 3]);
    }
}
#endif

// VFPU-ядро веб-свечения: полное разрешение 240x136, float32 (точнее 16.16).
// Стадии: 0 = выкл (C-путь половинного разрешения — сохранённый быстрый вариант),
// 1 = фон + круги (const lay/spark), 2 = +шумовые поля, 3 = полное (все стадии).
#define VFPU_STAGE 1
// Временный дамп значений ядра в лог (диагностика пересвета). 0 = выкл.
#define VFPU_DUMP 0

#if VFPU_STAGE
#include <stddef.h>

// Определены ниже по файлу — форвардные объявления для каркаса ядра.
static float s_vtime;
static unsigned s_web_tick;
static void hsl_to_rgb(float h, float s, float l, float *r, float *g, float *b);
static sfix snoise3_fx(sfix x, sfix y, sfix z);
static float fast_atan2(float y, float x);
static sfix raw_spark_fx(sfix afx, sfix vtime);

// Блоб указателей для ядра. Смещения фиксированы (проверяются в init).
typedef struct {
    const float *g0, *g1, *g2;       // +0,+4,+8 (стадия 2+)
    const float *sparkf;             // +12 (стадия 3)
    const float *bgq;                // +16: фон 240 квадов {r,g,b,0} 0..1
    const float *colTq;              // +20: цвета [3][3][2] {c1,c2} транспонированные
    const float *prmTq;              // +24: параметры [3][8] {invedge,inten,offx,offy,brad,bwidH,bmul,recip}
    const float *txq;                // +28 (стадия 2+)
    const float *tyq;                // +32 (стадия 2+)
    const float *uvxq;               // +36: 60 квадов uvx
    const float *nx2q;               // +40: 60 квадов nx^2
    const float *uvyq;               // +44: 136 квадов {uvy,0,0,0}
    const float *oneq;               // +48: {1,1,1,1}
    float *tmp;                      // +52: выходные [60][3] {R,G,B} 0..1
    const short *x0q;                // +56 (стадия 2+)
    const float *k41q;               // +60: {0.41 x4}
    const float *khalfq;             // +64: {0.5 x4}
    const float *k7q;                // +68: {0.7 x4}
    const float *k3q;                // +72: {0.3 x4}
    const float *k11q;               // +76: {11.0 x4}
    const float *vigtq;              // +80: 136 квадов {ny2*0.5 x4}
} VfpuBlob;
static VfpuBlob s_vblob;

static float s_fbgq[240][4] __attribute__((aligned(16)));
static float s_fbgT[60][3][4] __attribute__((aligned(16)));
static float s_fcolT[3][3][2][4] __attribute__((aligned(16)));
static float s_fprmT[3][8][4] __attribute__((aligned(16)));
static float s_fuvxq[60][4] __attribute__((aligned(16)));
static float s_fnx2q[60][4] __attribute__((aligned(16)));
static float s_fuvyq[136][4] __attribute__((aligned(16)));
static float s_fvigt[136][4] __attribute__((aligned(16)));
static float s_fone[4] __attribute__((aligned(16)));
static float s_fk41[4] __attribute__((aligned(16)));
static float s_fkhalf[4] __attribute__((aligned(16)));
static float s_fk7[4] __attribute__((aligned(16)));
static float s_fk3[4] __attribute__((aligned(16)));
static float s_fk11[4] __attribute__((aligned(16)));
static float s_ftmp[60][3][4] __attribute__((aligned(16)));
// Шумовые поля (стадия 2): двойной буфер + по-кадровый lerped-слой.
// Убирает 8.7Гц-ступенчатость морфа (тёмные прыгающие bands на полном разрешении).
static float s_fgridA[3][19][33] __attribute__((aligned(16)));
static float s_fgridB[3][19][33] __attribute__((aligned(16)));
static float s_fgridL[3][19][33] __attribute__((aligned(16)));
static int s_gridParity = 0;
static int s_gridAge = 0;
static float s_ftxq[60][4] __attribute__((aligned(16)));
static float s_fty2[136][4] __attribute__((aligned(16)));
static short s_fx0q[60] __attribute__((aligned(16)));
static short s_fy0r[136] __attribute__((aligned(16)));
// Спарк стадии 3: LUT шейдера (float) + построчные буферы (spark, recip/circle).
static float s_fsparkLUT[SPARK_N] __attribute__((aligned(16)));
static float s_sparkQR[60][4] __attribute__((aligned(16)));
static float s_recipQQR[3][60][4] __attribute__((aligned(16)));

// Статическая геометрия + проводка блоба. Разово при инициализации.
static void vfpu_init_tables(void)
{
    int x, y, i;
    // Индексы и веса билинейки полей: статическая геометрия.
    for (x = 0; x < 60; x++) {
        int q;
        float fxc = ((float)(4 * x + 1) + 0.5f) * (1.0f / (GLOW_W - 1)) * (float)(NF_W - 1);
        int x0 = (int)fxc;
        if (x0 < 0) x0 = 0;
        if (x0 > NF_W - 2) x0 = NF_W - 2;
        s_fx0q[x] = (short)x0;
        for (q = 0; q < 4; q++) {
            float fx = ((float)(4 * x + q)) * (1.0f / (GLOW_W - 1)) * (float)(NF_W - 1);
            float tx = fx - (float)x0;
            if (tx < 0.0f) tx = 0.0f;
            if (tx > 1.0f) tx = 1.0f;
            s_ftxq[x][q] = tx;
        }
    }
    for (y = 0; y < 136; y++) {
        float fy = (float)y * (1.0f / (GLOW_H - 1)) * (float)(NF_H - 1);
        float ty = fy - (float)((int)fy);
        int y0raw = (int)fy;
        int y0;
        int q;
        if (y0raw > NF_H - 2) { y0 = NF_H - 2; ty = 1.0f; }
        else { y0 = y0raw < 0 ? 0 : y0raw; }
        if (ty < 0.0f) ty = 0.0f;
        if (ty > 1.0f) ty = 1.0f;
        s_fy0r[y] = (short)y0;
        for (q = 0; q < 4; q++) s_fty2[y][q] = ty;
    }
    for (x = 0; x < 60; x++) {
        float uu = ((float)(4 * x) + 1.5f) * (1.0f / (GLOW_W - 1)) * 0.5f;
        float nx = uu * 2.0f - 1.0f;
        float uvx = nx * WEB_EX;
        int q;
        for (q = 0; q < 4; q++) {
            float uup = ((float)(4 * x + q)) * (1.0f / (GLOW_W - 1));
            float nxp = uup * 2.0f - 1.0f;
            s_fuvxq[x][q] = nxp * WEB_EX;
            s_fnx2q[x][q] = nxp * nxp;
        }
        (void)uvx; (void)nx;
    }
    for (y = 0; y < 136; y++) {
        float vv = (float)y * (1.0f / (GLOW_H - 1));
        float ny = vv * 2.0f - 1.0f;
        float uvy = ny * WEB_EY;
        s_fuvyq[y][0] = uvy; s_fuvyq[y][1] = uvy;
        s_fuvyq[y][2] = uvy; s_fuvyq[y][3] = uvy;
        s_fvigt[y][0] = s_fvigt[y][1] = s_fvigt[y][2] = s_fvigt[y][3]
            = ny * ny * 0.5f;
    }
    s_fone[0] = s_fone[1] = s_fone[2] = s_fone[3] = 1.0f;
    s_fk41[0] = s_fk41[1] = s_fk41[2] = s_fk41[3] = 0.41f;
    s_fkhalf[0] = s_fkhalf[1] = s_fkhalf[2] = s_fkhalf[3] = 0.5f;
    s_fk7[0] = s_fk7[1] = s_fk7[2] = s_fk7[3] = 0.7f;
    s_fk3[0] = s_fk3[1] = s_fk3[2] = s_fk3[3] = 0.3f;
    s_fk11[0] = s_fk11[1] = s_fk11[2] = s_fk11[3] = 11.0f;
    // Стадия 1: brad статичен (const lay) — базовые радиусы и половины ширин.
    // recip = 1/(outer-0.5) считается на CPU (как RECIP в C-пути): ядру не нужен
    // ни vrcp, ни чтение singles — только проверенные quad-операции.
    for (i = 0; i < 3; i++) {
        float brad = 0.95f - 0.15f * (float)i;
        float bwidH = (0.8f - 0.2f * (float)i) * 0.5f;
        float bmul = 1.0f - (float)i * 0.3f;
        float recip = 1.0f / ((brad + bwidH) - 0.5f);
        int q;
        for (q = 0; q < 4; q++) {
            s_fprmT[i][4][q] = brad;
            s_fprmT[i][5][q] = bwidH;
            s_fprmT[i][6][q] = bmul;
            s_fprmT[i][7][q] = recip;
        }
    }
    s_vblob.g0 = &s_fgridL[0][0][0];
    s_vblob.g1 = &s_fgridL[1][0][0];
    s_vblob.g2 = &s_fgridL[2][0][0];
    s_vblob.sparkf = NULL;
    s_vblob.bgq = &s_fbgT[0][0][0];
    s_vblob.colTq = &s_fcolT[0][0][0][0];
    s_vblob.prmTq = &s_fprmT[0][0][0];
    s_vblob.txq = &s_ftxq[0][0];
    s_vblob.tyq = &s_fty2[0][0];
    s_vblob.uvxq = &s_fuvxq[0][0];
    s_vblob.nx2q = &s_fnx2q[0][0];
    s_vblob.uvyq = &s_fuvyq[0][0];
    s_vblob.oneq = s_fone;
    s_vblob.tmp = &s_ftmp[0][0][0];
    s_vblob.x0q = s_fx0q;
    s_vblob.k41q = s_fk41;
    s_vblob.khalfq = s_fkhalf;
    s_vblob.k7q = s_fk7;
    s_vblob.k3q = s_fk3;
    s_vblob.k11q = s_fk11;
    s_vblob.vigtq = &s_fvigt[0][0];
    // Смещения фиксированы — ядро опирается на них через lw.
    if (offsetof(VfpuBlob, bgq) != 16 || offsetof(VfpuBlob, colTq) != 20 ||
        offsetof(VfpuBlob, prmTq) != 24 || offsetof(VfpuBlob, uvxq) != 36 ||
        offsetof(VfpuBlob, nx2q) != 40 || offsetof(VfpuBlob, uvyq) != 44 ||
        offsetof(VfpuBlob, oneq) != 48 || offsetof(VfpuBlob, tmp) != 52 ||
        offsetof(VfpuBlob, k41q) != 60 || offsetof(VfpuBlob, khalfq) != 64 ||
        offsetof(VfpuBlob, k7q) != 68 || offsetof(VfpuBlob, k3q) != 72 ||
        offsetof(VfpuBlob, k11q) != 76 || offsetof(VfpuBlob, vigtq) != 80 ||
        sizeof(VfpuBlob) != 84) {
        logLine("vfpu: blob layout mismatch, fallback to C\n");
    }
    // Кэш-гигиена CPU->VFPU для статической геометрии (разово): иначе на
    // железе VFPU читает stale-RAM (в эмуляторе память единая — незаметно).
    sceKernelDcacheWritebackRange(s_fuvxq, sizeof(s_fuvxq));
    sceKernelDcacheWritebackRange(s_fnx2q, sizeof(s_fnx2q));
    sceKernelDcacheWritebackRange(s_fuvyq, sizeof(s_fuvyq));
    sceKernelDcacheWritebackRange(s_fvigt, sizeof(s_fvigt));
    sceKernelDcacheWritebackRange(s_fone, sizeof(s_fone));
    sceKernelDcacheWritebackRange(s_fk41, sizeof(s_fk41));
    sceKernelDcacheWritebackRange(s_fkhalf, sizeof(s_fkhalf));
    sceKernelDcacheWritebackRange(s_fk7, sizeof(s_fk7));
    sceKernelDcacheWritebackRange(s_fk3, sizeof(s_fk3));
    sceKernelDcacheWritebackRange(s_fk11, sizeof(s_fk11));
    sceKernelDcacheWritebackRange(s_ftxq, sizeof(s_ftxq));
    sceKernelDcacheWritebackRange(s_fty2, sizeof(s_fty2));
}

// Кадровые таблицы из морфирующих цветов/параметров (float, 0..1).
static void vfpu_build_frame(float bright)
{
    static const float kRot[3][3] = {
        { -0.3f, 0.3f, 0.2f }, { -0.3f, -0.3f, -0.2f }, { -0.3f, -0.3f, 0.2f }
    };
    int i, x, q;
    (void)bright;
    for (x = 0; x < GLOW_W; x++) {
        float pos = (1.0f - (float)x / (float)(GLOW_W - 1)) * 100.0f;
        int s = 0;
        float f;
        while (s < WEB_STOPS - 2 && kWebStopPos[s + 1] < pos) s++;
        f = (pos - kWebStopPos[s]) /
            (kWebStopPos[s + 1] - kWebStopPos[s] + 1e-6f);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        for (i = 0; i < 3; i++) {
            s_fbgq[x][i] = (s_web_stops[s][i] +
                (s_web_stops[s + 1][i] - s_web_stops[s][i]) * f) * (0.62f / 255.0f);
        }
        s_fbgq[x][3] = 0.0f;
    }
    // Транспонированный фон для ядра: [quad][канал][4 пикселя].
    for (x = 0; x < 60; x++) {
        for (q = 0; q < 4; q++) {
            s_fbgT[x][0][q] = s_fbgq[x * 4 + q][0];
            s_fbgT[x][1][q] = s_fbgq[x * 4 + q][1];
            s_fbgT[x][2][q] = s_fbgq[x * 4 + q][2];
        }
    }
    for (i = 0; i < WEB_CIRCLES; i++) {
        float ra = s_vtime * kRot[i][2];
        float cs = cosf(ra), sn = sinf(ra);
        float offx = kRot[i][0] * cs - kRot[i][1] * sn;
        float offy = kRot[i][0] * sn + kRot[i][1] * cs;
        float edge = 1.1f + sinf(s_vtime + 1.57f * (float)i);
        float inten = 0.15f * (1.0f - 0.75f * sinf(s_vtime * 2.0f + 0.785f * (float)i));
        float r1, g1, b1, r2, g2, b2;
        hsl_to_rgb(s_web_col[i][0], s_web_col[i][1], s_web_col[i][2], &r1, &g1, &b1);
        hsl_to_rgb(s_web_col[i + 3][0], s_web_col[i + 3][1], s_web_col[i + 3][2], &r2, &g2, &b2);
        // hsl_to_rgb отдаёт 0..255 — ядру нужно 0..1.
        r1 *= 0.0039215686f; g1 *= 0.0039215686f; b1 *= 0.0039215686f;
        r2 *= 0.0039215686f; g2 *= 0.0039215686f; b2 *= 0.0039215686f;
        for (q = 0; q < 4; q++) {
            s_fprmT[i][0][q] = 1.0f / edge;
            s_fprmT[i][1][q] = inten;
            s_fprmT[i][2][q] = offx;
            s_fprmT[i][3][q] = offy;
        }
        s_fcolT[i][0][0][0] = s_fcolT[i][0][0][1] = s_fcolT[i][0][0][2] = s_fcolT[i][0][0][3] = r1;
        s_fcolT[i][0][1][0] = s_fcolT[i][0][1][1] = s_fcolT[i][0][1][2] = s_fcolT[i][0][1][3] = r2;
        s_fcolT[i][1][0][0] = s_fcolT[i][1][0][1] = s_fcolT[i][1][0][2] = s_fcolT[i][1][0][3] = g1;
        s_fcolT[i][1][1][0] = s_fcolT[i][1][1][1] = s_fcolT[i][1][1][2] = s_fcolT[i][1][1][3] = g2;
        s_fcolT[i][2][0][0] = s_fcolT[i][2][0][1] = s_fcolT[i][2][0][2] = s_fcolT[i][2][0][3] = b1;
        s_fcolT[i][2][1][0] = s_fcolT[i][2][1][1] = s_fcolT[i][2][1][2] = s_fcolT[i][2][1][3] = b2;
    }
}

// Шумовые сетки float с паддингом (стадия 2). Та же математика, что web_build_fields.
// Пишет в parity-буфер (double-buffer для по-кадровой интерполяции).
static void vfpu_build_grids(sfix vt)
{
    float (*dst)[19][33] = s_gridParity ? s_fgridB : s_fgridA;
    float kx = WEB_EX * 1.2f * WEB_NOISE_ZOOM;
    float ky = WEB_EY * 1.2f * WEB_NOISE_ZOOM;
    int f, j, i;
    for (f = 0; f < 3; f++) {
        sfix off = f == 0 ? 0 : (f == 1 ? 102944 : 205887);
        for (j = 0; j < NF_H; j++) {
            float evy = (((float)j / (float)(NF_H - 1)) * 2.0f - 1.0f) * ky;
            sfix evy_s = (sfix)(evy * 65536.0f);
            for (i = 0; i < NF_W; i++) {
                float eux = (((float)i / (float)(NF_W - 1)) * 2.0f - 1.0f) * kx;
                sfix eux_s = (sfix)(eux * 65536.0f);
                sfix r = snoise3_fx(eux_s + off, evy_s + off, vt + off);
                dst[f][j][i] = ((float)r / 65536.0f + 1.0f) * 0.5f;
            }
            dst[f][j][NF_W] = dst[f][j][NF_W - 1];
        }
        for (i = 0; i <= NF_W; i++) {
            dst[f][NF_H][i] = dst[f][NF_H - 1][i];
        }
    }
    sceKernelDcacheWritebackRange(dst, sizeof(s_fgridA));
}

// По-кадровая интерполяция сеток: L = lerp(prev, curr, 1-age/3). ~0.3мс.
static void vfpu_lerp_grids(void)
{
    float (*prv)[19][33] = s_gridParity ? s_fgridA : s_fgridB;
    float (*cur)[19][33] = s_gridParity ? s_fgridB : s_fgridA;
    float k = 1.0f - (float)s_gridAge * (1.0f / 3.0f);
    int f, j, i;
    if (k < 0.0f) k = 0.0f;
    for (f = 0; f < 3; f++) {
        for (j = 0; j <= NF_H; j++) {
            for (i = 0; i <= NF_W; i++) {
                float a = prv[f][j][i];
                s_fgridL[f][j][i] = a + (cur[f][j][i] - a) * k;
            }
        }
    }
    sceKernelDcacheWritebackRange(s_fgridL, sizeof(s_fgridL));
}

// Спарк-LUT шейдера во float (стадия 3). Та же математика, что C-путь.
static void vfpu_build_sparklut(sfix vtf)
{
    int i;
    for (i = 0; i < SPARK_N; i++) {
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
        s_fsparkLUT[i] = (float)fx_mul(sp, gate) / 65536.0f;
    }
}

// Построчные спарк и recip/circle (стадия 3). brad/bwidH/bmul — литералы кругов.
static void vfpu_build_sparkrow(int y)
{
    static const float kBrad[3] = { 0.95f, 0.80f, 0.65f };
    static const float kBwidH[3] = { 0.40f, 0.30f, 0.20f };
    static const float kBmul[3] = { 1.0f, 0.70f, 0.40f };
    int xq, i, q;
    float vc = (float)y * (1.0f / (GLOW_H - 1));
    float uvy = (vc * 2.0f - 1.0f) * WEB_EY;
    for (xq = 0; xq < 60; xq++) {
        float uc = ((float)(4 * xq) + 1.5f) * (1.0f / (GLOW_W - 1));
        float uvx = (uc * 2.0f - 1.0f) * WEB_EX;
        float ang = fast_atan2(uvy, uvx) * 0.15915494f + 0.5f;
        float fi, ft, spark;
        int i0, i1;
        if (ang < 0.0f) ang = 0.0f;
        if (ang > 1.0f) ang = 1.0f;
        fi = ang * (float)(SPARK_N - 1);
        i0 = (int)fi;
        if (i0 < 0) i0 = 0;
        if (i0 > SPARK_N - 1) i0 = SPARK_N - 1;
        ft = fi - (float)i0;
        i1 = i0 + 1;
        if (i1 > SPARK_N - 1) i1 = SPARK_N - 1;
        spark = s_fsparkLUT[i0] + (s_fsparkLUT[i1] - s_fsparkLUT[i0]) * ft;
        for (q = 0; q < 4; q++) s_sparkQR[xq][q] = spark;
        for (i = 0; i < 3; i++) {
            float outer = kBrad[i] + kBwidH[i] + kBmul[i] * spark;
            float recip = 1.0f / (outer - 0.5f);
            for (q = 0; q < 4; q++) s_recipQQR[i][xq][q] = recip;
        }
    }
}

// Стадия 1: фон + круги (const lay/spark), полное разрешение.
// Регистры: R000=uvx R100=uvy R001=K41 R101=KHALF R103=ONE R200-202=cr/cg/cb.
// Круги: R300-303=offx/offy/invedge/inten R400/R401=brad/bwidH R002/R003=c1/c2.
// Временные: R500=blen R501/R502=dx/dy R503=llen R600-603 R700-703.
#define VFPU_CIRCLE(po_, coff_, gi_, rq_) \
        "lw %[s0], " #gi_ "(%[BL])\n\t" \
        "addu %[s0], %[s0], %[Y0B]\n\t" \
        "addu %[s1], %[s0], 132\n\t" \
        "addu %[s0], %[s0], %[X0B]\n\t" \
        "addu %[s1], %[s1], %[X0B]\n\t" \
        "vsub.q R700, R103, R402\n\t" \
        "vsub.q R702, R103, R403\n\t" \
        "vzero.q R701\n\t" \
        "lv.s S300, 0(%[s0])\n\t" \
        "vmul.q R703, R700, R702\n\t" \
        "vscl.q R703, R703, S300\n\t" \
        "vadd.q R701, R701, R703\n\t" \
        "lv.s S300, 4(%[s0])\n\t" \
        "vmul.q R703, R402, R702\n\t" \
        "vscl.q R703, R703, S300\n\t" \
        "vadd.q R701, R701, R703\n\t" \
        "lv.s S300, 0(%[s1])\n\t" \
        "vmul.q R703, R700, R403\n\t" \
        "vscl.q R703, R703, S300\n\t" \
        "vadd.q R701, R701, R703\n\t" \
        "lv.s S300, 4(%[s1])\n\t" \
        "vmul.q R703, R402, R403\n\t" \
        "vscl.q R703, R703, S300\n\t" \
        "vadd.q R701, R701, R703\n\t" \
        "lw %[s0], 24(%[BL])\n\t" \
        "addu %[s0], %[s0], " #po_ "\n\t" \
        "lv.q R300, 32(%[s0])\n\t" \
        "lv.q R301, 48(%[s0])\n\t" \
        "lv.q R302, 0(%[s0])\n\t" \
        "lv.q R303, 16(%[s0])\n\t" \
        "vadd.q R501, R000, R300\n\t" \
        "vadd.q R502, R100, R301\n\t" \
        "vabs.q R501, R501\n\t" \
        "vabs.q R502, R502\n\t" \
        "vmax.q R503, R501, R502\n\t" \
        "vmin.q R700, R501, R502\n\t" \
        "vmul.q R700, R700, R001\n\t" \
        "vadd.q R503, R503, R700\n\t" \
        "vsub.q R601, R500, R101\n\t" \
        "addu %[s2], %[RQX], " #rq_ "\n\t" \
        "lv.q R700, 0(%[s2])\n\t" \
        "vmul.q R602, R601, R700\n\t" \
        "vsat0.q R602, R602\n\t" \
        "vmul.q R603, R602, R602\n\t" \
        "vsub.q R700, R103, R602\n\t" \
        "vadd.q R700, R700, R700\n\t" \
        "vadd.q R700, R103, R700\n\t" \
        "vmul.q R603, R603, R700\n\t" \
        "vsub.q R602, R103, R603\n\t" \
        "vsub.q R600, R503, R701\n\t" \
        "vmul.q R700, R600, R302\n\t" \
        "vsat0.q R700, R700\n\t" \
        "vsub.q R601, R103, R700\n\t" \
        "vmul.q R601, R601, R602\n\t" \
        "vabs.q R600, R600\n\t" \
        "lw %[s2], 76(%[BL])\n\t" \
        "lv.q R603, 0(%[s2])\n\t" \
        "vmul.q R600, R600, R603\n\t" \
        "vadd.q R600, R600, R103\n\t" \
        "vdiv.q R602, R303, R600\n\t" \
        "vadd.q R603, R100, R301\n\t" \
        "vadd.q R603, R603, R603\n\t" \
        "vsat0.q R603, R603\n\t" \
        "lw %[s1], 20(%[BL])\n\t" \
        "addu %[s1], %[s1], " #coff_ "\n\t"

#define VFPU_CHAN(cr_, c1o_, c2o_) \
        "lv.q R002, " #c1o_ "(%[s1])\n\t" \
        "lv.q R003, " #c2o_ "(%[s1])\n\t" \
        "vsub.q R700, R003, R002\n\t" \
        "vmul.q R700, R700, R603\n\t" \
        "vadd.q R700, R700, R002\n\t" \
        "vadd.q R700, R700, R602\n\t" \
        "vsat0.q R700, R700\n\t" \
        "vsub.q R700, R700, R" #cr_ "\n\t" \
        "vmul.q R700, R700, R601\n\t" \
        "vadd.q R" #cr_ ", R" #cr_ ", R700\n\t"

static void vfpu_render_pack(float bright)
{
    // Байер 4x4 для дитеринга (±0.5 LSB): разбивает 8-битные bands градиента.
    static const float kDith[16] = {
        0.0f/16.0f,  8.0f/16.0f,  2.0f/16.0f, 10.0f/16.0f,
        12.0f/16.0f,  4.0f/16.0f, 14.0f/16.0f,  6.0f/16.0f,
        3.0f/16.0f, 11.0f/16.0f,  1.0f/16.0f,  9.0f/16.0f,
        15.0f/16.0f,  7.0f/16.0f, 13.0f/16.0f,  5.0f/16.0f
    };
    int y, xq;
#if VFPU_DUMP
    {
        static int dumped = 0;
        if (!dumped) {
            int yy, xx;
            dumped = 1;
            logLine("vfdump colT0: %f %f %f / %f %f %f\n",
                s_fcolT[0][0][0][0], s_fcolT[0][1][0][0], s_fcolT[0][2][0][0],
                s_fcolT[0][0][1][0], s_fcolT[0][1][1][0], s_fcolT[0][2][1][0]);
            logLine("vfdump prmT0: inv=%f inten=%f off=(%f,%f) brad=%f bwidH=%f\n",
                s_fprmT[0][0][0], s_fprmT[0][1][0], s_fprmT[0][2][0],
                s_fprmT[0][3][0], s_fprmT[0][4][0], s_fprmT[0][5][0]);
            logLine("vfdump bgT0=%f bgT59=%f uvx0=%f uvx59=%f\n",
                s_fbgT[0][0][0], s_fbgT[59][0][3], s_fuvxq[0][0], s_fuvxq[59][3]);
            (void)yy; (void)xx;
        }
    }
#endif
    for (y = 0; y < GLOW_H; y++) {
        const float *uy = &s_fuvyq[y][0];
        const float *vt = &s_fvigt[y][0];
        const float *tyr = &s_fty2[y][0];
        const float *rqx = &s_recipQQR[0][0][0];
        int y0b = (int)s_fy0r[y] * 132;
        if ((s_web_tick % 3) == 0) {
            vfpu_build_sparkrow(y);
            sceKernelDcacheWritebackRange(s_sparkQR, sizeof(s_sparkQR));
            sceKernelDcacheWritebackRange(s_recipQQR, sizeof(s_recipQQR));
        }
        for (xq = 0; xq < 60; xq++) {
            const float *T = &s_ftmp[xq][0][0];
            const float *BG = &s_fbgT[xq][0][0];
            const float *UX = &s_fuvxq[xq][0];
            const float *NX = &s_fnx2q[xq][0];
            const float *TX = &s_ftxq[xq][0];
            int x0b = (int)s_fx0q[xq] * 4;
            const VfpuBlob *BL = &s_vblob;
            void *s0, *s1, *s2;
            __asm__ volatile (
                "lw %[s2], 48(%[BL])\n\t"
                "lv.q R103, 0(%[s2])\n\t"
                "lw %[s2], 60(%[BL])\n\t"
                "lv.q R001, 0(%[s2])\n\t"
                "lw %[s2], 64(%[BL])\n\t"
                "lv.q R101, 0(%[s2])\n\t"
                "lv.q R402, 0(%[TX])\n\t"
                "lv.q R403, 0(%[TYR])\n\t"
                "lv.q R000, 0(%[UX])\n\t"
                "lv.q R100, 0(%[UY])\n\t"
                "vabs.q R501, R000\n\t"
                "vabs.q R502, R100\n\t"
                "vmax.q R503, R501, R502\n\t"
                "vmin.q R700, R501, R502\n\t"
                "vmul.q R700, R700, R001\n\t"
                "vadd.q R500, R503, R700\n\t"
                "lv.q R200, 0(%[BG])\n\t"
                "lv.q R201, 16(%[BG])\n\t"
                "lv.q R202, 32(%[BG])\n\t"
                VFPU_CIRCLE(0, 0, 0, 0)
                VFPU_CHAN(200, 0, 16)
                VFPU_CHAN(201, 32, 48)
                VFPU_CHAN(202, 64, 80)
                VFPU_CIRCLE(128, 96, 4, 960)
                VFPU_CHAN(200, 0, 16)
                VFPU_CHAN(201, 32, 48)
                VFPU_CHAN(202, 64, 80)
                VFPU_CIRCLE(256, 192, 8, 1920)
                VFPU_CHAN(200, 0, 16)
                VFPU_CHAN(201, 32, 48)
                VFPU_CHAN(202, 64, 80)
                "lv.q R601, 0(%[NX])\n\t"
                "lw %[s2], 68(%[BL])\n\t"
                "lv.q R102, 0(%[s2])\n\t"
                "lv.q R600, 0(%[VT])\n\t"
                "lw %[s2], 72(%[BL])\n\t"
                "lv.q R603, 0(%[s2])\n\t"
                "vmul.q R601, R601, R102\n\t"
                "vadd.q R601, R601, R600\n\t"
                "vmul.q R601, R601, R603\n\t"
                "vsub.q R600, R103, R601\n\t"
                "vmul.q R200, R200, R600\n\t"
                "vmul.q R201, R201, R600\n\t"
                "vmul.q R202, R202, R600\n\t"
                "sv.q R200, 0(%[T])\n\t"
                "sv.q R201, 16(%[T])\n\t"
                "sv.q R202, 32(%[T])\n\t"
                : [s0]"=&r"(s0), [s1]"=&r"(s1), [s2]"=&r"(s2)
                : [BL]"r"(BL), [T]"r"(T), [BG]"r"(BG), [UX]"r"(UX),
                  [NX]"r"(NX), [UY]"r"(uy), [VT]"r"(vt), [TX]"r"(TX),
                  [TYR]"r"(tyr), [Y0B]"r"(y0b), [X0B]"r"(x0b), [RQX]"r"(rqx)
                : "memory");
        }
        // Строка s_ftmp только что записана VFPU мимо CPU-кэша: инвалидировать.
        sceKernelDcacheInvalidateRange(&s_ftmp[0][0][0], sizeof(s_ftmp));
        {
            unsigned char *dst = s_buf + (size_t)y * GLOW_PITCH * 4;
            for (xq = 0; xq < 60; xq++) {
                int lane;
                for (lane = 0; lane < 4; lane++) {
                    int px = xq * 4 + lane;
                    float r = s_ftmp[xq][0][lane];
                    float g = s_ftmp[xq][1][lane];
                    float b = s_ftmp[xq][2][lane];
                    int ri, gi, bi;
                    r = s_fbgq[px][0] + (r - s_fbgq[px][0]) * bright;
                    g = s_fbgq[px][1] + (g - s_fbgq[px][1]) * bright;
                    b = s_fbgq[px][2] + (b - s_fbgq[px][2]) * bright;
                    if (s_liked) {
                        r += 0.0392f; g -= 0.0157f; b -= 0.0392f;
                    }
                    {
                        float dth = (kDith[(px & 3) | ((y & 3) << 2)] - 0.5f) * (1.0f / 255.0f);
                        r += dth; g += dth; b += dth;
                    }
                    ri = (int)(r * 255.0f + 0.5f);
                    gi = (int)(g * 255.0f + 0.5f);
                    bi = (int)(b * 255.0f + 0.5f);
                    if (ri < 0) ri = 0; else if (ri > 255) ri = 255;
                    if (gi < 0) gi = 0; else if (gi > 255) gi = 255;
                    if (bi < 0) bi = 0; else if (bi > 255) bi = 255;
                    dst[px * 4 + 0] = (unsigned char)ri;
                    dst[px * 4 + 1] = (unsigned char)gi;
                    dst[px * 4 + 2] = (unsigned char)bi;
                    dst[px * 4 + 3] = 255;
                }
            }
        }
    }
}
#endif
static float s_vtime = 0.0f;
static unsigned long long s_last_us = 0;
static unsigned s_web_tick = 0;
static int s_web_init = 0;

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

// Быстрая длина вектора в fixed (погрешность ~4%) — для мягких масок годится.
static sfix fast_len_fx(sfix x, sfix y)
{
    sfix ax = fx_abs(x), ay = fx_abs(y);
    sfix m = ax > ay ? ax : ay;
    sfix n = ax > ay ? ay : ax;
    return m + fx_mul(n, 26870);  // +0.41*min
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
        float v = rzf + fsin(rzf + fsin(zzf) * 2.8f) * 2.2f;
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
        float sf = fsin((float)seam_angle / 65536.0f);
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
    s_liked = liked ? 1 : 0;
}

void glow_bg_pulse(void)
{
    s_pulse_until = s_frame + 45;  // ~0.75 сек при 60fps
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
    fs_ensure_dir(GLOW_STYLE_PATH);
    fd = fs_open(GLOW_STYLE_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        logLine("glow: style save open failed 0x%08X\n", fd);
        return;
    }
    written = fs_write(fd, buffer, (size_t)length);
    fs_close(fd);
    if (written != length) {
        logLine("glow: style save short %d/%d\n", written, length);
        return;
    }
    logLine("glow: style=%d saved\n", style);
    logger_flush();
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

// Классика: 4 мягких пятна (оригинал, каждый кадр, полное разрешение).
static void classic_update(void)
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
static void web_build_fields(sfix vt)
{
    sfix kx = (sfix)(WEB_EX * 1.2f * WEB_NOISE_ZOOM * 65536.0f);
    sfix ky = (sfix)(WEB_EY * 1.2f * WEB_NOISE_ZOOM * 65536.0f);
    int x, y;
    for (y = 0; y < NF_H; y++) {
        sfix vf = ((sfix)y << 16) / (NF_H - 1);
        sfix evy = fx_mul(vf + vf - 65536, ky);
        for (x = 0; x < NF_W; x++) {
            sfix uf = ((sfix)x << 16) / (NF_W - 1);
            sfix eux = fx_mul(uf + uf - 65536, kx);
            s_nfield[0][y][x] = (snoise3_fx(eux, evy, vt) + 65536) / 2;
            s_nfield[1][y][x] = (snoise3_fx(eux + 102944, evy + 102944, vt + 102944) + 65536) / 2;
            s_nfield[2][y][x] = (snoise3_fx(eux + 205887, evy + 205887, vt + 205887) + 65536) / 2;
        }
    }
}

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

// Веб-стиль: порт canvas-шейдера, hot path в fixed-point, полное разрешение.
static void web_update(float dt)
{
    static const float kRot[3][3] = {
        { -0.3f, 0.3f, 0.2f }, { -0.3f, -0.3f, -0.2f }, { -0.3f, -0.3f, 0.2f }
    };
    float bright = s_playing ? 1.0f : 0.55f;
    float pulse = 1.0f;
    sfix ex = (sfix)(WEB_EX * 65536.0f);
    sfix ey = (sfix)(WEB_EY * 65536.0f);
    sfix offx[3], offy[3], edge[3], inten[3], inv_edge[3];
    sfix brad0[3], bwid_[3], bmul_[3];
    sfix wrgb[6][3];
    sfix bright_fx;
    int i, x, y;

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
#if VFPU_STAGE
    // VFPU-путь (полное разрешение): float-таблицы + ядро + пак. C-путь ниже
    // остаётся как сохранённый быстрый вариант (VFPU_STAGE 0).
    if ((s_web_tick % 3) == 0) {
        sfix vt = (sfix)(s_vtime * 32768.0f);
        sfix vtf = (sfix)(s_vtime * 65536.0f);
        s_gridParity ^= 1;
        vfpu_build_grids(vt);
        vfpu_build_sparklut(vtf);
        sceKernelDcacheWritebackRange(s_fsparkLUT, sizeof(s_fsparkLUT));
        s_gridAge = 0;
    } else if (s_gridAge < 2) {
        s_gridAge++;
    }
    vfpu_lerp_grids();
    vfpu_build_frame(bright);
    // Кадровые таблицы в RAM для VFPU.
    sceKernelDcacheWritebackRange(s_fbgT, sizeof(s_fbgT));
    sceKernelDcacheWritebackRange(s_fcolT, sizeof(s_fcolT));
    sceKernelDcacheWritebackRange(s_fprmT, sizeof(s_fprmT));
    vfpu_render_pack(bright);
    s_web_tick++;
    return;
#endif
    for (i = 0; i < 6; i++) {
        float r, g, b;
        hsl_to_rgb(s_web_col[i][0], s_web_col[i][1], s_web_col[i][2], &r, &g, &b);
        wrgb[i][0] = (sfix)(r * 256.0f);
        wrgb[i][1] = (sfix)(g * 256.0f);
        wrgb[i][2] = (sfix)(b * 256.0f);
    }
    bright_fx = (sfix)(bright * 65536.0f);

    // Фон: градиент бакета по колонкам (угол 270°: свет справа).
    for (x = 0; x < WEB_RW; x++) {
        float pos = (1.0f - (float)x / (float)(WEB_RW - 1)) * 100.0f;
        int s = 0;
        float f;
        while (s < WEB_STOPS - 2 && kWebStopPos[s + 1] < pos) s++;
        f = (pos - kWebStopPos[s]) /
            (kWebStopPos[s + 1] - kWebStopPos[s] + 1e-6f);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        for (i = 0; i < 3; i++) {
            s_wbg[x][i] = (sfix)(((s_web_stops[s][i] +
                (s_web_stops[s + 1][i] - s_web_stops[s][i]) * f) * 0.62f) * 256.0f);
        }
    }

    for (i = 0; i < WEB_CIRCLES; i++) {
        float ra = s_vtime * kRot[i][2];
        float cs = cosf(ra), sn = sinf(ra);
        offx[i] = (sfix)((kRot[i][0] * cs - kRot[i][1] * sn) * 65536.0f);
        offy[i] = (sfix)((kRot[i][0] * sn + kRot[i][1] * cs) * 65536.0f);
        edge[i] = (sfix)((1.1f + sinf(s_vtime + 1.57f * (float)i)) * 65536.0f);
        inten[i] = (sfix)((0.15f * (1.0f - 0.75f * sinf(s_vtime * 2.0f + 0.785f * (float)i))) * 65536.0f);
        inv_edge[i] = fx_div(65536, edge[i]);
        brad0[i] = (sfix)((0.95f - 0.15f * (float)i) * 65536.0f);
        bwid_[i] = (sfix)((0.8f - 0.2f * (float)i) * 65536.0f);
        bmul_[i] = 65536 - (sfix)((float)i * 19661.0f);
    }

    // Медленно морфирующие части — шум и спарк-LUT — раз в 3 пересчёта.
    if ((s_web_tick % 3) == 0) {
        sfix vt = (sfix)(s_vtime * 32768.0f);  // vtime*0.5
        sfix vtf = (sfix)(s_vtime * 65536.0f);
        web_build_fields(vt);

        // Спарк-LUT по углу (triNoise3D шейдера с бесшовной склейкой).
        // shapeSpark сразу запечён в LUT — попиксельно остаётся лишь lerp.
        for (i = 0; i < SPARK_N; i++) {
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
            s_spark[i] = fx_mul(sp, gate);
        }
    }
    s_web_tick++;

    for (y = 0; y < WEB_H2; y++) {
        sfix vf, ny, uvy, ny2;
        vf = s_vv2[y];
        ny = vf + vf - 65536;
        uvy = fx_mul(ny, ey);
        ny2 = fx_mul(ny, ny);

        for (x = 0; x < WEB_W2; x++) {
            sfix uf = s_uu2[x];
            sfix nx = uf + uf - 65536;
            sfix uvx = fx_mul(nx, ex);
            sfix blen = fast_len_fx(uvx, uvy);
            sfix fi, ft, spark;
            sfix ch0, ch1, ch2;
            sfix cr, cg, cb, vig;
            unsigned char *p = s_buf + (size_t)(y * GLOW_PITCH + x) * 4;
            int i0, i1;

            fi = fx_mul((sfix)s_spark_x2[y][x] * 257, 16711680);
            i0 = (int)(fi >> 16);
            if (i0 > SPARK_N - 1) i0 = SPARK_N - 1;
            if (i0 < 0) i0 = 0;
            ft = fi - ((sfix)i0 << 16);
            i1 = i0 + 1;
            if (i1 > SPARK_N - 1) i1 = SPARK_N - 1;
            spark = s_spark[i0] + fx_mul(s_spark[i1] - s_spark[i0], ft);

            // Три поля одним проходом: веса билинейки общие (та же математика).
            {
                sfix fx = fx_mul(uf, (sfix)((NF_W - 1) << 16));
                sfix fy = fx_mul(vf, (sfix)((NF_H - 1) << 16));
                int x0 = fx_floor(fx), y0 = fx_floor(fy);
                int x1 = x0 + 1, y1 = y0 + 1;
                sfix tx = fx - ((sfix)x0 << 16), ty = fy - ((sfix)y0 << 16);
                sfix w00 = fx_mul(65536 - tx, 65536 - ty);
                sfix w10 = fx_mul(tx, 65536 - ty);
                sfix w01 = fx_mul(65536 - tx, ty);
                sfix w11 = fx_mul(tx, ty);
                if (x1 > NF_W - 1) x1 = NF_W - 1;
                if (y1 > NF_H - 1) y1 = NF_H - 1;
                ch0 = fx_mul(s_nfield[0][y0][x0], w00) + fx_mul(s_nfield[0][y0][x1], w10) +
                      fx_mul(s_nfield[0][y1][x0], w01) + fx_mul(s_nfield[0][y1][x1], w11);
                ch1 = fx_mul(s_nfield[1][y0][x0], w00) + fx_mul(s_nfield[1][y0][x1], w10) +
                      fx_mul(s_nfield[1][y1][x0], w01) + fx_mul(s_nfield[1][y1][x1], w11);
                ch2 = fx_mul(s_nfield[2][y0][x0], w00) + fx_mul(s_nfield[2][y0][x1], w10) +
                      fx_mul(s_nfield[2][y1][x0], w01) + fx_mul(s_nfield[2][y1][x1], w11);
            }

            cr = s_wbg[x][0];
            cg = s_wbg[x][1];
            cb = s_wbg[x][2];

            for (i = 0; i < WEB_CIRCLES; i++) {
                sfix brad = brad0[i] + fx_mul(19661, ch0);
                sfix bwid = bwid_[i];
                sfix bmul = bmul_[i];
                sfix bspark = fx_mul(bmul, spark);
                sfix outer = brad + (bwid >> 1) + bspark;
                sfix dx, dy, llen, dn, lay, alpha, om, glow, tt;
                sfix rr, gg, bb;

                if (blen >= outer) continue;
                dx = uvx + offx[i];
                dy = uvy + offy[i];
                llen = fast_len_fx(dx, dy);
                lay = i == 0 ? ch0 : (i == 1 ? ch1 : ch2);
                dn = llen >= lay ? llen - lay : lay - llen;
                alpha = 65536 - fx_clamp01(fx_mul(llen - lay, inv_edge[i]));
                {
                    // Было: t2 = (blen-0.5)/(outer-0.5) через div.
                    // Стало: индекс в RECIP (1/(0.25+q/32), ошибка <2% — невидимо).
                    int rq = (int)((outer - 49152) >> 11);
                    sfix t2;
                    if (rq < 0) rq = 0; else if (rq > RECIP_N - 1) rq = RECIP_N - 1;
                    t2 = fx_clamp01(fx_mul(blen - 32768, s_recip[rq]));
                    om = 65536 - fx_mul(fx_mul(t2, t2), 196608 - 2 * t2);
                }
                alpha = fx_mul(alpha, om);
                if (alpha <= 0) continue;
                {
                    // Было: glow = inten/(1+11*dn) через div.
                    // Стало: GLOWFALL (точное на узлах сетки 1/256).
                    int gq = (int)(dn >> 8);
                    if (gq > GLOWFALL_N - 1) gq = GLOWFALL_N - 1;
                    glow = fx_mul(inten[i], s_glowfall[gq]);
                }
                tt = fx_clamp01((uvy + offy[i]) * 2);
                rr = wrgb[i][0] + fx_mul(wrgb[i + 3][0] - wrgb[i][0], tt) + fx_mul(glow, 65280);
                gg = wrgb[i][1] + fx_mul(wrgb[i + 3][1] - wrgb[i][1], tt) + fx_mul(glow, 65280);
                bb = wrgb[i][2] + fx_mul(wrgb[i + 3][2] - wrgb[i][2], tt) + fx_mul(glow, 65280);
                if (rr < 0) rr = 0; else if (rr > 65280) rr = 65280;
                if (gg < 0) gg = 0; else if (gg > 65280) gg = 65280;
                if (bb < 0) bb = 0; else if (bb > 65280) bb = 65280;
                cr += fx_mul(rr - cr, alpha);
                cg += fx_mul(gg - cg, alpha);
                cb += fx_mul(bb - cb, alpha);
            }

            // На паузе кольца гаснут к фону.
            cr = s_wbg[x][0] + fx_mul(cr - s_wbg[x][0], bright_fx);
            cg = s_wbg[x][1] + fx_mul(cg - s_wbg[x][1], bright_fx);
            cb = s_wbg[x][2] + fx_mul(cb - s_wbg[x][2], bright_fx);

            if (s_liked) {
                cr += 2560;
                cg -= 1024;
                cb -= 2560;
            }

            vig = 65536 - fx_mul(19661, fx_mul(fx_mul(nx, nx), 45875) +
                                          fx_mul(ny2, 32768));
            cr = fx_mul(cr, vig);
            cg = fx_mul(cg, vig);
            cb = fx_mul(cb, vig);

            if (cr < 0) cr = 0; else if (cr > 65280) cr = 65280;
            if (cg < 0) cg = 0; else if (cg > 65280) cg = 65280;
            if (cb < 0) cb = 0; else if (cb > 65280) cb = 65280;
            p[0] = (unsigned char)((cr + 128) >> 8);
            p[1] = (unsigned char)((cg + 128) >> 8);
            p[2] = (unsigned char)((cb + 128) >> 8);
            p[3] = 255;
        }
    }
}

// Обновление текстуры: каждый стиль — каждый кадр (плавность без рывков).
static void glow_update(void)
{
    unsigned long long now;
    float dt;

    if (!s_sin_ready) {
        sin_tab_init();
    }

    if (s_style == GLOW_STYLE_CLASSIC) {
        classic_update();
    } else if (s_style == GLOW_STYLE_STRANDS) {
        strands_update();
    } else {
        now = sceKernelGetSystemTimeWide();
        dt = s_last_us ? (float)(now - s_last_us) / 1000000.0f : 0.033f;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.25f) dt = 0.25f;
        s_last_us = now;
        web_update(dt);
    }

    // Padding для LINEAR-фильтра: реплика последнего столбца/строки.
    // VFPU-ядро (как классика/нити) рисует 240x136; C-путь веба — 120x68.
    {
        int y, pw, ph;
#if VFPU_STAGE
        pw = GLOW_W;
        ph = GLOW_H;
#else
        pw = (s_style == GLOW_STYLE_WEB) ? WEB_W2 : GLOW_W;
        ph = (s_style == GLOW_STYLE_WEB) ? WEB_H2 : GLOW_H;
#endif
        for (y = 0; y < ph; y++) {
            memcpy(s_buf + (size_t)(y * GLOW_PITCH + pw) * 4,
                   s_buf + (size_t)(y * GLOW_PITCH + pw - 1) * 4,
                   (size_t)(GLOW_PITCH - pw) * 4);
        }
        memcpy(s_buf + (size_t)(ph * GLOW_PITCH) * 4,
               s_buf + (size_t)((ph - 1) * GLOW_PITCH) * 4,
               (size_t)GLOW_PITCH * 4);
    }
}

void glow_bg_init(void)
{
    if (s_buf) {
        return;
    }
#if VFPU_PROBE
    vfpu_probe_run();
#endif
    s_buf = (unsigned char *)memalign(64, (size_t)GLOW_PITCH * GLOW_TEX_H * 4);
    if (s_buf) {
        memset(s_buf, 0, (size_t)GLOW_PITCH * GLOW_TEX_H * 4);
    }
    s_frame = 0;
    glow_config_load();

    // Предрасчёты статической геометрии веб-стиля: разово, не в кадре.
    if (!s_sin_ready) {
        sin_tab_init();
    }
    {
        int x, y;
        for (x = 0; x < GLOW_W; x++) {
            s_uu[x] = ((sfix)x << 16) / (GLOW_W - 1);
        }
        for (y = 0; y < GLOW_H; y++) {
            s_vv[y] = ((sfix)y << 16) / (GLOW_H - 1);
        }
        for (y = 0; y < GLOW_H; y++) {
            float v = (float)y * (1.0f / (GLOW_H - 1));
            float uvy = (v * 2.0f - 1.0f) * WEB_EY;
            for (x = 0; x < GLOW_W; x++) {
                float u = (float)x * (1.0f / (GLOW_W - 1));
                float uvx = (u * 2.0f - 1.0f) * WEB_EX;
                float ang = fast_atan2(uvy, uvx) * 0.15915494f + 0.5f;
                if (ang < 0.0f) ang = 0.0f;
                if (ang > 1.0f) ang = 1.0f;
                s_spark_x[y][x] = (unsigned char)(ang * 255.0f);
            }
        }
        // Половинная геометрия веб-стиля + LUT вместо попиксельных делений.
        // Всё целочисленное, разово при инициализации.
        for (x = 0; x < WEB_W2; x++) {
            s_uu2[x] = (sfix)(((int64_t)(4 * x + 1) << 15) / (GLOW_W - 1));
        }
        for (y = 0; y < WEB_H2; y++) {
            s_vv2[y] = (sfix)(((int64_t)(4 * y + 1) << 15) / (GLOW_H - 1));
        }
        for (y = 0; y < WEB_H2; y++) {
            float v2 = ((float)(2 * y) + 0.5f) * (1.0f / (GLOW_H - 1));
            float uvy2 = (v2 * 2.0f - 1.0f) * WEB_EY;
            for (x = 0; x < WEB_W2; x++) {
                float u2 = ((float)(2 * x) + 0.5f) * (1.0f / (GLOW_W - 1));
                float uvx2 = (u2 * 2.0f - 1.0f) * WEB_EX;
                float ang2 = fast_atan2(uvy2, uvx2) * 0.15915494f + 0.5f;
                if (ang2 < 0.0f) ang2 = 0.0f;
                if (ang2 > 1.0f) ang2 = 1.0f;
                s_spark_x2[y][x] = (unsigned char)(ang2 * 255.0f);
            }
        }
        for (x = 0; x < GLOWFALL_N; x++) {
            s_glowfall[x] = (sfix)(16777216 / (256 + 11 * x));
        }
        for (x = 0; x < RECIP_N; x++) {
            s_recip[x] = (sfix)(2097152 / (8 + x));
        }
#if VFPU_STAGE
        vfpu_init_tables();
#endif
    }
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
        v[1].u = (unsigned short)(
#if VFPU_STAGE
            GLOW_W
#else
            ((s_style == GLOW_STYLE_WEB) ? WEB_W2 : GLOW_W)
#endif
        );
        v[1].v = (unsigned short)(
#if VFPU_STAGE
            GLOW_H
#else
            ((s_style == GLOW_STYLE_WEB) ? WEB_H2 : GLOW_H)
#endif
        );
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
