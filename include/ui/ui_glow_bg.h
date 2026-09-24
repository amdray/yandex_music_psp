#ifndef YM_UI_GLOW_BG_H
#define YM_UI_GLOW_BG_H

// Светящийся фон как в Яндекс Музыке (vibe-визуализация).
// CPU генерирует 3 мягких пятна в 240x136, GU растягивает до 480x272
// с LINEAR-фильтром и premultiplied-блендом (мягкое свечение).
void glow_bg_init(void);
void glow_bg_free(void);
void glow_bg_draw(float x, float y, float w, float h);

#endif
