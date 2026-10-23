#include "ui/ui_theme.h"

#include <stdio.h>
#include <pspkernel.h>

#include "core/fs.h"
#include "core/logger.h"
#include "fonts/cbmf_fonts.h"

#define THEME_CFG_PATH "config/theme.cfg"

typedef struct ThemeTone {
    unsigned char text[3];    /* самый яркий цвет текста */
    unsigned char accent[3];  /* акцент (выделение, прогресс) */
    unsigned char bg[3];      /* фон */
} ThemeTone;

static ThemeTone kTones[THEME_COUNT] = {
    { {255, 255, 255}, {255, 213, 0}, {26, 26, 26} },   /* не используется */
    { {255, 150,  50}, {255, 205, 100}, {6, 3, 1} },    /* ГРИ: янтарь Nixie */
    { {255,  90,  40}, {255, 215, 170}, {4, 0, 0} },    /* PDP: красно-оранжевый неон */
    { {240, 240, 240}, {255, 213, 0}, {26, 26, 26} },   /* Обложка: до первой обложки как обычно */
};
/* Тема «Обложка»: цель (из палитры обложки) и текущее значение (плавно подходит). */
static float s_cov_cur[3][3] = { {240, 240, 240}, {255, 213, 0}, {26, 26, 26} };
static float s_cov_tgt[3][3] = { {240, 240, 240}, {255, 213, 0}, {26, 26, 26} };

static int s_theme = THEME_DEFAULT;
static int s_loaded = 0;

void ui_theme_init(void)
{
    char buf[24];
    SceUID fd;
    int count, v = -1;

    if (s_loaded) return;
    s_loaded = 1;
    fd = fs_open(THEME_CFG_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    count = fs_read(fd, buf, sizeof(buf) - 1);
    fs_close(fd);
    if (count <= 0) return;
    buf[count] = '\0';
    if (sscanf(buf, "theme=%d", &v) == 1 && v >= 0 && v < THEME_COUNT) {
        s_theme = v;
    }
    logLine("theme: %d source=config\n", s_theme);
}

int ui_theme_get(void)
{
    if (!s_loaded) ui_theme_init();
    return s_theme;
}

void ui_theme_cycle(int dir)
{
    char buf[24];
    int n;

    if (!s_loaded) ui_theme_init();
    s_theme = (s_theme + (dir >= 0 ? 1 : THEME_COUNT - 1)) % THEME_COUNT;
    if (cbmf_fonts_set_variant(s_theme) != 0) {
        logLine("theme: font variant %d unavailable, default font kept\n", s_theme);
    }
    n = snprintf(buf, sizeof(buf), "theme=%d\n", s_theme);
    if (n > 0 && n < (int)sizeof(buf)) {
        fs_write_deferred(THEME_CFG_PATH, buf, (size_t)n);
    }
    logLine("theme: %d queued\n", s_theme);
}

LocaleKey ui_theme_name_key(int theme)
{
    switch (theme) {
    case THEME_GRI: return LOCALE_THEME_GRI;
    case THEME_PDP: return LOCALE_THEME_PDP;
    case THEME_COVER: return LOCALE_THEME_COVER;
    default:        return LOCALE_THEME_DEFAULT;
    }
}

u32 ui_theme_map(u32 c)
{
    const ThemeTone *t;
    unsigned r, g, b, mx, mn, a;
    float f, inv;

    if (s_theme == THEME_DEFAULT) return c;
    t = &kTones[s_theme];
    a = c >> 24;
    r = c & 0xFFu; g = (c >> 8) & 0xFFu; b = (c >> 16) & 0xFFu;
    mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    mn = r < g ? (r < b ? r : b) : (g < b ? g : b);

    /* акцент интерфейса: жёлтый #FFD500 (в ABGR 0x00D5FF) */
    if (r > 0xE0u && g > 0xA0u && b < 0x40u) {
        return (a << 24) | ((unsigned)t->accent[2] << 16) |
               ((unsigned)t->accent[1] << 8) | (unsigned)t->accent[0];
    }
    if (mx - mn > 14u) return c;        /* цветное (ошибка и т.п.) — как есть */

    /* серая шкала: фон ≈ 0x1A → тон фона, белый → яркий тон текста */
    f = ((float)r - 32.0f) * (1.0f / 223.0f);
    if (f < 0.0f) f = 0.0f;
    f = f * (1.6f - 0.6f * f);
    inv = 1.0f - f;
    r = (unsigned)((float)t->text[0] * f + (float)t->bg[0] * inv + 0.5f);
    g = (unsigned)((float)t->text[1] * f + (float)t->bg[1] * inv + 0.5f);
    b = (unsigned)((float)t->text[2] * f + (float)t->bg[2] * inv + 0.5f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

/* Яркий светлый тон из цвета обложки: максимум канала → 255, затем подмешивание белого. */
static void lift_tone(const unsigned char in[3], float white, float out[3])
{
    float mx = (float)in[0], k;
    int i;
    if ((float)in[1] > mx) mx = (float)in[1];
    if ((float)in[2] > mx) mx = (float)in[2];
    k = mx > 8.0f ? 255.0f / mx : 1.0f;
    for (i = 0; i < 3; i++) {
        float v = (float)in[i] * k;
        if (mx <= 8.0f) v = 255.0f;
        out[i] = v + (255.0f - v) * white;
    }
}

void ui_theme_set_cover(const unsigned char accent1[3], const unsigned char accent2[3],
                        const unsigned char average[3])
{
    int i;
    unsigned char a2[3];
    int d = 0;
    for (i = 0; i < 3; i++) {
        int t = (int)accent1[i] - (int)accent2[i];
        d += t < 0 ? -t : t;
    }
    for (i = 0; i < 3; i++) a2[i] = d < 90 ? accent1[i] : accent2[i];
    lift_tone(accent1, 0.32f, s_cov_tgt[0]);              /* текст: светлый оттенок акцента */
    lift_tone(a2, d < 90 ? 0.0f : 0.05f, s_cov_tgt[1]);   /* акцент: насыщенный второй цвет */
    for (i = 0; i < 3; i++) {
        float v = (float)average[i] * 0.09f;
        s_cov_tgt[2][i] = v < 3.0f ? 3.0f : v;            /* фон: очень тёмный оттенок среднего */
    }
}

void ui_theme_tick(void)
{
    int k, i;
    if (s_theme != THEME_COVER) return;
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 3; i++) {
            s_cov_cur[k][i] += (s_cov_tgt[k][i] - s_cov_cur[k][i]) * 0.07f;
        }
    }
    for (i = 0; i < 3; i++) {
        kTones[THEME_COVER].text[i]   = (unsigned char)(s_cov_cur[0][i] + 0.5f);
        kTones[THEME_COVER].accent[i] = (unsigned char)(s_cov_cur[1][i] + 0.5f);
        kTones[THEME_COVER].bg[i]     = (unsigned char)(s_cov_cur[2][i] + 0.5f);
    }
}
