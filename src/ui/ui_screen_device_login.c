#include "ui/ui_screen_device_login.h"

#include <pspkernel.h>
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "services/locale.h"
#include "core/logger.h"
#include "core/fs.h"
#include "services/ya_auth.h"
#include "services/qrcodegen.h"
#include "services/net_client.h"
#include "services/net_tls.h"

// Состояние экрана (как g_dc/g_login_* в main.c psp_yandex).
static ya_device_codes s_dc;
static char s_device_id[32];
static char s_msg[128];       // текст ошибки (как g_login_msg)
static volatile int s_codes_ready = 0;
static volatile int s_result = -2;   // -2 идёт, 0 ок, -1 ошибка
static volatile int s_cancel = 0;
static volatile int s_applied = 0;
static volatile int s_tries = 0;
static volatile int s_lastrc = 0;
static volatile unsigned long long s_codes_us = 0;  // когда получены коды
static UserInfo s_user;
static SceUID s_tid = -1;

// QR ссылки для телефона (альтернатива ручному вводу адреса).
// Кодируется один раз при получении кодов, рисуется из s_qr_data.
#define QR_MAX_VERSION 10
static uint8_t s_qr_tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static uint8_t s_qr_data[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static int s_qr_size = 0;  // 0 = нет кода
static char s_qr_text[160];  // полный URL для QR (страница + код)

static void login_qr_encode(const char *text)
{
    s_qr_size = 0;
    if (!text || !text[0]) {
        return;
    }
    if (!qrcodegen_encodeText(text, s_qr_tmp, s_qr_data,
                              qrcodegen_Ecc_MEDIUM, 1, QR_MAX_VERSION,
                              qrcodegen_Mask_AUTO, true)) {
        logLine("login: qr encode failed\n");
        logger_flush();
        return;
    }
    s_qr_size = qrcodegen_getSize(s_qr_data);
    logLine("login: qr size=%d\n", s_qr_size);
    logger_flush();
}
static void login_draw_qr(void)
{
    int r, c;
    const int border = 4;
    const int scale = 2;
    const float y0 = 48.0f;
    float box;
    float x0;

    if (s_qr_size <= 0) {
        return;
    }
    box = (float)((s_qr_size + border * 2) * scale);
    x0 = 472.0f - box;
    ui_draw_rect(x0, y0, box, box, 0xFFFFFFFF);
    for (r = 0; r < s_qr_size; r++) {
        for (c = 0; c < s_qr_size; c++) {
            if (qrcodegen_getModule(s_qr_data, c, r)) {
                ui_draw_rect(x0 + (float)((border + c) * scale),
                             y0 + (float)((border + r) * scale),
                             (float)scale, (float)scale, 0xFF000000);
            }
        }
    }
}

// Сон ломтиками, чтобы выход с экрана не ждал полный interval.
static void login_sleep_s(int seconds)
{
    int i;
    for (i = 0; i < seconds * 5 && !s_cancel; i++) {
        sceKernelDelayThread(200000);
    }
}

// Секунд с получения кодов (по часам — тикает каждый кадр, как у нас).
static int login_elapsed_s(void)
{
    unsigned long long now;
    if (!s_codes_ready || s_codes_us == 0) {
        return 0;
    }
    now = sceKernelGetSystemTimeWide();
    if (now < s_codes_us) {
        return 0;
    }
    return (int)((now - s_codes_us) / 1000000ULL);
}

// Сохранить токен в формате token_loader (YANDEX_TOKEN = "...").
// Пишем через fs-слой (он резолвит путь в ms0:/... сам): сырые sceIo*
// fs_write_atomic с относительным путём под ARK не видят каталог игры —
// так и падала запись. Прямая TRUNC-запись, как логи net_client
// (сохранение account_status_response.json на этом же железе — ок).
static int login_save_token(const char *token)
{
    char buf[320];
    int len;
    SceUID fd;
    int w;

    if (!token || !token[0]) {
        return -1;
    }
    len = snprintf(buf, sizeof(buf), "YANDEX_TOKEN = \"%s\"\n", token);
    if (len <= 0 || len >= (int)sizeof(buf)) {
        return -1;
    }
    fd = fs_open("config/token.txt",
                 PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        logLine("login: save open failed 0x%08X\n", fd);
        logger_flush();
        return -1;
    }
    w = fs_write(fd, buf, (size_t)len);
    fs_close(fd);
    if (w != len) {
        logLine("login: save short %d/%d\n", w, len);
        logger_flush();
        return -1;
    }
    logLine("login: token saved len=%d\n", len);
    logger_flush();
    return 0;
}

static int login_worker(SceSize args, void *argp)
{
    char tok[256];
    int r;
    int interval;

    (void)args;
    (void)argp;
    logLine("login: worker start\n");
    net_tls_cancel_bind((volatile int *)&s_cancel);
    if (!s_device_id[0]) {
        ya_device_id(s_device_id, sizeof(s_device_id));
    }

    // Шаг 1: запрос кодов (как do_login_begin).
    s_codes_ready = 0;
    s_qr_size = 0;
    r = ya_device_begin(s_device_id, &s_dc);
    if (r != 0) {
        snprintf(s_msg, sizeof(s_msg), "Не вышло (%d) %s", r, s_dc.err);
        s_result = -1;
        logLine("login: begin failed %d\n", r);
        logger_flush();
        net_tls_cancel_unbind();
        return 0;
    }
    s_codes_ready = 1;
    s_codes_us = sceKernelGetSystemTimeWide();
    /* QR открывает страницу ввода кода. Полный URL с кодом (?user_code=)
     * Яндекс игнорирует (проверено на железе) — сервер не отдаёт
     * verification_uri_complete, поэтому префилла нет. */
    snprintf(s_qr_text, sizeof(s_qr_text), "%s", s_dc.verify_url);
    logLine("login: qr url=%s\n", s_qr_text);
    login_qr_encode(s_qr_text);
    logLine("login: code ready, polling\n");
    logger_flush();

    // Шаг 2: опрос раз в interval (как login_poll_tick).
    interval = s_dc.interval;
    if (interval <= 0) {
        interval = 5;
    }
    for (;;) {
        if (s_cancel) {
            break;
        }
        login_sleep_s(interval);
        if (s_cancel) {
            break;
        }
        // Коды протухли — дальше сервер скажет expired_token.
        if (s_dc.expires_in > 0 && login_elapsed_s() >= s_dc.expires_in) {
            snprintf(s_msg, sizeof(s_msg), "Время вышло, повтори.");
            s_result = -1;
            break;
        }
        s_tries++;
        r = ya_device_poll(s_dc.device_code, tok, sizeof(tok));
        s_lastrc = r;
        if (r == 1) {
            int frc;
            // Проверка токена (как ya_init) и сохранение.
            frc = net_client_fetch_user_info(tok, &s_user);
            logLine("login: user info rc=%d\n", frc);
            if (frc != 0) {
                snprintf(s_msg, sizeof(s_msg), "Ошибка сети. (%d)", r);
                s_result = -1;
                break;
            }
            if (login_save_token(tok) != 0) {
                snprintf(s_msg, sizeof(s_msg), "Ошибка записи. (%d)", r);
                s_result = -1;
                break;
            }
            s_result = 0;
            logLine("login: ok\n");
            logger_flush();
            net_tls_cancel_unbind();
            return 0;
        }
        if (r < 0) {
            // Как в нашем main.c: -2 = отклонён/протух, иначе сеть.
            if (r == -2) {
                snprintf(s_msg, sizeof(s_msg),
                         "Код отклонён/протух. Начни заново. (%d)", r);
            } else {
                snprintf(s_msg, sizeof(s_msg), "Ошибка сети. (%d)", r);
            }
            s_result = -1;
            break;
        }
        // r == 0: ещё не подтвердили — ждём дальше.
    }

    net_tls_cancel_unbind();
    if (s_cancel) {
        logLine("login: cancelled\n");
    } else if (s_result != 0) {
        s_result = -1;
        logLine("login: failed\n");
    }
    logger_flush();
    return 0;
}

// Дождаться воркера (ограниченно): TLS-ожидания прерываются cancel-флагом
// за сотни мс. Живой тред не удаляем: on_enter подождёт его снова.
static void login_join_worker(void)
{
    SceUInt timeout_us;

    if (s_tid < 0) {
        return;
    }
    timeout_us = 8000000U;
    if (sceKernelWaitThreadEnd(s_tid, &timeout_us) == 0) {
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
    }
}

static void login_start_worker(void)
{
    s_cancel = 1;
    login_join_worker();
    if (s_tid >= 0) {
        return;  // старый воркер ещё висит — новый не плодим
    }
    s_cancel = 0;
    s_result = -2;
    s_applied = 0;
    s_codes_ready = 0;
    s_qr_size = 0;
    s_tries = 0;
    s_lastrc = 0;
    s_codes_us = 0;
    s_msg[0] = '\0';
    memset(&s_dc, 0, sizeof(s_dc));
    memset(&s_user, 0, sizeof(s_user));
    s_tid = sceKernelCreateThread("login_worker", login_worker,
                                  0x18, 64 * 1024, 0, NULL);
    if (s_tid < 0) {
        logLine("login: create thread failed 0x%08X\n", s_tid);
        snprintf(s_msg, sizeof(s_msg), "Нет потока. (%d)", (int)s_tid);
        s_result = -1;
        return;
    }
    if (sceKernelStartThread(s_tid, 0, NULL) < 0) {
        logLine("login: start thread failed\n");
        sceKernelDeleteThread(s_tid);
        s_tid = -1;
        snprintf(s_msg, sizeof(s_msg), "Нет потока.");
        s_result = -1;
        return;
    }
    logLine("login: worker started\n");
}

void ui_screen_device_login_on_enter(AppState *state)
{
    (void)state;
    logLine("login: enter\n");
    login_start_worker();
}

void ui_screen_device_login_on_exit(AppState *state)
{
    (void)state;
    logLine("login: exit\n");
    s_cancel = 1;
    login_join_worker();
}

void ui_screen_device_login_update(AppState *state)
{
    if (s_result == 0 && !s_applied && state) {
        memcpy(&state->currentUser, &s_user, sizeof(s_user));
        s_applied = 1;
        logLine("login: profile applied -> menu\n");
        logger_flush();
        app_state_reset(state, SCREEN_MENU);
    }
}

void ui_screen_device_login_handle_input(AppState *state, const InputState *input)
{
    (void)state;
    if (input->pressed & PSP_CTRL_CROSS) {
        logLine("login: retry requested\n");
        login_start_worker();
    }
}

void ui_screen_device_login_render(const AppState *state)
{
    char line[192];
    int left = 0;

    (void)state;
    ui_draw_clear(0xFF1A1A1A);
    ui_common_draw_header(locale_get(LOCALE_LOGIN_TITLE));

    if (s_result == 0) {
        ui_draw_text(16.0f, 60.0f, locale_get(LOCALE_LOGIN_OK), 0xFF00FF00);
    } else if (!s_codes_ready && s_result == -2) {
        ui_draw_text(16.0f, 60.0f, locale_get(LOCALE_LOGIN_REQUEST), 0xFFFFFF00);
    } else if (s_codes_ready) {
        if (s_dc.expires_in > 0) {
            left = s_dc.expires_in - login_elapsed_s();
            if (left < 0) {
                left = 0;
            }
        }
        ui_draw_text(16.0f, 52.0f, locale_get(LOCALE_LOGIN_OPEN), 0xFFFFFFFF);
        snprintf(line, sizeof(line), "%.60s", s_dc.verify_url);
        ui_draw_text(16.0f, 68.0f, line, 0xFF00FF00);
        ui_draw_text(16.0f, 92.0f, locale_get(LOCALE_LOGIN_ENTER_CODE), 0xFFFFFFFF);
        snprintf(line, sizeof(line), "%.20s", s_dc.user_code);
        ui_draw_text(100.0f, 116.0f, line, 0xFFFFFF00);
        snprintf(line, sizeof(line), locale_get(LOCALE_LOGIN_LEFT),
                 left, s_tries);
        ui_draw_text(16.0f, 150.0f, line, 0xFFAAAAAA);
        login_draw_qr();
    }
    if (s_msg[0]) {
        ui_draw_text(16.0f, 170.0f, s_msg, 0xFFFF4444);
    }

    ui_common_draw_prompts(LOCALE_LOGIN_RETRY_PROMPT, LOCALE_MENU_BACK_PROMPT);
}
