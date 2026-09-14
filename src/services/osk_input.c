// Блокирующий ввод через системную OSK, адаптирован из psp_yandex
// (native/ui.c: ui_input) без зависимостей от приложения.
//
// Интеграция с главным циклом: эта функция вызывается из контекста
// update/input-хендлера (как on_enter/handle_input экранов) — т.е. ВНЕ
// sceGu-кадра (hal_gpu_begin/end_frame обрамляют только render).
// Поэтому внутри НЕТ вызовов sceGu: открытый display list был бы
// повреждён диалогом утилиты. Кадр только один:
//   sceGuSync(FINISH)      — дождаться GE до передачи дисплея утилите,
//   sceGuDisplay(GU_FALSE) — отдать экран диалогу,
//   цикл sceDisplayWaitVblankStart + sceUtilityOskUpdate (как в референсе),
//   sceGuDisplay(GU_TRUE)  — вернуть экран приложению,
//   invalidate кэшированного GE-состояния hal (текстуры/ножницы).
// Вызов внутри кадра запрещён: вернёт -1 (проверка hal_gpu_in_frame()).
#include "services/osk_input.h"

#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputility.h>
#include <psputility_osk.h>

#include <string.h>

#include "hal/hal_gpu.h"

#define OSK_INTEXT_MAX 256
#define OSK_DESC_MAX 64

// UTF-16 -> UTF-8 (BMP + склейка суррогатов), как в референсе.
static int osk_u16_to_u8(const unsigned short *in, char *out, int out_sz)
{
    int i = 0, j = 0;

    while (in[i] && j + 4 < out_sz) {
        unsigned cp = in[i++];
        if (cp >= 0xD800 && cp <= 0xDBFF && in[i]) {
            unsigned lo = in[i];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) {
            out[j++] = (char)cp;
        } else if (cp < 0x800) {
            out[j++] = (char)(0xC0 | (cp >> 6));
            out[j++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[j++] = (char)(0xE0 | (cp >> 12));
            out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[j++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[j++] = (char)(0xF0 | (cp >> 18));
            out[j++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[j++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[j] = '\0';
    return j;
}

// UTF-8 -> UTF-16 (упрощённо: латиница + кириллица), как в референсе.
static void osk_u8_to_u16(const char *init, unsigned short *dst, int dst_cap)
{
    int i = 0, j = 0;

    dst[0] = 0;
    if (!init) {
        return;
    }
    while (init[i] && j + 1 < dst_cap) {
        unsigned char c = (unsigned char)init[i];
        if (c < 0x80) {
            dst[j++] = c;
            i++;
        } else if (c == 0xD0 && init[i + 1]) {
            unsigned char c2 = (unsigned char)init[i + 1];
            if (c2 >= 0x90 && c2 <= 0xBF) {
                dst[j++] = (unsigned short)(0x410 + (c2 - 0x90));
            } else if (c2 == 0x81) {
                dst[j++] = 0x401;
            } else {
                dst[j++] = (unsigned short)'?';
            }
            i += 2;
        } else if (c == 0xD1 && init[i + 1]) {
            unsigned char c2 = (unsigned char)init[i + 1];
            if (c2 >= 0x80 && c2 <= 0x8F) {
                dst[j++] = (unsigned short)(0x440 + (c2 - 0x80));
            } else if (c2 == 0x91) {
                dst[j++] = 0x451;
            } else {
                dst[j++] = (unsigned short)'?';
            }
            i += 2;
        } else {
            dst[j++] = (unsigned short)'?';
            i++;
        }
    }
    dst[j] = 0;
}

// Заголовок диалога: только ASCII (кириллица в desc на части прошивок
// рисуется как ???; сам ввод при этом — полный UTF-8).
static void osk_fold_desc(const char *title, unsigned short *dst, int dst_cap)
{
    int i = 0;

    dst[0] = 0;
    if (!title) {
        return;
    }
    while (title[i] && i + 1 < dst_cap) {
        unsigned char c = (unsigned char)title[i];
        dst[i] = (c < 0x80) ? c : (unsigned short)'?';
        i++;
    }
    dst[i] = 0;
}

int osk_input_text(const char *title, const char *initial,
                   char *out, int out_size)
{
    SceUtilityOskData data;
    SceUtilityOskParams params;
    unsigned short intext[OSK_INTEXT_MAX];
    unsigned short outtext[OSK_INTEXT_MAX];
    unsigned short desc[OSK_DESC_MAX];

    if (!out || out_size <= 1) {
        return -1;
    }
    out[0] = '\0';
    // Внутри sceGu-кадра диалог запускать нельзя (см. шапку файла).
    if (hal_gpu_in_frame()) {
        return -1;
    }
    memset(&data, 0, sizeof(data));
    memset(&params, 0, sizeof(params));
    memset(intext, 0, sizeof(intext));
    memset(outtext, 0, sizeof(outtext));
    memset(desc, 0, sizeof(desc));

    osk_u8_to_u16(initial, intext, OSK_INTEXT_MAX - 56);
    osk_fold_desc(title, desc, OSK_DESC_MAX - 4);

    data.language = PSP_UTILITY_OSK_LANGUAGE_RUSSIAN;
    data.lines = 1;
    data.unk_24 = 1;
    data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc = desc;
    data.intext = intext;
    data.outtext = outtext;
    data.outtextlength = OSK_INTEXT_MAX - 1;
    data.outtextlimit = 60;

    params.base.size = sizeof(params);
    params.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    params.base.buttonSwap = 1;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
                               &params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN,
                               &params.base.buttonSwap);
    params.base.graphicsThread = 17;
    params.base.accessThread = 19;
    params.base.fontThread = 18;
    params.base.soundThread = 16;
    params.datacount = 1;
    params.data = &data;

    if (sceUtilityOskInitStart(&params) < 0) {
        return -1;
    }
    // Отдаём дисплей диалогу на время цикла (состояние — как в hal_gpu_init:
    // приложение всегда работает с sceGuDisplay(GU_TRUE)).
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceGuDisplay(GU_FALSE);
    while (sceUtilityOskGetStatus() != PSP_UTILITY_DIALOG_NONE) {
        sceDisplayWaitVblankStart();
        sceUtilityOskUpdate(1);
    }
    sceGuDisplay(GU_TRUE);
    // Утилита могла сменить GE-контекст: сбросить кэшированное состояние hal,
    // чтобы следующий кадр переизлучил его заново.
    hal_gpu_invalidate_tex_state();
    hal_gpu_invalidate_scissor();

    if (data.result != PSP_UTILITY_OSK_RESULT_CHANGED) {
        return -2;
    }
    osk_u16_to_u8(outtext, out, out_size);
    return 0;
}
