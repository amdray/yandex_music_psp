#include "hal/hal_fb.h"
#include "hal/hal_gfx_config.h"

#include <pspge.h>
#include <stdint.h>

extern void *hal_gpu_get_draw_buffer(void);

void *hal_fb_get_draw_buffer(void)
{
    /* Get uncached address of draw buffer for direct CPU access.
     * According to PSPSDK documentation:
     * - hal_gpu_get_draw_buffer() returns VRAM offset
     * - sceGeEdramGetAddr() returns base VRAM address
     * - For uncached access: (0x40000000 | base_addr) + offset
     */
    void *base = sceGeEdramGetAddr();
    uintptr_t offset = (uintptr_t)hal_gpu_get_draw_buffer();
    uintptr_t base_addr = (uintptr_t)base;
    return (void *)((0x40000000UL | base_addr) + offset);
}

int hal_fb_get_width(void)
{
    return SCREEN_WIDTH;
}

int hal_fb_get_height(void)
{
    return SCREEN_HEIGHT;
}

int hal_fb_get_stride(void)
{
    return VRAM_BUFFER_WIDTH;
}
