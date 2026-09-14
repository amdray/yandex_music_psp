#include "hal/hal_gpu.h"

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <stddef.h>
#include <stdint.h>
#include <malloc.h>

#include "hal/hal_gfx_config.h"
#include "core/logger.h"
#include <stdlib.h>

// Command list for GU: 512 KB is sufficient based on actual usage monitoring
// Maximum observed: 235 KB, so 512 KB provides ~2.2x safety margin
#define GU_CMD_LIST_SIZE (512 * 1024)  // 512 KB; do NOT reduce: smaller values caused GE hangs
static unsigned int *s_list = NULL;  // Dynamically allocated to reduce .bss size
static void *s_draw_buffer = NULL;
static void *s_display_buffer = NULL;
static int s_in_frame = 0;
static int s_texturing_enabled = -1;
static int s_blend_enabled = -1;
static int s_blend_op = -1;
static int s_blend_src = -1;
static int s_blend_dst = -1;
static int s_viewport_width = -1;
static int s_viewport_height = -1;
static int s_scissor_x = -1;
static int s_scissor_y = -1;
static int s_scissor_w = -1;
static int s_scissor_h = -1;

static void *hal_gpu_display_addr_from_offset(void *buffer_offset)
{
    uintptr_t edram_base;
    uintptr_t offset;

    edram_base = (uintptr_t)sceGeEdramGetAddr();
    offset = (uintptr_t)buffer_offset;
    return (void *)(edram_base + offset);
}

static void hal_gpu_present_frame(void)
{
    void *present_buffer;

    present_buffer = s_draw_buffer;
    sceDisplayWaitVblankStart();
    sceDisplaySetFrameBuf(hal_gpu_display_addr_from_offset(present_buffer),
                          VRAM_BUFFER_WIDTH,
                          GU_PSM_8888,
                          PSP_DISPLAY_SETBUF_IMMEDIATE);

    s_draw_buffer = s_display_buffer;
    s_display_buffer = present_buffer;
}

static void hal_gpu_reset_cached_state(void)
{
    s_texturing_enabled = -1;
    s_blend_enabled = -1;
    s_blend_op = -1;
    s_blend_src = -1;
    s_blend_dst = -1;
    s_viewport_width = -1;
    s_viewport_height = -1;
    s_scissor_x = -1;
    s_scissor_y = -1;
    s_scissor_w = -1;
    s_scissor_h = -1;
}

int hal_gpu_init(void)
{
    int ret;

    hal_gpu_reset_cached_state();

    // Allocate command list dynamically (512 KB) with 64-byte alignment
    s_list = (unsigned int *)memalign(64, GU_CMD_LIST_SIZE);
    if (!s_list) {
        logLine("hal_gpu: failed to allocate command list (%d KB)\n", GU_CMD_LIST_SIZE / 1024);
        return -1;
    }

    ret = sceGuInit();
    if (ret < 0) {
        logLine("hal_gpu: sceGuInit failed: %d\n", ret);
        free(s_list);
        s_list = NULL;
        return ret;
    }
    logLine("hal_gpu: sceGuInit OK\n");

    // Allocate framebuffers using standard PSP SDK function
    // Note: guGetStaticVramBuffer() returns VRAM offset (can be 0x0 for first buffer, which is valid)
    s_draw_buffer = guGetStaticVramBuffer(VRAM_BUFFER_WIDTH, SCREEN_HEIGHT, GU_PSM_8888);
    s_display_buffer = guGetStaticVramBuffer(VRAM_BUFFER_WIDTH, SCREEN_HEIGHT, GU_PSM_8888);
    logLine("hal_gpu: buffers allocated: draw=%p display=%p\n", s_draw_buffer, s_display_buffer);
    // No NULL check needed: guGetStaticVramBuffer() always returns valid offset (starts at 0x0)

    sceGuStart(GU_DIRECT, s_list);
    sceGuDrawBuffer(GU_PSM_8888, s_draw_buffer, VRAM_BUFFER_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, s_display_buffer, VRAM_BUFFER_WIDTH);
    hal_gpu_set_viewport(SCREEN_WIDTH, SCREEN_HEIGHT);
    hal_gpu_set_scissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    hal_gpu_set_blend(1, GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA);
    hal_gpu_set_texturing(0);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplaySetFrameBuf(hal_gpu_display_addr_from_offset(s_display_buffer),
                          VRAM_BUFFER_WIDTH,
                          GU_PSM_8888,
                          PSP_DISPLAY_SETBUF_IMMEDIATE);
    sceGuDisplay(GU_TRUE);
    logLine("hal_gpu: initialization complete, explicit framebuffer ownership enabled\n");
    return 0;
}

void hal_gpu_shutdown(void)
{
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
    hal_gpu_reset_cached_state();
    if (s_list) {
        free(s_list);
        s_list = NULL;
        logLine("hal_gpu: command list freed\n");
    }
}

void hal_gpu_begin_frame(void)
{
    if (s_in_frame) {
        // Already in frame, skip
        return;
    }
    if (!s_list) {
        // GPU not initialized
        return;
    }
    // Note: s_draw_buffer is VRAM offset (can be 0x0), not a pointer, so no NULL check needed
    sceGuStart(GU_DIRECT, s_list);
    sceGuDrawBuffer(GU_PSM_8888, s_draw_buffer, VRAM_BUFFER_WIDTH);
    s_in_frame = 1;
}

void hal_gpu_end_frame(void)
{
    if (!s_in_frame) {
        // Not in frame, skip
        return;
    }
    int list_size = sceGuFinish();
    if (list_size > 0) {
        sceKernelDcacheWritebackRange(s_list, list_size);
    }
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    hal_gpu_present_frame();
    s_in_frame = 0;
}

void hal_gpu_clear(u32 color_abgr)
{
    sceGuClearColor(color_abgr);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

void hal_gpu_set_texturing(int enabled)
{
    if (s_texturing_enabled == enabled) {
        return;
    }
    if (enabled) {
        sceGuEnable(GU_TEXTURE_2D);
    } else {
        sceGuDisable(GU_TEXTURE_2D);
    }
    s_texturing_enabled = enabled;
}

void hal_gpu_set_tex_mode(int format)
{
    sceGuTexMode(format, 0, 0, 0);
}

void hal_gpu_set_tex_func(int tfx, int tcc)
{
    sceGuTexFunc(tfx, tcc);
}

void hal_gpu_set_tex_filter(int min_filter, int mag_filter)
{
    sceGuTexFilter(min_filter, mag_filter);
}

void hal_gpu_set_blend(int enabled, int op, int src, int dst)
{
    if (s_blend_enabled == enabled &&
        (!enabled || (s_blend_op == op && s_blend_src == src && s_blend_dst == dst))) {
        return;
    }

    if (enabled) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(op, src, dst, 0, 0);
        s_blend_op = op;
        s_blend_src = src;
        s_blend_dst = dst;
    } else {
        sceGuDisable(GU_BLEND);
        s_blend_op = -1;
        s_blend_src = -1;
        s_blend_dst = -1;
    }
    s_blend_enabled = enabled;
}

void hal_gpu_flush_cache_range(const void *ptr, unsigned int size)
{
    sceKernelDcacheWritebackRange(ptr, size);
}

void hal_gpu_set_scissor(int x, int y, int w, int h)
{
    if (s_scissor_x == x && s_scissor_y == y && s_scissor_w == w && s_scissor_h == h) {
        return;
    }
    sceGuScissor(x, y, x + w, y + h);
    s_scissor_x = x;
    s_scissor_y = y;
    s_scissor_w = w;
    s_scissor_h = h;
}

void hal_gpu_set_viewport(int width, int height)
{
    if (s_viewport_width == width && s_viewport_height == height) {
        return;
    }
    sceGuOffset(2048 - (width / 2), 2048 - (height / 2));
    sceGuViewport(2048, 2048, width, height);
    s_viewport_width = width;
    s_viewport_height = height;
}

int hal_gpu_in_frame(void)
{
    return s_in_frame;
}

void *hal_gpu_get_draw_buffer(void)
{
    return s_draw_buffer;
}

void hal_gpu_invalidate_tex_state(void)
{
    s_texturing_enabled = -1;
}

void hal_gpu_invalidate_scissor(void)
{
    s_scissor_x = -1;
    s_scissor_y = -1;
    s_scissor_w = -1;
    s_scissor_h = -1;
}

void hal_gpu_copy_image(int psm, int sx, int sy, int width, int height, int srcw, const void *src,
                        int dx, int dy, int destw, void *dest)
{
    if (!s_in_frame) {
        return;
    }
    sceGuCopyImage(psm, sx, sy, width, height, srcw, (void *)src, dx, dy, destw, dest);
}
