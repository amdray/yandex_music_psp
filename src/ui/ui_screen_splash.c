#include "ui/ui_screen_splash.h"
#include "ui/ui_draw.h"
#include "ui/ui_layout.h"
#include "hal/hal_gfx_config.h"
#include "hal/hal_gpu.h"
#include "services/locale.h"
#include "app/app_state.h"
#include <pspctrl.h>
#include <pspgu.h>

void ui_screen_splash_handle_input(AppState *state, const InputState *input, SplashFlow *flow)
{
    (void)state;
    // If there's an error, allow retry with X button
    if (splash_flow_has_error(flow)) {
        if (input->pressed & PSP_CTRL_CROSS) {
            splash_flow_retry(flow);
        }
    }
}

static void splash_layout_slot(const UiLayoutWidget *widget, void *user_data)
{
    const SplashFlow *flow = (const SplashFlow *)user_data;

    if (widget == &ui_layout_slot_splash__logo) {
        int x;
        int y;
        int src_stride;
        if (!flow || !flow->image_pixels || flow->image_w <= 0 ||
            flow->image_h <= 0 ||
            flow->image_stride_bytes < flow->image_w * 4) {
            return;
        }
        x = (int)(widget->x + (widget->w - flow->image_w) * 0.5f);
        y = (int)(widget->y + (widget->h - flow->image_h) * 0.5f);
        if (x < 0 || y < 0 || x + flow->image_w > SCREEN_WIDTH ||
            y + flow->image_h > SCREEN_HEIGHT) {
            return;
        }
        void *dst = hal_gpu_get_draw_buffer_cpu();
        int stride = VRAM_BUFFER_WIDTH;
        src_stride = flow->image_stride_bytes / 4;

        hal_gpu_flush_cache_range(flow->image_pixels,
                                  (unsigned int)(flow->image_stride_bytes *
                                                 flow->image_h));
        hal_gpu_copy_image(GU_PSM_8888, 0, 0, flow->image_w, flow->image_h,
                           src_stride, flow->image_pixels, x, y, stride, dst);
    } else if (widget == &ui_layout_slot_splash__status) {
        const char *status = flow && flow->status
                                 ? flow->status
                                 : locale_get(LOCALE_SPLASH_INITIALIZING);
        ui_draw_text(widget->x, widget->y, status, widget->color);
    }
}

void ui_screen_splash_render(const SplashFlow *flow)
{
    ui_layout_render(&ui_layout_screen_splash, splash_layout_slot, (void *)flow);
}
