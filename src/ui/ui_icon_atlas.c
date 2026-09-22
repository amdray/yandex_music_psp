#include "ui/ui_icon_atlas.h"

#include <pspgu.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"
#include "hal/hal_gpu.h"

#define UI_ICON_ATLAS_MAGIC_0 'Y'
#define UI_ICON_ATLAS_MAGIC_1 'M'
#define UI_ICON_ATLAS_MAGIC_2 'I'
#define UI_ICON_ATLAS_MAGIC_3 'A'
#define UI_ICON_ATLAS_VERSION 2
#define UI_ICON_ATLAS_WIDTH 128
#define UI_ICON_ATLAS_HEIGHT 64
#define UI_ICON_ATLAS_ENTRY_SIZE 24
#define UI_ICON_ATLAS_MAX_ENTRIES 32
#define UI_ICON_ATLAS_NAME_SIZE 16
#define UI_ICON_ATLAS_PIXELS_SIZE (UI_ICON_ATLAS_WIDTH * UI_ICON_ATLAS_HEIGHT / 2)

typedef struct UiIconEntry {
    char name[UI_ICON_ATLAS_NAME_SIZE];
    unsigned char x;
    unsigned char y;
    unsigned char width;
    unsigned char height;
} UiIconEntry;

typedef struct UiIconVertex {
    unsigned short u;
    unsigned short v;
    u32 color;
    short x;
    short y;
    short z;
} UiIconVertex;

static unsigned char s_pixels[UI_ICON_ATLAS_PIXELS_SIZE]
    __attribute__((aligned(16)));
static u32 s_clut[16] __attribute__((aligned(16)));
static UiIconEntry s_entries[UI_ICON_ATLAS_MAX_ENTRIES];
static int s_entry_count;
static int s_ready;

static unsigned int read_u16_le(const unsigned char *bytes)
{
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
}

static int read_exact(SceUID fd, void *buffer, unsigned int size)
{
    unsigned char *out = (unsigned char *)buffer;
    unsigned int total = 0;
    while (total < size) {
        int count = fs_read(fd, out + total, size - total);
        if (count <= 0) return -1;
        total += (unsigned int)count;
    }
    return 0;
}

static const UiIconEntry *find_entry(const char *name)
{
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < s_entry_count; ++i) {
        if (strcmp(s_entries[i].name, name) == 0) return &s_entries[i];
    }
    return NULL;
}

int ui_icon_atlas_init(const char *path)
{
    unsigned char header[16];
    unsigned char entry_bytes[UI_ICON_ATLAS_ENTRY_SIZE];
    unsigned int version;
    unsigned int width;
    unsigned int height;
    unsigned int entry_count;
    unsigned int entry_size;
    unsigned int data_offset;
    SceUID fd;
    unsigned int i;
    unsigned char trailing;

    ui_icon_atlas_shutdown();
    if (!path || !path[0]) return -1;
    fd = fs_open(path, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        logLine("ui_icons: open failed path='%s' rc=0x%08X\n", path, fd);
        return -1;
    }
    if (read_exact(fd, header, sizeof(header)) != 0) goto invalid;
    if (header[0] != UI_ICON_ATLAS_MAGIC_0 ||
        header[1] != UI_ICON_ATLAS_MAGIC_1 ||
        header[2] != UI_ICON_ATLAS_MAGIC_2 ||
        header[3] != UI_ICON_ATLAS_MAGIC_3) goto invalid;

    version = read_u16_le(header + 4);
    width = read_u16_le(header + 6);
    height = read_u16_le(header + 8);
    entry_count = read_u16_le(header + 10);
    entry_size = read_u16_le(header + 12);
    data_offset = read_u16_le(header + 14);
    if (version != UI_ICON_ATLAS_VERSION ||
        width != UI_ICON_ATLAS_WIDTH || height != UI_ICON_ATLAS_HEIGHT ||
        entry_count == 0 || entry_count > UI_ICON_ATLAS_MAX_ENTRIES ||
        entry_size != UI_ICON_ATLAS_ENTRY_SIZE ||
        data_offset != sizeof(header) + entry_count * entry_size) goto invalid;

    for (i = 0; i < entry_count; ++i) {
        UiIconEntry *entry = &s_entries[i];
        unsigned int j;
        if (read_exact(fd, entry_bytes, sizeof(entry_bytes)) != 0) goto invalid;
        entry->x = entry_bytes[1];
        entry->y = entry_bytes[2];
        entry->width = entry_bytes[3];
        entry->height = entry_bytes[4];
        memcpy(entry->name, entry_bytes + 8, UI_ICON_ATLAS_NAME_SIZE);
        entry->name[UI_ICON_ATLAS_NAME_SIZE - 1] = '\0';
        if (!entry->name[0] || !entry->width || !entry->height ||
            (unsigned int)entry->x + entry->width > width ||
            (unsigned int)entry->y + entry->height > height) goto invalid;
        for (j = 0; j < i; ++j) {
            if (strcmp(entry->name, s_entries[j].name) == 0) goto invalid;
        }
    }
    if (read_exact(fd, s_pixels, sizeof(s_pixels)) != 0) goto invalid;
    if (fs_read(fd, &trailing, 1) != 0) goto invalid;
    fs_close(fd);

    for (i = 0; i < 16; ++i) {
        s_clut[i] = (i * 17U << 24) | 0x00FFFFFFU;
    }
    hal_gpu_flush_cache_range(s_pixels, sizeof(s_pixels));
    hal_gpu_flush_cache_range(s_clut, sizeof(s_clut));
    s_entry_count = (int)entry_count;
    s_ready = 1;
    logLine("ui_icons: loaded path='%s' entries=%u size=%ux%u\n",
            path, entry_count, width, height);
    return 0;

invalid:
    fs_close(fd);
    ui_icon_atlas_shutdown();
    logLine("ui_icons: invalid atlas path='%s'\n", path);
    return -1;
}

void ui_icon_atlas_shutdown(void)
{
    s_ready = 0;
    s_entry_count = 0;
}

int ui_icon_atlas_get_size(const char *name, int *out_width, int *out_height)
{
    const UiIconEntry *entry;
    if (!s_ready || !out_width || !out_height) return -1;
    entry = find_entry(name);
    if (!entry) return -1;
    *out_width = entry->width;
    *out_height = entry->height;
    return 0;
}

int ui_icon_atlas_draw(const char *name, int x, int y, u32 color_abgr)
{
    const UiIconEntry *entry;
    UiIconVertex *vertices;

    if (!s_ready || !hal_gpu_in_frame()) return -1;
    entry = find_entry(name);
    if (!entry) return -1;

    hal_gpu_set_texturing(1);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuClutMode(GU_PSM_8888, 0, 0x0F, 0);
    sceGuClutLoad(2, s_clut);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexImage(0, UI_ICON_ATLAS_WIDTH, UI_ICON_ATLAS_HEIGHT,
                  UI_ICON_ATLAS_WIDTH, s_pixels);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);

    vertices = (UiIconVertex *)sceGuGetMemory(2 * sizeof(*vertices));
    vertices[0].u = entry->x;
    vertices[0].v = entry->y;
    vertices[0].color = color_abgr;
    vertices[0].x = (short)x;
    vertices[0].y = (short)y;
    vertices[0].z = 0;
    vertices[1].u = entry->x + entry->width;
    vertices[1].v = entry->y + entry->height;
    vertices[1].color = color_abgr;
    vertices[1].x = (short)(x + entry->width);
    vertices[1].y = (short)(y + entry->height);
    vertices[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                       GU_TRANSFORM_2D,
                   2, NULL, vertices);

    sceGuDisable(GU_ALPHA_TEST);
    hal_gpu_set_texturing(0);
    return 0;
}
