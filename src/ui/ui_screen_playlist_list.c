#include "ui/ui_screen_playlist_list.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_layout.h"
#include "ui/ui_screens.h"
#include "services/locale.h"
#include "services/cover_manager.h"
#include "services/library.h"
#include "fonts/text.h"
#include "core/logger.h"
#include "app/app_state.h"
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

// Отслеживаем последний видимый диапазон, чтобы не запрашивать обложки каждый кадр
static int s_last_visible_start = -1;
static int s_last_visible_end = -1;
static int s_last_tab = -1;  // для сброса при смене вкладки
static int s_pending_tab = -1;
static int s_marquee_tab = -1;
static int s_marquee_selected = -1;
static char s_marquee_title[64];
static u64 s_marquee_start_us = 0;

static int playlist_list_visible_slots(void)
{
    UiLayoutWidget items;
    int slots;
    if (ui_layout_get_widget("playlist_list", "items", &items) != 0 ||
        items.step <= 0.0f || items.h <= 0.0f)
        return 0;
    slots = (int)(items.h / items.step);
    return slots < PLAYLIST_VISIBLE_SLOTS ? slots : PLAYLIST_VISIBLE_SLOTS;
}

void ui_screen_playlist_list_on_enter(AppState *state)
{
    s_pending_tab = -1;
    s_marquee_tab = -1;
    s_marquee_selected = -1;
    s_marquee_title[0] = '\0';
    s_marquee_start_us = 0;
    /* Refresh stale data on entry so playlist revisions (the track-cache keys)
       are current — catches an edit made on another device. A load that just
       completed (the gated entry from the menu) is NOT redone. */
    library_playlists_refresh_on_enter(state);
}

/* Navigation-gate hook: kick the active tab's load and report whether the
   list is ready to be shown (the menu waits on this before pushing). */
int ui_screen_playlist_list_content_ready(AppState *state)
{
    PlaylistLoadStatus status;

    if (!state) {
        return -1;
    }
    status = library_playlists_service(state, state->playlist_tab);
    if (status == PLAYLIST_LOAD_READY) {
        return 1;
    }
    if (status == PLAYLIST_LOAD_ERROR) {
        const PlaylistLoadState *load = state->playlist_tab == 0
                                            ? &state->playlists_load
                                            : &state->liked_playlists_load;
        logLine("ui: playlist navigation failed tab=%d generation=%d error=%d\n",
                state->playlist_tab, load->generation, load->error_code);
        logger_flush();
        return -1;
    }
    return 0;
}

void ui_screen_playlist_list_update(AppState *state)
{
    library_playlists_service(state, state->playlist_tab);

    if (s_pending_tab >= 0) {
        PlaylistLoadStatus status = library_playlists_service(state, s_pending_tab);
        if (status == PLAYLIST_LOAD_READY) {
            state->playlist_tab = s_pending_tab;
            s_pending_tab = -1;
            s_last_visible_start = -1;
            s_last_visible_end = -1;
        } else if (status == PLAYLIST_LOAD_ERROR) {
            PlaylistLoadState *load = s_pending_tab == 0
                                          ? &state->playlists_load
                                          : &state->liked_playlists_load;
            logLine("ui: playlist tab failed tab=%d generation=%d error=%d\n",
                    s_pending_tab, load->generation, load->error_code);
            logger_flush();
            load->status = PLAYLIST_LOAD_IDLE;
            s_pending_tab = -1;
        }
    }

    // Если вкладка сменилась — сбросить видимый диапазон
    if (state->playlist_tab != s_last_tab) {
        s_last_visible_start = -1;
        s_last_visible_end = -1;
        s_last_tab = state->playlist_tab;
    }

    // Validate scroll position and update cover manager for the active tab
    PlaylistEntry *active_list = (state->playlist_tab == 0) ? state->playlists : state->liked_playlists;
    int active_count = (state->playlist_tab == 0) ? state->playlist_count : state->liked_playlist_count;
    int *active_scroll = (state->playlist_tab == 0) ? &state->playlist_scroll : &state->liked_playlist_scroll;

    if (active_count > 0) {
        const int max_visible = playlist_list_visible_slots();
        if (*active_scroll < 0) {
            *active_scroll = 0;
        }
        if (*active_scroll > active_count - max_visible) {
            *active_scroll = (active_count > max_visible) ?
                             (active_count - max_visible) : 0;
        }

        // Update visible range for cover manager (updates ref_count)
        int start_idx = *active_scroll;
        int end_idx = start_idx + max_visible;
        if (end_idx > active_count) {
            end_idx = active_count;
        }

        // Update cover manager with visible range (manages ref_count internally)
        cover_manager_set_visible_range(start_idx, end_idx, active_list, active_count);

        // Запрашиваем обложки ТОЛЬКО при изменении видимого диапазона (не каждый кадр!)
        if (start_idx != s_last_visible_start || end_idx != s_last_visible_end) {
            s_last_visible_start = start_idx;
            s_last_visible_end = end_idx;

            // Request covers for visible playlists (highest priority)
            for (int i = start_idx; i < end_idx; i++) {
                const PlaylistEntry *pl = &active_list[i];
                if (pl->cover_uri[0]) {
                    cover_manager_request_cover(COVER_ENTITY_PLAYLIST, pl->playlist_id, pl->cover_uri, COVER_PRIORITY_VISIBLE, NULL);
                }
            }

            // Prefetch next 2 playlists (medium priority)
            for (int i = end_idx; i < end_idx + 2 && i < active_count; i++) {
                const PlaylistEntry *pl = &active_list[i];
                if (pl->cover_uri[0]) {
                    cover_manager_request_cover(COVER_ENTITY_PLAYLIST, pl->playlist_id, pl->cover_uri, COVER_PRIORITY_NEARBY, NULL);
                }
            }
        }
    }
}

void ui_screen_playlist_list_handle_input(AppState *state, const InputState *input)
{
    // L/R: переключение вкладок
    if (input->pressed & PSP_CTRL_LTRIGGER) {
        s_pending_tab = state->playlist_tab == 0 ? -1 : 0;
    }
    if (input->pressed & PSP_CTRL_RTRIGGER) {
        s_pending_tab = state->playlist_tab == 1 ? -1 : 1;
    }

    // Указатели на данные активной вкладки
    PlaylistEntry *active_list = (state->playlist_tab == 0) ? state->playlists : state->liked_playlists;
    int active_count = (state->playlist_tab == 0) ? state->playlist_count : state->liked_playlist_count;
    int *active_selected = (state->playlist_tab == 0) ? &state->playlist_selected : &state->liked_playlist_selected;
    int *active_scroll   = (state->playlist_tab == 0) ? &state->playlist_scroll   : &state->liked_playlist_scroll;

    if (input->pressed & PSP_CTRL_UP) {
        if (*active_selected > 0) {
            (*active_selected)--;
            if (*active_selected < *active_scroll) {
                *active_scroll = *active_selected;
            }
        }
    }
    if (input->pressed & PSP_CTRL_DOWN) {
        if (*active_selected < active_count - 1) {
            (*active_selected)++;
            const int max_visible = playlist_list_visible_slots();
            if (*active_selected >= *active_scroll + max_visible) {
                *active_scroll = *active_selected - max_visible + 1;
            }
        }
    }
    if (input->pressed & PSP_CTRL_CROSS && active_count > 0) {
        if (*active_selected < 0 || *active_selected >= active_count) {
            logLine("ui: playlist open rejected tab=%d index=%d count=%d\n",
                    state->playlist_tab, *active_selected, active_count);
            logger_flush();
            return;
        }
        int rc = library_open_playlist(state, &active_list[*active_selected],
                                       state->playlist_tab, *active_selected);
        if (rc == 0) {
            ui_screens_navigate(state, SCREEN_TRACK_LIST);
        } else {
            logLine("ui: playlist open start failed playlist=%d error=%d\n",
                    active_list[*active_selected].playlist_id,
                    state->track_boot.error_code);
            logger_flush();
            app_state_cancel_track_bootstrap(state);
        }
    }
}

static void format_playlist_info(char *buffer, size_t buffer_size,
                                 const PlaylistEntry *playlist, int tab)
{
    char detail[96];
    size_t used;

    if (!buffer || buffer_size == 0 || !playlist) {
        return;
    }
    if (playlist->track_count >= 0) {
        snprintf(buffer, buffer_size, "%d %s", playlist->track_count,
                 locale_get(LOCALE_PLAYLIST_TRACKS));
    } else {
        snprintf(buffer, buffer_size, "-");
    }

    detail[0] = '\0';
    if (tab == 0 && strlen(playlist->modified_date) == 10) {
        snprintf(detail, sizeof(detail), "%s %c%c.%c%c.%c%c",
                 locale_get(LOCALE_PLAYLIST_MODIFIED),
                 playlist->modified_date[8], playlist->modified_date[9],
                 playlist->modified_date[5], playlist->modified_date[6],
                 playlist->modified_date[2], playlist->modified_date[3]);
    } else if (tab == 1 && playlist->owner_name[0]) {
        snprintf(detail, sizeof(detail), "%s", playlist->owner_name);
    }
    if (!detail[0]) {
        return;
    }
    used = strlen(buffer);
    if (used < buffer_size) {
        snprintf(buffer + used, buffer_size - used,
                 " \xE2\x80\xA2 %s", detail);
    }
}

typedef struct PlaylistListLayoutContext {
    const AppState *state;
    const PlaylistEntry *list;
    int count;
    int selected;
    int scroll;
    PlaylistLoadStatus status;
    int tab;
    int opening_tracks;
    u32 opening_color;
    u64 marquee_start_us;
    u64 now_us;
} PlaylistListLayoutContext;

static void playlist_list_items_widget(UiLayoutWidget *items)
{
    memset(items, 0, sizeof(*items));
    ui_layout_get_widget("playlist_list", "items", items);
}

static void playlist_list_tabs_widget(UiLayoutWidget *tabs)
{
    memset(tabs, 0, sizeof(*tabs));
    ui_layout_get_widget("playlist_list", "tabs", tabs);
}

static void playlist_list_tab_widget(int tab_index, UiLayoutWidget *tab)
{
    UiLayoutWidget tabs;
    UiLayoutWidget first;
    const char *label;

    memset(tab, 0, sizeof(*tab));
    ui_layout_get_widget("playlist_list",
                         tab_index == 0 ? "tab_my" : "tab_liked", tab);
    label = locale_get(tab_index == 0 ? LOCALE_TAB_MY_PLAYLISTS
                                      : LOCALE_TAB_LIKED_PLAYLISTS);
    tab->w = text_measure_width(label);
    if (tab_index == 0) {
        return;
    }

    playlist_list_tabs_widget(&tabs);
    memset(&first, 0, sizeof(first));
    ui_layout_get_widget("playlist_list", "tab_my", &first);
    tab->x += first.x + text_measure_width(
        locale_get(LOCALE_TAB_MY_PLAYLISTS)) +
        tabs.step;
}

static int playlist_list_layout_rows_visible(
    const PlaylistListLayoutContext *ctx)
{
    return ctx->status != PLAYLIST_LOAD_ERROR && ctx->count > 0;
}

static void playlist_list_layout_slot(const UiLayoutWidget *widget,
                                      void *user_data)
{
    PlaylistListLayoutContext *ctx = (PlaylistListLayoutContext *)user_data;
    UiLayoutWidget items;
    float row_step;
    int start_idx;
    int end_idx;
    int i;

    if (strcmp(widget->binding, "playlist_list.tab_my") == 0 ||
        strcmp(widget->binding, "playlist_list.tab_liked") == 0) {
        UiLayoutWidget tabs;
        int tab = strcmp(widget->binding, "playlist_list.tab_my") == 0 ? 0 : 1;
        UiLayoutWidget tab_geometry;
        const char *label = locale_get(tab == 0 ? LOCALE_TAB_MY_PLAYLISTS
                                                : LOCALE_TAB_LIKED_PLAYLISTS);
        u32 color = s_pending_tab == tab
                        ? ui_common_pulse_color()
                        : (ctx->state->playlist_tab == tab
                               ? widget->color : 0xFF888888);
        playlist_list_tabs_widget(&tabs);
        playlist_list_tab_widget(tab, &tab_geometry);
        ui_draw_text(tabs.x + tab_geometry.x,
                     tabs.y + tab_geometry.y, label, color);
        return;
    }

    if (strcmp(widget->binding, "playlist_list.tab_indicator") == 0) {
        UiLayoutWidget tabs;
        UiLayoutWidget tab;
        playlist_list_tabs_widget(&tabs);
        playlist_list_tab_widget(ctx->state->playlist_tab, &tab);
        ui_draw_rect(tabs.x + tab.x + widget->x,
                     tabs.y + tab.y + widget->y,
                     widget->w, widget->h, widget->color);
        return;
    }

    if (strcmp(widget->binding, "playlist_list.tabs") == 0 ||
        strcmp(widget->binding, "playlist_list.items") == 0 ||
        !playlist_list_layout_rows_visible(ctx)) {
        return;
    }

    playlist_list_items_widget(&items);
    row_step = items.step;
    start_idx = ctx->scroll;
    end_idx = start_idx + playlist_list_visible_slots();
    if (end_idx > ctx->count) {
        end_idx = ctx->count;
    }

    if (strcmp(widget->binding, "playlist_list.item_selection") == 0) {
        if (ctx->selected >= start_idx && ctx->selected < end_idx) {
            float row_y = items.y + (float)(ctx->selected - start_idx) * row_step;
            ui_draw_rect(items.x + widget->x, row_y + widget->y,
                         widget->w, widget->h, widget->color);
        }
        return;
    }

    for (i = start_idx; i < end_idx; i++) {
        const PlaylistEntry *playlist = &ctx->list[i];
        float row_y = items.y + (float)(i - start_idx) * row_step;
        float x = items.x + widget->x;
        float y = row_y + widget->y;

        if (strcmp(widget->binding, "playlist_list.item_covers") == 0) {
            if (!cover_manager_draw_cover(COVER_ENTITY_PLAYLIST,
                                          playlist->playlist_id, NULL,
                                          (int)x, (int)y,
                                          (int)widget->w, (int)widget->h) &&
                cover_manager_is_loading(COVER_ENTITY_PLAYLIST,
                                         playlist->playlist_id, NULL)) {
                ui_draw_rect(x, y, widget->w, widget->h, widget->color);
            }
        } else if (strcmp(widget->binding,
                          "playlist_list.item_titles") == 0) {
            u32 color = i == ctx->selected ? widget->color : 0xFFBBBBBB;
            if (ctx->opening_tracks &&
                i == ctx->state->track_ui.pending_playlist_selected_index) {
                color = ctx->opening_color;
            }
            if (i == ctx->selected) {
                ui_common_draw_marquee(x, y, widget->w,
                                        playlist->title, color,
                                        NULL, color,
                                        ctx->marquee_start_us, ctx->now_us);
            } else {
                text_render_clipped(x, y, playlist->title, color, widget->w);
            }
        } else if (strcmp(widget->binding,
                          "playlist_list.item_info") == 0) {
            char info[128];
            format_playlist_info(info, sizeof(info), playlist, ctx->tab);
            text_render_clipped(x, y, info, widget->color, widget->w);
        }
    }
}

void ui_screen_playlist_list_render(const AppState *state)
{
    {
        PlaylistListLayoutContext context;
        context.state = state;
        context.list = state->playlist_tab == 0
                           ? state->playlists : state->liked_playlists;
        context.count = state->playlist_tab == 0
                            ? state->playlist_count
                            : state->liked_playlist_count;
        context.selected = state->playlist_tab == 0
                               ? state->playlist_selected
                               : state->liked_playlist_selected;
        context.scroll = state->playlist_tab == 0
                             ? state->playlist_scroll
                             : state->liked_playlist_scroll;
        context.status = state->playlist_tab == 0
                             ? state->playlists_load.status
                             : state->liked_playlists_load.status;
        context.tab = state->playlist_tab;
        context.opening_tracks =
            ui_screens_nav_pending() == SCREEN_TRACK_LIST;
        context.opening_color = context.opening_tracks
                                    ? ui_common_pulse_color() : 0xFFFFFFFF;
        if (context.selected >= 0 && context.selected < context.count) {
            const char *title = context.list[context.selected].title;
            if (s_marquee_tab != state->playlist_tab ||
                s_marquee_selected != context.selected ||
                strcmp(s_marquee_title, title) != 0) {
                s_marquee_tab = state->playlist_tab;
                s_marquee_selected = context.selected;
                snprintf(s_marquee_title, sizeof(s_marquee_title), "%s", title);
                s_marquee_start_us = state->ui_now_us;
            }
        } else {
            s_marquee_tab = -1;
            s_marquee_selected = -1;
            s_marquee_title[0] = '\0';
            s_marquee_start_us = state->ui_now_us;
        }
        context.marquee_start_us = s_marquee_start_us;
        context.now_us = state->ui_now_us;
        (void)ui_layout_render("playlist_list", NULL,
                               playlist_list_layout_slot, &context);
        ui_common_draw_header(locale_get(LOCALE_SCREEN_PLAYLIST_LIST));
    }
}
