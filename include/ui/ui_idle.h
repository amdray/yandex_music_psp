#ifndef YM_UI_IDLE_H
#define YM_UI_IDLE_H

#include "services/locale.h"

/* Режим бездействия на экране воспроизведения: через N секунд без нажатий
 * обложка и текст плавно тают (полупрозрачно / скрыть) либо обложка
 * растягивается на весь экран и «дышит» поверх свечения. Любая кнопка возвращает
 * интерфейс. Настройки живут в config/idle.cfg. */
typedef enum IdleMode {
    IDLE_OFF = 0,
    IDLE_DIM,      /* полупрозрачно */
    IDLE_HIDE,     /* полностью убрать */
    IDLE_COVER,    /* обложка на весь экран, морфинг с эффектом */
    IDLE_MODE_COUNT
} IdleMode;

void idle_init(void);
int idle_get_mode(void);
void idle_cycle_mode(int dir);          /* dir = +1 / -1 */
LocaleKey idle_mode_name_key(int mode);
int idle_get_delay_s(void);
void idle_cycle_delay(int dir);

void idle_note_activity(void);          /* любая кнопка / смена трека */
/* Вызывать раз за кадр рендера экрана воспроизведения. Возвращает «глубину»
 * бездействия 0..1 (0 — интерфейс как обычно, 1 — полностью в режиме). */
float idle_step(void);

#endif
