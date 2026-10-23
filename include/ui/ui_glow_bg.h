#ifndef YM_UI_GLOW_BG_H
#define YM_UI_GLOW_BG_H

#include "services/locale.h"

// Светящийся фон как в Яндекс Музыке (vibe-визуализация).
// CPU генерирует текстуру 240x136, GU растягивает до 480x272
// с LINEAR-фильтром и alpha-блендом (мягкое свечение).
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
#define GLOW_STYLE_COUNT 6
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

// Подложка под текст: тёмная мягкая плашка в прямоугольнике (x,y,w,h), сила
// которой растёт с яркостью фона под ней (чтобы серый текст не сливался со
// свечением). Вызывать после glow_bg_draw, до отрисовки текста.
void glow_bg_text_scrim(float x, float y, float w, float h);

// Палитра из обложки (как в Яндексе): акцент/вторичный/средний/база.
// rgba — декодированная обложка RGBA8888; вызывать при её загрузке.
void glow_bg_set_cover(const void *rgba, int w, int h, int stride_bytes);

#endif
