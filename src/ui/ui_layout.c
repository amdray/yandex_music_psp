#include "ui/ui_layout.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"
#include "fonts/text.h"
#include "ui/ui_draw.h"

#define UI_LAYOUT_FILE_MAX (32 * 1024)
#define UI_LAYOUT_SCREEN_MAX 24
#define UI_LAYOUT_WIDGET_MAX 384

typedef struct UiLayoutScreen {
    char name[UI_LAYOUT_NAME_MAX];
    u32 background;
    int first_widget;
    int widget_count;
} UiLayoutScreen;

static char s_file[UI_LAYOUT_FILE_MAX + 1];
static UiLayoutScreen s_screens[UI_LAYOUT_SCREEN_MAX];
static UiLayoutWidget s_widgets[UI_LAYOUT_WIDGET_MAX];
static int s_screen_count = 0;
static int s_widget_count = 0;
static int s_loaded = 0;

static char *trim(char *text)
{
    char *end;
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static int copy_name(char *dst, size_t dst_size, const char *src)
{
    size_t n;
    if (!dst || dst_size == 0 || !src || !src[0]) {
        return -1;
    }
    n = strlen(src);
    if (n >= dst_size) {
        return -1;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

static int split_fields(char *text, char **fields, int max_fields)
{
    int count = 0;
    char *start = text;
    char *p = text;
    if (!text || !fields || max_fields <= 0) {
        return 0;
    }
    for (;;) {
        if (*p == '|' || *p == '\0') {
            char delimiter = *p;
            *p = '\0';
            if (count >= max_fields) {
                return -1;
            }
            fields[count++] = trim(start);
            if (delimiter == '\0') {
                break;
            }
            start = p + 1;
        }
        p++;
    }
    return count;
}

static int parse_color(const char *text, u32 *out)
{
    char *end = NULL;
    unsigned long value;
    if (!text || !out) {
        return -1;
    }
    if (text[0] == '#') {
        text++;
    } else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    if (strlen(text) != 8) {
        return -1;
    }
    value = strtoul(text, &end, 16);
    if (!end || *end != '\0') {
        return -1;
    }
    *out = (u32)value;
    return 0;
}

static int parse_widget_type(const char *text, UiLayoutWidgetType *out)
{
    if (strcmp(text, "rect") == 0) {
        *out = UI_LAYOUT_WIDGET_RECT;
    } else if (strcmp(text, "text") == 0) {
        *out = UI_LAYOUT_WIDGET_TEXT;
    } else if (strcmp(text, "slot") == 0) {
        *out = UI_LAYOUT_WIDGET_SLOT;
    } else {
        return -1;
    }
    return 0;
}

static int parse_widget(char *value, UiLayoutScreen *screen, int line_number)
{
    char *f[9];
    int count;
    UiLayoutWidget *widget;
    char *end;

    count = split_fields(value, f, 9);
    if (count < 8 || count > 9 || s_widget_count >= UI_LAYOUT_WIDGET_MAX) {
        logLine("layout: bad widget line=%d fields=%d\n", line_number, count);
        return -1;
    }
    widget = &s_widgets[s_widget_count];
    memset(widget, 0, sizeof(*widget));
    if (copy_name(widget->id, sizeof(widget->id), f[0]) != 0 ||
        parse_widget_type(f[1], &widget->type) != 0 ||
        parse_color(f[6], &widget->color) != 0 ||
        copy_name(widget->binding, sizeof(widget->binding), f[7]) != 0) {
        logLine("layout: invalid widget line=%d\n", line_number);
        return -1;
    }
    widget->x = (float)strtod(f[2], &end);
    if (!end || *end) return -1;
    widget->y = (float)strtod(f[3], &end);
    if (!end || *end) return -1;
    widget->w = (float)strtod(f[4], &end);
    if (!end || *end) return -1;
    widget->h = (float)strtod(f[5], &end);
    if (!end || *end) return -1;
    widget->step = 0.0f;
    if (count == 9 && f[8][0]) {
        widget->step = (float)strtod(f[8], &end);
        if (!end || *end) return -1;
    }
    s_widget_count++;
    screen->widget_count++;
    return 0;
}

static UiLayoutScreen *find_screen(const char *name)
{
    int i;
    for (i = 0; i < s_screen_count; i++) {
        if (strcmp(s_screens[i].name, name) == 0) {
            return &s_screens[i];
        }
    }
    return NULL;
}

typedef struct RequiredWidget {
    const char *screen;
    const char *id;
    UiLayoutWidgetType type;
    const char *binding;
    int requires_step;
} RequiredWidget;

static int validate_required_widgets(void)
{
    static const RequiredWidget required[] = {
        { "status_overlay", "wifi", UI_LAYOUT_WIDGET_SLOT, "status.wifi", 0 },
        { "status_overlay", "playback_state", UI_LAYOUT_WIDGET_SLOT, "status.playback_state", 0 },
        { "status_overlay", "clock", UI_LAYOUT_WIDGET_SLOT, "status.clock", 0 },
        { "status_overlay", "activity", UI_LAYOUT_WIDGET_SLOT, "status.activity", 0 },
        { "status_overlay", "battery_percent", UI_LAYOUT_WIDGET_SLOT, "status.battery_percent", 0 },
        { "status_overlay", "battery", UI_LAYOUT_WIDGET_SLOT, "status.battery", 0 },
        { "screen_header", "title", UI_LAYOUT_WIDGET_SLOT, "screen_header.title", 0 },
        { "screen_header", "title_rule", UI_LAYOUT_WIDGET_RECT, "none", 0 },
        { "menu", "items", UI_LAYOUT_WIDGET_SLOT, "menu.items", 1 },
        { "menu", "selection", UI_LAYOUT_WIDGET_SLOT, "menu.selection", 0 },
        { "help", "rows", UI_LAYOUT_WIDGET_SLOT, "help.rows", 1 },
        { "playlist_list", "tabs", UI_LAYOUT_WIDGET_SLOT, "playlist_list.tabs", 1 },
        { "playlist_list", "tab_my", UI_LAYOUT_WIDGET_SLOT, "playlist_list.tab_my", 0 },
        { "playlist_list", "tab_liked", UI_LAYOUT_WIDGET_SLOT, "playlist_list.tab_liked", 0 },
        { "playlist_list", "tab_indicator", UI_LAYOUT_WIDGET_SLOT, "playlist_list.tab_indicator", 0 },
        { "playlist_list", "items", UI_LAYOUT_WIDGET_SLOT, "playlist_list.items", 1 },
        { "playlist_list", "item_selection", UI_LAYOUT_WIDGET_SLOT, "playlist_list.item_selection", 0 },
        { "playlist_list", "item_covers", UI_LAYOUT_WIDGET_SLOT, "playlist_list.item_covers", 0 },
        { "playlist_list", "item_titles", UI_LAYOUT_WIDGET_SLOT, "playlist_list.item_titles", 0 },
        { "playlist_list", "item_info", UI_LAYOUT_WIDGET_SLOT, "playlist_list.item_info", 0 },
        { "now_playing", "bottom_bar", UI_LAYOUT_WIDGET_RECT, "none", 0 }
    };
    unsigned int r;

    for (r = 0; r < sizeof(required) / sizeof(required[0]); ++r) {
        UiLayoutScreen *screen = find_screen(required[r].screen);
        int i;
        const UiLayoutWidget *widget = NULL;
        if (screen) {
            for (i = 0; i < screen->widget_count; ++i) {
                UiLayoutWidget *candidate = &s_widgets[screen->first_widget + i];
                if (strcmp(candidate->id, required[r].id) == 0) {
                    widget = candidate;
                    break;
                }
            }
        }
        if (!widget || widget->type != required[r].type ||
            strcmp(widget->binding, required[r].binding) != 0 ||
            widget->w < 0.0f || widget->h < 0.0f ||
            (required[r].requires_step && widget->step <= 0.0f)) {
            logLine("layout: required widget invalid screen='%s' id='%s'\n",
                    required[r].screen, required[r].id);
            return -1;
        }
    }
    return 0;
}

int ui_layout_load(const char *path)
{
    SceUID fd;
    int total = 0;
    int line_number = 0;
    char *cursor;
    UiLayoutScreen *screen = NULL;

    ui_layout_unload();
    if (!path || !path[0]) {
        return -1;
    }
    fd = fs_open(path, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        logLine("layout: open failed '%s' rc=0x%08X\n", path, fd);
        return -1;
    }
    while (total < UI_LAYOUT_FILE_MAX) {
        int n = fs_read(fd, s_file + total,
                        (size_t)(UI_LAYOUT_FILE_MAX - total));
        if (n < 0) {
            fs_close(fd);
            ui_layout_unload();
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    fs_close(fd);
    if (total == UI_LAYOUT_FILE_MAX) {
        logLine("layout: file too large '%s'\n", path);
        ui_layout_unload();
        return -1;
    }
    s_file[total] = '\0';
    cursor = s_file;
    while (*cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        char *text;
        line_number++;
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor += strlen(cursor);
        }
        text = trim(line);
        if (!text[0] || text[0] == '#' || text[0] == ';') {
            continue;
        }
        if (text[0] == '[') {
            char *close = strrchr(text, ']');
            if (!close || close[1] != '\0' || s_screen_count >= UI_LAYOUT_SCREEN_MAX) {
                logLine("layout: bad screen line=%d\n", line_number);
                ui_layout_unload();
                return -1;
            }
            *close = '\0';
            if (find_screen(trim(text + 1))) {
                logLine("layout: duplicate screen line=%d\n", line_number);
                ui_layout_unload();
                return -1;
            }
            screen = &s_screens[s_screen_count++];
            memset(screen, 0, sizeof(*screen));
            if (copy_name(screen->name, sizeof(screen->name), trim(text + 1)) != 0) {
                ui_layout_unload();
                return -1;
            }
            screen->background = 0xFF1A1A1A;
            screen->first_widget = s_widget_count;
            continue;
        }
        if (!screen) {
            logLine("layout: property before screen line=%d\n", line_number);
            ui_layout_unload();
            return -1;
        }
        {
            char *equals = strchr(text, '=');
            char *key;
            char *value;
            if (!equals) {
                ui_layout_unload();
                return -1;
            }
            *equals = '\0';
            key = trim(text);
            value = trim(equals + 1);
            if (strcmp(key, "background") == 0) {
                if (parse_color(value, &screen->background) != 0) {
                    ui_layout_unload();
                    return -1;
                }
            } else if (strcmp(key, "widget") == 0) {
                if (parse_widget(value, screen, line_number) != 0) {
                    ui_layout_unload();
                    return -1;
                }
            } else {
                logLine("layout: unknown key '%s' line=%d\n", key, line_number);
                ui_layout_unload();
                return -1;
            }
        }
    }
    if (s_screen_count <= 0) {
        ui_layout_unload();
        return -1;
    }
    if (validate_required_widgets() != 0) {
        ui_layout_unload();
        return -1;
    }
    s_loaded = 1;
    logLine("layout: loaded screens=%d widgets=%d path='%s'\n",
            s_screen_count, s_widget_count, path);
    return 0;
}

void ui_layout_unload(void)
{
    s_screen_count = 0;
    s_widget_count = 0;
    s_loaded = 0;
    s_file[0] = '\0';
    memset(s_screens, 0, sizeof(s_screens));
    memset(s_widgets, 0, sizeof(s_widgets));
}

int ui_layout_is_loaded(void)
{
    return s_loaded;
}

int ui_layout_render(const char *screen_name,
                     UiLayoutTextResolver text_resolver,
                     UiLayoutSlotRenderer slot_renderer,
                     void *user_data)
{
    UiLayoutScreen *screen;
    int i;
    char text[256];
    if (!s_loaded || !screen_name) {
        return -1;
    }
    screen = find_screen(screen_name);
    if (!screen) {
        return -1;
    }
    ui_draw_clear(screen->background);
    for (i = 0; i < screen->widget_count; i++) {
        UiLayoutWidget *widget = &s_widgets[screen->first_widget + i];
        if (widget->type == UI_LAYOUT_WIDGET_RECT) {
            ui_draw_rect(widget->x, widget->y, widget->w, widget->h,
                         widget->color);
        } else if (widget->type == UI_LAYOUT_WIDGET_TEXT && text_resolver) {
            const char *resolved;
            text[0] = '\0';
            resolved = text_resolver(widget->binding, text, sizeof(text),
                                     user_data);
            if (resolved && resolved[0]) {
                if (widget->w > 0.0f) {
                    text_render_clipped(widget->x, widget->y, resolved,
                                        widget->color, widget->w);
                } else {
                    ui_draw_text(widget->x, widget->y, resolved,
                                 widget->color);
                }
            }
        } else if (widget->type == UI_LAYOUT_WIDGET_SLOT && slot_renderer) {
            slot_renderer(widget, user_data);
        }
    }
    return 0;
}

int ui_layout_get_widget(const char *screen_name, const char *widget_id,
                         UiLayoutWidget *out_widget)
{
    UiLayoutScreen *screen;
    int i;
    if (!s_loaded || !screen_name || !widget_id || !out_widget) {
        return -1;
    }
    screen = find_screen(screen_name);
    if (!screen) {
        return -1;
    }
    for (i = 0; i < screen->widget_count; i++) {
        UiLayoutWidget *widget = &s_widgets[screen->first_widget + i];
        if (strcmp(widget->id, widget_id) == 0) {
            memcpy(out_widget, widget, sizeof(*out_widget));
            return 0;
        }
    }
    return -1;
}
