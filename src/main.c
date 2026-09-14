#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspmodulemgr.h>
#include <psploadexec.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <pspsysmem.h>
#include <string.h>
#include <stdio.h>

#include "app/app_state.h"
#include "hal/hal_input.h"
#include "hal/hal_gpu.h"
#include "fonts/text.h"
#include "ui/ui_draw.h"
#include "ui/ui_screens.h"
#include "services/image_loader.h"
#include "services/cover_manager.h"
#include "services/eq.h"
#include "services/system_status.h"
#include "services/playback_controller.h"
#include "services/audio_player.h"
#include "services/net_client.h"
#include "core/logger.h"
#include "core/fs.h"
#include "core/clock.h"
#include "services/systemctrl_rng.h"

#define LOGGER_PATH "data/logs/app.log"

#define LOG_INIT() \
    do { \
        logger_init(LOGGER_PATH); \
        logLine("app: start\n"); \
        logLine("app: BUILD " __DATE__ " " __TIME__ "\n"); \
        logger_flush(); \
    } while (0)
#define LOG_FLUSH() logger_flush()
#define LOG_SHUTDOWN() logger_shutdown()

#ifndef EXIT_STAGE
#define EXIT_STAGE 0
#endif

// AppState (~60 KB with reduced arrays), use static allocation
static AppState s_app_state;

// Ready flags for graceful shutdown
static int hal_gpu_ready = 0;
static int text_ready = 0;
static int ui_draw_ready = 0;
static int ui_screens_ready = 0;
static int hal_input_ready = 0;

typedef enum {
    EXIT_STAGE_NONE = 0,
    EXIT_STAGE_AFTER_LOGGER = 1,
    EXIT_STAGE_AFTER_FS = 2,
    EXIT_STAGE_AFTER_CLOCK = 3,
} ExitStage;

static int exit_on_stage(ExitStage stage)
{
    if (EXIT_STAGE == stage) {
        sceKernelExitGame();
        return 1;
    }
    return 0;
}

static void log_data_paths(void)
{
    char log_path[512];
    if (fs_make_abs_path("data/logs/app.log", log_path, sizeof(log_path)) == 0) {
        logLine("app: log path '%s'\n", log_path);
    } else {
        logLine("app: log path 'data/logs/app.log'\n");
    }

    char cwd[256];
    if (fs_get_cwd(cwd, sizeof(cwd)) == 0) {
        logLine("app: cwd abs '%s'\n", cwd);
    } else {
        logLine("app: cwd unavailable\n");
    }
}

/* Exit callback - called when user presses HOME button */
static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    /* Workers may still be running. Process exit reclaims the intact graph. */
    sceKernelExitGame();
    return 0;
}

/* Флаг событий питания (сон/пробуждение/переключатель). Ставит колбэк,
// разбирает главный цикл: там можно писать в лог. */
static volatile int s_power_event = 0;

static void power_callback(int unknown, int power_info)
{
    (void)unknown;
    s_power_event = power_info;
}

/* Callback thread - handles system callbacks */
static int callback_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    if (cbid < 0) {
        logLine("app: failed to create exit callback: %d\n", cbid);
        return -1;
    }
    if (sceKernelRegisterExitCallback(cbid) < 0) {
        logLine("app: failed to register exit callback\n");
        return -1;
    }
    logLine("app: exit callback registered\n");
    {
        int pcbid = sceKernelCreateCallback("Power Callback",
                                            (void *)power_callback, NULL);
        if (pcbid >= 0) {
            int slot = scePowerRegisterCallback(-1, pcbid);
            logLine("app: power callback slot=%d\n", slot);
        } else {
            logLine("app: failed to create power callback: %d\n", pcbid);
        }
    }
    sceKernelSleepThreadCB();
    return 0;
}

static int setup_callbacks(void)
{
    int thid = sceKernelCreateThread("callback_thread", callback_thread, 0x11, 0x10000, 0, NULL);
    if (thid < 0) {
        logLine("app: failed to create callback thread: %d\n", thid);
        return -1;
    }
    if (sceKernelStartThread(thid, 0, NULL) < 0) {
        logLine("app: failed to start callback thread\n");
        return -1;
    }
    logLine("app: callback thread started\n");
    return thid;
}

PSP_MODULE_INFO("YMPSP", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(2048);  // 2MB stack - increased from 512KB for large JSON parsing (875 tracks)
PSP_HEAP_SIZE_KB(16384);  // 16MB heap - increased from 8MB for cJSON (149k allocations, ~4MB peak)

int main(int argc, char *argv[])
{
    int exit_code = 1;
    u64 last_log_flush_us = 0;

    // Set CWD from argv[0] - required for relative paths to work
    if (argc > 0 && argv[0]) {
        char cwd[256];
        strncpy(cwd, argv[0], sizeof(cwd) - 1);
        cwd[sizeof(cwd) - 1] = '\0';
        char *last_slash = strrchr(cwd, '/');
        if (last_slash) {
            *last_slash = '\0';
            sceIoChdir(cwd);
        }
    }

    // Initialize fs module
    if (fs_init() != 0) {
        return 1;
    }

    /* Эквалайзер: читает config/eq.cfg (нет файла = Off). */
    eq_init();

    LOG_INIT();
    if (exit_on_stage(EXIT_STAGE_AFTER_LOGGER)) {
        return 0;
    }

    /* Verify the same ARK SystemCtrlForUser RNG used by TLS. */
    systemctrl_rng_probe();
    logger_flush();

    /* Setup system callbacks (HOME button support) - early to handle exit during initialization */
    if (setup_callbacks() < 0) {
        logLine("app: warning: failed to setup callbacks, HOME button may not work\n");
    }

    logLine("app: init - log_data_paths\n");
    log_data_paths();
    if (exit_on_stage(EXIT_STAGE_AFTER_FS)) {
        return 0;
    }

    logLine("app: init - clock_init\n");
    // Initialize system time (required for TLS/SSL certificate validation)
    if (clock_init() < 0) {
        logLine("app: FATAL - RTC check failed, TLS/SSL will not work\n");
        logLine("app: Please check:\n");
        logLine("app:   1. PSP battery (RTC battery may be dead)\n");
        logLine("app:   2. Date/Time in PSP settings (must be >= 2024)\n");
        goto cleanup;
    }
    logLine("app: system time initialized\n");
    if (exit_on_stage(EXIT_STAGE_AFTER_CLOCK)) {
        return 0;
    }

    logLine("app: init - hal_gpu_init\n");
    LOG_FLUSH();  // Flush before GPU init to see progress
    if (hal_gpu_init() < 0) {
        logLine("app: GPU initialization failed\n");
        goto cleanup;
    }
    hal_gpu_ready = 1;
    logLine("app: GPU initialized successfully\n");
    LOG_FLUSH();  // Flush after successful GPU init

    logLine("app: init - text_init\n");
    if (text_init() < 0) {
        logLine("app: text initialization failed (font not found)\n");
        goto cleanup;
    }
    text_ready = 1;
    logLine("app: text initialized successfully\n");

    logLine("app: init - ui_draw_init\n");
    if (ui_draw_init() < 0) {
        logLine("app: UI draw initialization failed\n");
        goto cleanup;
    }
    ui_draw_ready = 1;

    logLine("app: init - ui_screens_init\n");
    if (ui_screens_init() < 0) {
        logLine("app: UI screens initialization failed\n");
        goto cleanup;
    }
    ui_screens_ready = 1;
    logLine("app: UI screens initialized successfully\n");
    LOG_FLUSH();  // Flush after successful UI init

    logLine("app: init - hal_input_init\n");
    if (hal_input_init() < 0) {
        logLine("app: input initialization failed\n");
        goto cleanup;
    }
    hal_input_ready = 1;

    logLine("app: init - app_state_init\n");
    if (app_state_init(&s_app_state) < 0) {
        logLine("app: app state initialization failed\n");
        goto cleanup;
    }
    exit_code = 0;

    // Log memory state after all initialization (using safe function)
    logLine("app: entering main loop\n");
    logger_flush();
    u64 last_net_poll_us = 0;
    int wd_pos = -1;
    u64 wd_t0 = 0;
    int wd_stage = 0;
    while (1) {
        InputState input;
        u64 now_us;
        AudioPlayerSnapshot snap;
        int have_snap;

        hal_input_poll(&input);

        /* События питания (сон/пробуждение/переключатель): сразу
         * перепроверяем сеть, а не ждём секундного тика. */
        if (s_power_event) {
            int ev = s_power_event;
            s_power_event = 0;
            logLine("app: power event 0x%08X, net recheck\n", ev);
            logger_flush();
            net_client_poll();
        }

        /* Кнопка ♪ (NOTE): переключение профилей эквалайзера везде. */
        if (input.pressed & PSP_CTRL_NOTE) {
            int p = eq_next_preset();
            eq_save();
            logLine("app: NOTE -> eq preset %d\n", p);
        }

        /* Громкость поверх системного максимума: VOL+ на 30/30 растит
         * предусиление (+2 дБ, до +12), VOL- сначала сливает его.
         * (ОС свои шаги тоже делает — это её сторону не отменяет.) */
        if (input.pressed & (PSP_CTRL_VOLUP | PSP_CTRL_VOLDOWN)) {
            SystemStatusSnapshot vs;
            system_status_get_snapshot(&vs);
            if (input.pressed & PSP_CTRL_VOLUP) {
                if (vs.volume_available && vs.volume_level >= 30 &&
                    eq_get_preamp_db() < EQ_PREAMP_MAX_DB) {
                    eq_set_preamp_db(eq_get_preamp_db() + EQ_PREAMP_STEP_DB);
                    eq_save();
                }
            } else if (eq_get_preamp_db() > 0.0f) {
                eq_set_preamp_db(eq_get_preamp_db() - EQ_PREAMP_STEP_DB);
                eq_save();
            }
        }

        ui_screens_update(&s_app_state, &input);
        if (net_client_requires_process_exit()) {
            sceKernelExitGame();
            return 1;
        }
        ui_screens_handle_input(&s_app_state, &input);
        ui_screens_render(&s_app_state);
        playback_controller_service();

        now_us = sceKernelGetSystemTimeWide();

        /* Keep APCTL state current after the splash screen. Previously it was
         * polled only during startup, so a later network transition was
         * invisible and net_client_is_ready() remained stale. */
        if (last_net_poll_us == 0 || now_us - last_net_poll_us >= 1000000ULL) {
            net_client_poll();
            last_net_poll_us = now_us;
        }

        /* Watchdog: PLAYING, но позиция стоит — движок подвис (сон экрана,
         * умерший ME/поток). L1: перепроверка сети. L2: перезапуск трека. */
        have_snap = audio_player_get_snapshot(&snap);
        if (have_snap && snap.state == AUDIO_PLAYER_PLAYING &&
            snap.duration_ms > 0 &&
            snap.position_ms < snap.duration_ms - 5000) {
            if (snap.position_ms != wd_pos) {
                wd_pos = snap.position_ms;
                wd_t0 = now_us;
                wd_stage = 0;
            } else if (wd_stage == 0 && now_us - wd_t0 >= 8000000ULL) {
                wd_stage = 1;
                logLine("app: watchdog stall L1 net recheck pos=%d\n",
                        snap.position_ms);
                logger_flush();
                net_client_poll();
            } else if (wd_stage == 1 && now_us - wd_t0 >= 16000000ULL) {
                wd_stage = 2;
                logLine("app: watchdog stall L2 restart pos=%d\n",
                        snap.position_ms);
                logger_flush();
                playback_controller_play_current();
            }
        } else {
            wd_pos = have_snap ? snap.position_ms : -1;
            wd_t0 = now_us;
            wd_stage = 0;
        }

        /* Memory Stick writes can block for 50-180 ms. Never flush while the
         * audio path is real-time; retain logs in RAM and flush afterwards. */
        AudioPlayerState audio_state = audio_player_get_state();
        int realtime = audio_state == AUDIO_PLAYER_OPENING ||
                       audio_state == AUDIO_PLAYER_PLAYING ||
                       audio_state == AUDIO_PLAYER_BUFFERING ||
                       audio_state == AUDIO_PLAYER_STOPPING;
        logger_set_realtime_mode(realtime);
        if (!realtime &&
            (last_log_flush_us == 0 || (now_us - last_log_flush_us) >= 250000ULL)) {
            logger_flush();
            last_log_flush_us = now_us;
        }

        // Browser cover uploads are processed only in browsing mode.
        if (app_state_get_resource_mode(&s_app_state) == APP_MODE_BROWSING) {
            cover_manager_process_pending();
        }
    }

cleanup:
    if (ui_screens_ready) {
        /* ui_screens_shutdown is join-only. Whether every join succeeds or not,
         * no application/network destructor runs in this process. */
        (void)ui_screens_shutdown();
        sceKernelExitGame();
        return exit_code;
    }
    if (hal_input_ready) {
        hal_input_shutdown();
    }
    app_state_shutdown(&s_app_state);
    if (ui_draw_ready) {
        ui_draw_shutdown();
    }
    if (text_ready) {
        text_shutdown();
    }
    if (hal_gpu_ready) {
        hal_gpu_shutdown();
    }
    logLine("app: shutdown\n");
    LOG_FLUSH();
    LOG_SHUTDOWN();
    fs_shutdown();
    sceKernelExitGame();
    return exit_code;
}
