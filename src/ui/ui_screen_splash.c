#include "ui/ui_screen_splash.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "hal/hal_fb.h"
#include "hal/hal_gpu.h"
#include "services/locale.h"
#include "services/last_play.h"
#include "app/app_state.h"
#include "core/logger.h"
#include <pspctrl.h>
#include <pspgu.h>
#include <stdio.h>

void ui_screen_splash_init(void)
{
    // Initialization handled in ui_screens_init
}

void ui_screen_splash_shutdown(void)
{
    // Cleanup handled in ui_screens_shutdown
}

void ui_screen_splash_update(AppState *state, SplashFlow *flow)
{
    splash_flow_tick(flow, state);
    if (flow->ready) {
        // Было что играть — продолжаем сразу на плеере, иначе в меню.
        if (last_play_restore(state) == 0) {
            logLine("splash: ready -> app_state_reset(NOW_PLAYING) begin\n");
            logger_flush();
            app_state_reset(state, SCREEN_NOW_PLAYING);
            logLine("splash: app_state_reset(NOW_PLAYING) done\n");
            logger_flush();
            return;
        }
        logLine("splash: ready -> app_state_reset(MENU) begin\n");
        logger_flush();
        app_state_reset(state, SCREEN_MENU);
        logLine("splash: app_state_reset(MENU) done\n");
        logger_flush();
    }
}

void ui_screen_splash_handle_input(AppState *state, const InputState *input, SplashFlow *flow)
{
    // If there's an error, allow retry with X button
    if (splash_flow_has_error(flow)) {
        if (input->pressed & PSP_CTRL_CROSS) {
            splash_flow_retry(flow);
        }
    } else if (input->pressed && flow->ready) {
        // If ready, any button goes to menu
        app_state_reset(state, SCREEN_MENU);
    }
}

void ui_screen_splash_render(const SplashFlow *flow, void *splash_pixels, int splash_w, int splash_h)
{
    if (splash_pixels) {
        void *dst = hal_fb_get_draw_buffer();
        int stride = hal_fb_get_stride();
        int width = hal_fb_get_width();
        int height = hal_fb_get_height();
        int copy_w = splash_w < width ? splash_w : width;
        int copy_h = splash_h < height ? splash_h : height;

        hal_gpu_flush_cache_range(splash_pixels, (unsigned int)(splash_w * splash_h * 4));
        hal_gpu_copy_image(GU_PSM_8888, 0, 0, copy_w, copy_h, splash_w, splash_pixels,
                           0, 0, stride, dst);
    } else {
        ui_draw_clear(0xFF1A1A1A);
        ui_common_draw_header("YMPSP");
    }
    if (flow->status) {
        ui_draw_text(16.0f, 230.0f, flow->status, 0xFFFFFFFF);
    } else {
        ui_draw_text(16.0f, 230.0f, locale_get(LOCALE_SPLASH_INITIALIZING), 0xFFFFFFFF);
    }
    if (splash_flow_has_error(flow)) {
        ui_common_draw_prompts(LOCALE_SPLASH_RETRY_PROMPT, LOCALE_SPLASH_EXIT_PROMPT);
    }
}
