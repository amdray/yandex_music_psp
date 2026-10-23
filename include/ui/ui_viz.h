#ifndef YM_UI_VIZ_H
#define YM_UI_VIZ_H

/* Winamp-подобные визуализации поверх фона (рисуются сразу после сетки свечения,
 * под остальным интерфейсом). Источник — анализ в audio_player (спектр 16 полос,
 * осциллограф 64 точки). Вся геометрия — один-два GU-вызова на кадр, без текстур. */
typedef enum {
    VIZ_SPECTRUM = 0,   /* классический анализатор: столбики зелёный->красный + «колпачки» */
    VIZ_SCOPE,          /* осциллограф: светящаяся линия формы волны */
    VIZ_RADIAL          /* круг: лучи от центра по спектру, зеркально */
} VizKind;

/* Один шаг сглаживания (вызывать раз в кадр, когда режим активен). */
void viz_update(int playing);

/* Отрисовка в прямоугольнике (x,y,w,h). acc_r/g/b — акцентный цвет обложки. */
void viz_draw(VizKind kind, float x, float y, float w, float h,
              unsigned char acc_r, unsigned char acc_g, unsigned char acc_b);

#endif
