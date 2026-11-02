#ifndef YM_UI_GLOW_BG_H
#define YM_UI_GLOW_BG_H

#include <stddef.h>
#include "services/locale.h"

// Светящийся фон как в Яндекс Музыке (vibe-визуализация).
// Встроенные плагины в src/ui/glow: сетка цветов вершин либо текстура
// 240x136 с растяжением до 480x272.
// Реагирует на воспроизведение (яркость/скорость), лайк (тёплый оттенок)
// и смену трека (пульс).
//
// Стили (переключение в настройках, выбор хранится в config/glow.cfg):
//  CLASSIC — 4 мягких пятна; STRANDS — текучие нити;
//  WEB — порт веб-вайба Яндекса на сетке вершин GPU (по умолчанию).
#define GLOW_STYLE_CLASSIC 0
#define GLOW_STYLE_STRANDS 1
#define GLOW_STYLE_WEB 2
// Winamp-подобные визуализации (поверх приглушённого фона «Классики»):
#define GLOW_STYLE_SPECTRUM 3   // анализатор: столбики + колпачки
#define GLOW_STYLE_SCOPE 4      // осциллограф
#define GLOW_STYLE_RADIAL 5     // лучи от центра по спектру
#define GLOW_STYLE_OFF 6
#define GLOW_STYLE_ACCENT 7
#define GLOW_STYLE_COUNT 8
#define GLOW_STYLE_DEFAULT GLOW_STYLE_WEB

void glow_bg_init(void);
void glow_bg_free(void);
void glow_bg_draw(float x, float y, float w, float h);

// Вызывать из UI-слоя (не из аудиопотока):
void glow_bg_set_playing(int playing);   // 0 пауза, 1 играет
void glow_bg_set_liked(int liked);       // 0 обычный, 1 тёплый оттенок
void glow_bg_pulse(void);                // вспышка при смене трека

// Стиль свечения (сохраняется в config/glow.cfg).
void glow_bg_set_style(int style);
int glow_bg_get_style(void);
LocaleKey glow_bg_style_name_key(int style);

// Акцентный режим и «Классика» включают подложку при близости входных цветов
// к тексту. В анимированных режимах её сила зависит от яркости области.
// Границы содержимого имеют полную плотность; край затухает снаружи.
typedef struct UiGlowTextRegion {
    float left, top, right, bottom;
    float feather;
} UiGlowTextRegion;

// Вызывать после glow_bg_draw, до отрисовки текста; feather должен быть > 0.
void glow_bg_text_scrim(const UiGlowTextRegion *region,
                       const unsigned int *text_colors, size_t color_count);

// Палитра из обложки (как в Яндексе): акцент/вторичный/средний/база.
// rgba — декодированная обложка RGBA8888; вызывать при её загрузке.
void glow_bg_set_cover(const void *rgba, int w, int h, int stride_bytes);
/* Server accent in ABGR; zero means the track has no server color. */
void glow_bg_set_accent(unsigned int color);

#endif
