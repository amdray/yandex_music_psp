#include "ui/ui_screen_memory.h"

#include <stdio.h>
#include <string.h>
#include <kubridge.h>
#include <iplsdk/model.h>

#include "core/logger.h"
#include "core/mem_probe.h"
#include "services/locale.h"
#include "fonts/text.h"
#include "ui/ui_common.h"
#include "ui/ui_draw.h"
#include "ui/ui_layout.h"

#define MIB                   (1024u * 1024u)
#define SAMPLE_WIDTH          2
#define MAX_RECTS             (MEM_PROBE_HISTORY_CAPACITY * 4)

#define COLOR_HEAP_USED       0xFF4D7DFFu
#define COLOR_HEAP_RESERVE    0xFFB06A32u
#define COLOR_LARGEST_FREE    0xFF55B86Au
#define COLOR_OTHER           UI_COLOR_INACTIVE

typedef struct MemoryScreenContext {
    MemProbeSample samples[MEM_PROBE_HISTORY_CAPACITY];
    int count;
    u32 total_bytes;
} MemoryScreenContext;

static UiDrawColoredRect s_rects[MAX_RECTS];
static u32 s_total_bytes;

void ui_screen_memory_init(void)
{
    int model = kuKernelGetModel();

    if (model == PSP_MODEL_01G) {
        s_total_bytes = 32u * MIB;
    } else if (model >= PSP_MODEL_02G && model <= PSP_MODEL_11G) {
        s_total_bytes = 64u * MIB;
    } else {
        s_total_bytes = 0;
    }
    logLine("mem: model=%d physical=%u MiB\n", model,
            (unsigned int)(s_total_bytes / MIB));
}

static int bytes_to_pixels(u32 bytes, int height, u32 total_bytes)
{
    u32 pixels = (u32)(((u64)bytes * (u32)height) / total_bytes);
    return pixels > (u32)height ? height : (int)pixels;
}

static void append_rect(int *count, int x, int y, int w, int h, u32 color)
{
    UiDrawColoredRect *rect;

    if (h <= 0 || *count >= MAX_RECTS) {
        return;
    }
    rect = &s_rects[(*count)++];
    rect->x = (short)x;
    rect->y = (short)y;
    rect->w = (short)w;
    rect->h = (short)h;
    rect->color = color;
}

static void draw_graph(const UiLayoutWidget *widget,
                       const MemoryScreenContext *context)
{
    char label[8];
    float label_x;
    int graph_h = (int)widget->h;
    int start_x = (int)(widget->x + widget->w) -
                  context->count * SAMPLE_WIDTH;
    int rect_count = 0;
    int i;

    if (context->total_bytes == 0 || graph_h <= 0) return;

    for (i = 0; i < context->count; i++) {
        const MemProbeSample *sample = &context->samples[i];
        u32 heap_cap = sample->heap_cap;
        u32 heap_used = sample->heap_used;
        u32 largest_free = sample->largest_free;
        u32 cumulative;
        int used_y;
        int heap_y;
        int free_y;
        int bottom = (int)(widget->y + widget->h);
        int x = start_x + i * SAMPLE_WIDTH;

        if (heap_cap > context->total_bytes) heap_cap = context->total_bytes;
        if (heap_used > heap_cap) heap_used = heap_cap;
        if (largest_free > context->total_bytes - heap_cap) {
            largest_free = context->total_bytes - heap_cap;
        }

        used_y = bottom - bytes_to_pixels(heap_used, graph_h,
                                          context->total_bytes);
        heap_y = bottom - bytes_to_pixels(heap_cap, graph_h,
                                          context->total_bytes);
        cumulative = heap_cap + largest_free;
        free_y = bottom - bytes_to_pixels(cumulative, graph_h,
                                          context->total_bytes);

        append_rect(&rect_count, x, used_y, SAMPLE_WIDTH,
                    bottom - used_y, COLOR_HEAP_USED);
        append_rect(&rect_count, x, heap_y, SAMPLE_WIDTH,
                    used_y - heap_y, COLOR_HEAP_RESERVE);
        append_rect(&rect_count, x, free_y, SAMPLE_WIDTH,
                    heap_y - free_y, COLOR_LARGEST_FREE);
        append_rect(&rect_count, x, (int)widget->y, SAMPLE_WIDTH,
                    free_y - (int)widget->y, COLOR_OTHER);
    }

    ui_draw_rect(widget->x, widget->y + widget->h / 2.0f,
                 widget->w, 1.0f, UI_COLOR_INACTIVE);
    ui_draw_colored_rects(s_rects, rect_count);

    snprintf(label, sizeof(label), "%u",
             (unsigned int)(context->total_bytes / MIB));
    label_x = widget->x - 5.0f - text_measure_width(label);
    if (label_x < 8.0f) label_x = 8.0f;
    ui_draw_text(label_x, widget->y - 3.0f, label, UI_COLOR_INACTIVE);

    snprintf(label, sizeof(label), "%u",
             (unsigned int)(context->total_bytes / (2u * MIB)));
    label_x = widget->x - 5.0f - text_measure_width(label);
    if (label_x < 8.0f) label_x = 8.0f;
    ui_draw_text(label_x, widget->y + widget->h / 2.0f - 6.0f,
                 label, UI_COLOR_INACTIVE);

    label_x = widget->x - 5.0f - text_measure_width("0");
    if (label_x < 8.0f) label_x = 8.0f;
    ui_draw_text(label_x, widget->y + widget->h - 7.0f,
                 "0", UI_COLOR_INACTIVE);
}

static void draw_legend_row(float x, float y, u32 color,
                            const char *label, u32 bytes)
{
    char text[64];
    unsigned int tenths = (unsigned int)(((u64)bytes * 10ULL) / (1024u * 1024u));

    ui_draw_rect(x, y + 3.0f, 8.0f, 8.0f, color);
    snprintf(text, sizeof(text), "%s %u.%u MiB", label,
             tenths / 10u, tenths % 10u);
    ui_draw_text(x + 12.0f, y, text, UI_COLOR_ACTIVE);
}

static void draw_legend(const UiLayoutWidget *widget,
                        const MemoryScreenContext *context)
{
    const MemProbeSample *sample;
    u32 heap_cap;
    u32 heap_used;
    u32 largest_free;
    u32 other;

    if (context->count <= 0 || context->total_bytes == 0) return;
    sample = &context->samples[context->count - 1];
    heap_cap = sample->heap_cap > context->total_bytes
                   ? context->total_bytes : sample->heap_cap;
    heap_used = sample->heap_used > heap_cap ? heap_cap : sample->heap_used;
    largest_free = sample->largest_free;
    if (largest_free > context->total_bytes - heap_cap) {
        largest_free = context->total_bytes - heap_cap;
    }
    other = context->total_bytes - heap_cap - largest_free;

    draw_legend_row(widget->x, widget->y, COLOR_HEAP_USED,
                    "Heap used", heap_used);
    draw_legend_row(widget->x + 224.0f, widget->y, COLOR_HEAP_RESERVE,
                    "Heap reserve", heap_cap - heap_used);
    draw_legend_row(widget->x, widget->y + 17.0f, COLOR_LARGEST_FREE,
                    "Largest free", largest_free);
    draw_legend_row(widget->x + 224.0f, widget->y + 17.0f, COLOR_OTHER,
                    "Other/fragmented", other);
}

static void memory_layout_slot(const UiLayoutWidget *widget, void *user_data)
{
    MemoryScreenContext *context = (MemoryScreenContext *)user_data;

    if (strcmp(widget->binding, "memory.graph") == 0) {
        draw_graph(widget, context);
    } else if (strcmp(widget->binding, "memory.legend") == 0) {
        draw_legend(widget, context);
    }
}

void ui_screen_memory_render(const AppState *state)
{
    MemoryScreenContext context;

    (void)state;
    context.total_bytes = s_total_bytes;
    context.count = mem_probe_history_copy(context.samples,
                                            MEM_PROBE_HISTORY_CAPACITY);
    (void)ui_layout_render("memory", NULL, memory_layout_slot, &context);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_MEMORY));
    if (context.total_bytes == 0) {
        ui_draw_text(24.0f, 106.0f, "Memory model unavailable",
                     UI_COLOR_ACTIVE);
        return;
    }
}
