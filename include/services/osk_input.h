#ifndef YM_SERVICES_OSK_INPUT_H
#define YM_SERVICES_OSK_INPUT_H

// Блокирующий ввод текста через системную OSK (sceUtilityOsk*).
// Самодостаточен: зависит только от SDK + hal_gpu (проверка кадра),
// никаких зависимостей от app/UI.
// Русская клавиатура по умолчанию, выход — UTF-8.
// Возврат: 0 = ок (out — UTF-8, всегда NUL-терминирован),
//         -2 = отмена/без изменений, -1 = ошибка вызова.
int osk_input_text(const char *title, const char *initial,
                   char *out, int out_size);

#endif
