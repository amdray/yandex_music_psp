#include "services/image_loader.h"

#include <malloc.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>

#include "core/fs.h"
#include "core/logger.h"
#include "psptypes.h"
#include <pspkernel.h>
#include <limits.h>

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
    char message[JMSG_LENGTH_MAX];
} JpegError;

typedef struct {
    struct jpeg_decompress_struct cinfo;
    JpegError error;
    u8 *jpeg_buf;
    size_t jpeg_size;
    u8 *rgba_buf;
    u8 *row_buffer;
    int create_started;
    int result;
} JpegDecodeContext;

static int image_next_pow2(int v)
{
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

static size_t image_safe_mul(size_t a, size_t b, const char *context)
{
    if (b != 0 && a > SIZE_MAX / b) {
        logLine("image: size overflow %s (%zu * %zu)\n", context, a, b);
        return 0;
    }
    return a * b;
}

static void jpeg_error_exit(j_common_ptr cinfo)
{
    JpegError *err = (JpegError *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->setjmp_buffer, 1);
}

static int jpeg_decode_context_cleanup(JpegDecodeContext *ctx)
{
    int result;

    if (!ctx) return -1;
    result = ctx->result;
    if (ctx->create_started) {
        jpeg_destroy_decompress(&ctx->cinfo);
    }
    free(ctx->row_buffer);
    free(ctx->rgba_buf);
    free(ctx->jpeg_buf);
    free(ctx);
    return result;
}

static int read_file_to_buffer_from_fd(SceUID fd, u8 **out_buf, size_t *out_size)
{
    if (fd < 0 || !out_buf || !out_size) {
        return -1;
    }

    // Get current position (after header read)
    SceOff current_pos = fs_lseek(fd, 0, PSP_SEEK_CUR);
    SceOff file_size = fs_lseek(fd, 0, PSP_SEEK_END);
    fs_lseek(fd, current_pos, PSP_SEEK_SET);
    
    size_t remaining_size = (size_t)(file_size - current_pos);
    if (remaining_size == 0 || remaining_size > 10 * 1024 * 1024) {
        logLine("image: invalid file size %zu\n", remaining_size);
        return -1;
    }

    u8 *buffer = (u8 *)malloc(remaining_size);
    if (!buffer) {
        int maxFree = sceKernelMaxFreeMemSize();
        logLine("image: alloc failed %zu bytes (max_free=%d)\n", 
                remaining_size, maxFree);
        return -1;
    }

    int read_size = fs_read(fd, buffer, remaining_size);
    if (read_size != (int)remaining_size) {
        logLine("image: read failed %d != %zu\n", read_size, remaining_size);
        free(buffer);
        return -1;
    }

    *out_buf = buffer;
    *out_size = remaining_size;
    return 0;
}

static int load_jpeg_rgba(SceUID fd, const char *path, void **out_data, int *out_w, int *out_h, int *out_stride_bytes)
{
    JpegDecodeContext *ctx;
    int width;
    int height;
    int aligned_w;
    int aligned_h;
    size_t aligned_row_bytes;
    size_t rgba_size;
    size_t row_stride;

    if (fd < 0) {
        return -1;
    }

    ctx = (JpegDecodeContext *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        logLine("image: jpeg context alloc failed\n");
        return -1;
    }
    ctx->result = -1;

    // Reset to beginning (header was already read)
    fs_lseek(fd, 0, PSP_SEEK_SET);

    if (read_file_to_buffer_from_fd(fd, &ctx->jpeg_buf, &ctx->jpeg_size) != 0) {
        goto cleanup;
    }

    ctx->cinfo.err = jpeg_std_error(&ctx->error.pub);
    ctx->error.pub.error_exit = jpeg_error_exit;

    if (setjmp(ctx->error.setjmp_buffer)) {
        ctx->result = -1;
        if (ctx->jpeg_size >= 8) {
            if (ctx->jpeg_buf[0] == 0x89 && ctx->jpeg_buf[1] == 0x50 &&
                ctx->jpeg_buf[2] == 0x4E && ctx->jpeg_buf[3] == 0x47) {
                logLine("image: jpeg decode error - file is PNG, not JPEG (file='%s')\n", path);
            } else if (ctx->jpeg_buf[0] != 0xFF || ctx->jpeg_buf[1] != 0xD8) {
                logLine("image: jpeg decode error - invalid signature (file='%s', got %02X %02X)\n",
                        path, ctx->jpeg_buf[0], ctx->jpeg_buf[1]);
            } else {
                logLine("image: jpeg decode error (file='%s', size=%zu, reason='%s')\n",
                        path, ctx->jpeg_size, ctx->error.message);
            }
        } else {
            logLine("image: jpeg decode error (file='%s', size=%zu, reason='%s')\n",
                    path, ctx->jpeg_size, ctx->error.message);
        }
        goto cleanup;
    }

    /* jpeg_create_decompress may invoke error_exit before it returns. Mark the
     * attempt first; libjpeg initializes cinfo.mem before its first allocation,
     * so destroy is valid for a partially-created decoder after longjmp. */
    ctx->create_started = 1;
    jpeg_create_decompress(&ctx->cinfo);
    jpeg_mem_src(&ctx->cinfo, ctx->jpeg_buf, ctx->jpeg_size);
    if (jpeg_read_header(&ctx->cinfo, TRUE) != JPEG_HEADER_OK) {
        logLine("image: jpeg header failed\n");
        goto cleanup;
    }

    ctx->cinfo.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&ctx->cinfo)) {
        logLine("image: jpeg start failed\n");
        goto cleanup;
    }

    width = ctx->cinfo.output_width;
    height = ctx->cinfo.output_height;
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) {
        logLine("image: jpeg invalid size %dx%d\n", width, height);
        goto cleanup;
    }

    // Align width to 8 pixels (32 bytes) for PSP GU stride requirement (must be multiple of 8)
    // This prevents pixel shift issues when copying to framebuffer
    // Align height to next power-of-2: sceGuTexImage receives next_pow2(cover->h) in ui_draw.c
    // so the buffer must be large enough for that many rows
    aligned_w = ((width + 7) & ~7);
    aligned_h = image_next_pow2(height);
    aligned_row_bytes = (size_t)aligned_w * 4;
    rgba_size = image_safe_mul(aligned_row_bytes, aligned_h, "rgba buffer");
    if (rgba_size == 0) {
        logLine("image: rgba alloc size overflow\n");
        goto cleanup;
    }
    ctx->rgba_buf = (u8 *)memalign(16, rgba_size);
    if (!ctx->rgba_buf) {
        int maxFree = sceKernelMaxFreeMemSize();
        logLine("image: jpeg rgba alloc failed %dx%d (%zu bytes, max_free=%d)\n", 
                aligned_w, aligned_h, rgba_size, maxFree);
        goto cleanup;
    }
    memset(ctx->rgba_buf, 0, rgba_size);

    row_stride = image_safe_mul((size_t)ctx->cinfo.output_width,
                                ctx->cinfo.output_components,
                                "jpeg row stride");
    if (row_stride == 0) {
        logLine("image: invalid row stride\n");
        goto cleanup;
    }
    ctx->row_buffer = (u8 *)malloc(row_stride);
    if (!ctx->row_buffer) {
        logLine("image: row buffer alloc failed\n");
        goto cleanup;
    }

    while (ctx->cinfo.output_scanline < ctx->cinfo.output_height) {
        JSAMPROW row_ptr = ctx->row_buffer;
        const size_t decoded_row_bytes = (size_t)width * 4;
        if (jpeg_read_scanlines(&ctx->cinfo, &row_ptr, 1) != 1) {
            logLine("image: jpeg scanline failed\n");
            goto cleanup;
        }

        int y = ctx->cinfo.output_scanline - 1;
        u8 *dst = ctx->rgba_buf + (size_t)y * aligned_row_bytes;
        for (int x = 0; x < width; ++x) {
            size_t src_idx = (size_t)x * 3;
            size_t dst_idx = (size_t)x * 4;
            dst[dst_idx + 0] = ctx->row_buffer[src_idx + 0];
            dst[dst_idx + 1] = ctx->row_buffer[src_idx + 1];
            dst[dst_idx + 2] = ctx->row_buffer[src_idx + 2];
            dst[dst_idx + 3] = 0xFF;
        }
        if (aligned_row_bytes > decoded_row_bytes) {
            memset(dst + decoded_row_bytes, 0, aligned_row_bytes - decoded_row_bytes);
        }
    }

    jpeg_finish_decompress(&ctx->cinfo);

    // Log memory after large image decode (>= 200x200)
    if (width >= 200 || height >= 200) {
        int maxFree = sceKernelMaxFreeMemSize();
        logLine("mem: after jpeg decode %dx%d max_free=%d\n", width, height, maxFree);
    }

    *out_data = ctx->rgba_buf;
    ctx->rgba_buf = NULL;
    *out_w = width;
    *out_h = height;
    if (out_stride_bytes) {
        *out_stride_bytes = (int)aligned_row_bytes;
    }
    ctx->result = 0;

cleanup:
    return jpeg_decode_context_cleanup(ctx);
}

static void png_read_fn(png_structp png_ptr, png_bytep data, png_size_t length)
{
    void *io_ptr = png_get_io_ptr(png_ptr);
    SceUID *fd_ptr = (SceUID *)io_ptr;
    if (*fd_ptr >= 0) {
        int read_size = fs_read(*fd_ptr, data, (size_t)length);
        if (read_size != (int)length) {
            png_error(png_ptr, "Read error");
        }
    }
}

static int load_png_rgba(SceUID fd, const char *path, void **out_data, int *out_w, int *out_h, int *out_stride_bytes)
{
    (void)path;  // Unused, kept for consistency with load_jpeg_rgba signature
    if (fd < 0) {
        return -1;
    }

    // Reset to beginning (header was already read)
    fs_lseek(fd, 0, PSP_SEEK_SET);

    // Read PNG signature (8 bytes) - already read in detect, but verify
    u8 sig[8];
    int read_size = fs_read(fd, sig, 8);
    if (read_size != 8 || !png_check_sig(sig, 8)) {
        logLine("image: png invalid signature\n");
        return -1;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        logLine("image: png create read struct failed\n");
        return -1;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        logLine("image: png create info struct failed\n");
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return -1;
    }

    volatile u8 *rgba_buf_v = NULL;
    volatile png_bytep *row_pointers_v = NULL;

    if (setjmp(png_jmpbuf(png_ptr))) {
        logLine("image: png decode error\n");
        free((void *)row_pointers_v);
        free((void *)rgba_buf_v);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }

    png_set_read_fn(png_ptr, &fd, png_read_fn);
    png_set_sig_bytes(png_ptr, 8);
    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) {
        logLine("image: png invalid size %dx%d\n", width, height);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }

    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    // Convert to RGBA8888
    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY) {
        png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);
    }

    png_read_update_info(png_ptr, info_ptr);

    // Align width to 8 pixels (32 bytes) for PSP GU stride requirement
    // Align height to next power-of-2: sceGuTexImage receives next_pow2(cover->h) in ui_draw.c
    // so the buffer must be large enough for that many rows
    int aligned_w = ((width + 7) & ~7);
    int aligned_h = image_next_pow2(height);
    const size_t aligned_row_bytes = (size_t)aligned_w * 4;
    u8 *rgba_buf = (u8 *)memalign(16, aligned_row_bytes * aligned_h);
    if (!rgba_buf) {
        int maxFree = sceKernelMaxFreeMemSize();
        size_t needed = aligned_row_bytes * aligned_h;
        logLine("image: png rgba alloc failed %dx%d (%zu bytes, max_free=%d)\n", 
                aligned_w, aligned_h, needed, maxFree);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }
    rgba_buf_v = rgba_buf;
    memset(rgba_buf, 0, aligned_row_bytes * aligned_h);

    size_t rows_bytes = image_safe_mul(sizeof(png_bytep), (size_t)height, "png row pointers");
    if (rows_bytes == 0) {
        logLine("image: png row pointer count overflow\n");
        free(rgba_buf);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }
    png_bytep *row_pointers = (png_bytep *)malloc(rows_bytes);
    if (!row_pointers) {
        logLine("image: png row pointers alloc failed\n");
        free(rgba_buf);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }
    row_pointers_v = row_pointers;

    // Set row pointers to point to our buffer
    for (int y = 0; y < height; y++) {
        row_pointers[y] = rgba_buf + (size_t)y * aligned_row_bytes;
    }

    png_read_image(png_ptr, row_pointers);
    png_read_end(png_ptr, NULL);

    free(row_pointers);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    // Log memory after large image decode (>= 200x200)
    if (width >= 200 || height >= 200) {
        int maxFree = sceKernelMaxFreeMemSize();
        logLine("mem: after png decode %dx%d max_free=%d\n", width, height, maxFree);
    }

    *out_data = rgba_buf;
    *out_w = width;
    *out_h = height;
    if (out_stride_bytes) {
        *out_stride_bytes = (int)aligned_row_bytes;
    }
    return 0;
}

static int detect_image_format_from_header(const u8 *header, size_t header_size)
{
    if (!header || header_size < 8) {
        return -1;
    }

    // Check PNG signature: 89 50 4E 47 0D 0A 1A 0A
    if (header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) {
        return 1;  // PNG
    }

    // Check JPEG signature: FF D8
    if (header[0] == 0xFF && header[1] == 0xD8) {
        return 0;  // JPEG
    }

    return -1;  // Unknown
}

int image_load_rgba8888(const char *path, void **out_data, int *out_w, int *out_h, int *out_stride_bytes)
{
    if (!path || !out_data || !out_w || !out_h) {
        return -1;
    }

    SceUID fd = fs_open(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        logLine("image: open failed '%s'\n", path);
        return -1;
    }

    // Read header for format detection
    u8 header[8];
    int read_size = fs_read(fd, header, sizeof(header));
    if (read_size < 8) {
        logLine("image: read header failed '%s'\n", path);
        fs_close(fd);
        return -1;
    }

    // Detect format by file signature (more reliable than extension)
    int format = detect_image_format_from_header(header, sizeof(header));
    int result = -1;

    if (format == 1) {
        result = load_png_rgba(fd, path, out_data, out_w, out_h, out_stride_bytes);
    } else if (format == 0) {
        result = load_jpeg_rgba(fd, path, out_data, out_w, out_h, out_stride_bytes);
    } else {
        logLine("image: unsupported format '%s'\n", path);
    }

    fs_close(fd);
    return result;
}

void image_free_rgba8888(void *data)
{
    free(data);
}
