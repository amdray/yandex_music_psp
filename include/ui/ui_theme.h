#ifndef YM_UI_THEME_H
#define YM_UI_THEME_H

#include <psptypes.h>
#include "services/locale.h"

/* Темы оформления (ещё: THEME_COVER — цвета обложки + жирный шрифт): шрифт + цвета интерфейса.
 *  THEME_DEFAULT — как раньше;
 *  THEME_GRI     — газоразрядный индикатор (Nixie): тонкие светящиеся янтарные «проволочные» буквы;
 *  THEME_PDP     — плазменная панель: красно-оранжевая точечная матрица на чёрном.
 * Цвета интерфейса (серая шкала и акцент) перекрашиваются при отрисовке в
 * ui_theme_map(); обложка и эффекты свечения не затрагиваются. Шрифты берутся из
 * fonts/ui_gri.cbmf, ui16_gri.cbmf, ui_pdp.cbmf, ui16_pdp.cbmf (нет файла — остаётся
 * обычный шрифт). Выбор хранится в config/theme.cfg. */
typedef enum UiTheme {
    THEME_DEFAULT = 0,
    THEME_GRI,
    THEME_PDP,
    THEME_COVER,   /* цвета берутся из обложки текущего трека, жирный шрифт */
    THEME_COUNT
} UiTheme;

void ui_theme_init(void);               /* идемпотентно, читает config/theme.cfg */
int ui_theme_get(void);
void ui_theme_cycle(int dir);           /* +1 / -1: сохраняет и перезагружает шрифты */
LocaleKey ui_theme_name_key(int theme);
u32 ui_theme_map(u32 color_abgr);

/* Тема «Обложка»: цвета из палитры обложки (вызывает glow_bg_set_cover), плавный
 * переход между треками выполняет ui_theme_tick() раз в кадр. */
void ui_theme_set_cover(const unsigned char accent1[3], const unsigned char accent2[3],
                        const unsigned char average[3]);
void ui_theme_tick(void);       /* перекраска цвета интерфейса (альфа сохраняется) */

#endif
