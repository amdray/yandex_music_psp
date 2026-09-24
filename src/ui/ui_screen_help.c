#include "ui/ui_screen_help.h"

#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_icon_atlas.h"
#include "ui/ui_layout.h"
#include "services/locale.h"
#include "fonts/text.h"

#include <string.h>

typedef struct HelpRow {
    const char *button_text;
    const char *icon;
    const char *second_icon;
    LocaleKey description;
} HelpRow;

static const HelpRow s_help_rows[] = {
    { NULL,     "b_cross",     NULL,           LOCALE_HELP_SELECT },
    { NULL,     "b_circle",    NULL,           LOCALE_HELP_BACK },
    { "START",  NULL,          NULL,           LOCALE_HELP_PLAY_PAUSE },
    { NULL,     "b_square",    NULL,           LOCALE_HELP_STOP },
    { NULL,     "b_triangle",  NULL,           LOCALE_HELP_LIKE },
    { NULL,     "in_left_key", "in_right_key", LOCALE_HELP_SEEK },
    { NULL,     "in_music",    NULL,           LOCALE_HELP_NOTE },
};

static int button_icon_size(const char *name, int *width, int *height)
{
    return ui_icon_atlas_get_size(name, width, height);
}

static void draw_button_icon(const char *name, int x, int height,
                             float row_y, float row_step, u32 color)
{
    ui_icon_atlas_draw(name, x,
                       (int)(row_y + (row_step - (float)height) / 2.0f),
                       color);
}

static void help_layout_slot(const UiLayoutWidget *widget, void *user_data)
{
    float step = widget->step;
    unsigned i;
    (void)user_data;
    if (strcmp(widget->binding, "help.buttons") == 0) {
        for (i = 0; i < sizeof(s_help_rows) / sizeof(s_help_rows[0]); i++) {
            const HelpRow *row = &s_help_rows[i];
            float y = widget->y + (float)i * step;
            if (row->button_text) {
                float width = text_measure_width(row->button_text);
                ui_draw_text(widget->x + (widget->w - width) * 0.5f,
                             y, row->button_text, widget->color);
            } else {
                int first_width;
                int first_height;
                int second_width = 0;
                int second_height = 0;
                int gap = row->second_icon ? 4 : 0;
                int total_width;
                int x;

                if (button_icon_size(row->icon, &first_width,
                                     &first_height) != 0) {
                    continue;
                }
                if (row->second_icon &&
                    button_icon_size(row->second_icon, &second_width,
                                     &second_height) != 0) {
                    continue;
                }
                total_width = first_width + gap + second_width;
                x = (int)(widget->x + (widget->w - (float)total_width) * 0.5f);
                draw_button_icon(row->icon, x, first_height, y, step,
                                 widget->color);
                if (row->second_icon) {
                    draw_button_icon(row->second_icon,
                                     x + first_width + gap, second_height,
                                     y, step, widget->color);
                }
            }
        }
    } else if (strcmp(widget->binding, "help.descriptions") == 0) {
        for (i = 0; i < sizeof(s_help_rows) / sizeof(s_help_rows[0]); i++) {
            ui_draw_text(widget->x, widget->y + (float)i * step,
                         locale_get(s_help_rows[i].description), widget->color);
        }
    }
}

void ui_screen_help_render(const AppState *state)
{
    (void)state;
    (void)ui_layout_render("help", NULL, help_layout_slot, NULL);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_HELP));
}
