#ifndef YM_UI_GLOW_BG_H
#define YM_UI_GLOW_BG_H

// Светящийся фон как в Яндекс Музыке (vibe-визуализация).
// CPU генерирует 4 мягких пятна в 240x136, GU растягивает до 480x272
// с LINEAR-фильтром и alpha-блендом (мягкое свечение).
// Реагирует на воспроизведение (яркость/скорость), лайк (тёплый оттенок)
// и смену трека (пульс).
void glow_bg_init(void);
void glow_bg_free(void);
void glow_bg_draw(float x, float y, float w, float h);

// Вызывать из UI-слоя (не из аудиопотока):
void glow_bg_set_playing(int playing);   // 0 пауза, 1 играет
void glow_bg_set_liked(int liked);       // 0 обычный, 1 тёплый оттенок
void glow_bg_pulse(void);                // вспышка при смене трека

#endif
