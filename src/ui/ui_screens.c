#include "ui/ui_screens.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "core/fs.h"
#include "ui/ui_draw.h"
#include "services/image_loader.h"
#include "core/logger.h"
#include "services/splash_flow.h"
#include "services/audio_cache.h"
#include "services/cover_manager.h"
#include "services/cover_now_playing.h"
#include "services/video_cover.h"
#include "services/net_client.h"
#include "services/net_tls.h"
#include "services/net_stack.h"
#include "services/net_ui_status.h"
#include "services/playback_controller.h"
#include "services/system_status.h"

#include "ui/ui_screen_splash.h"
#include "ui/ui_screen_menu.h"
#include "ui/ui_screen_playlist_list.h"
#include "ui/ui_screen_track_list.h"
#include "ui/ui_screen_account.h"
#include "ui/ui_screen_now_playing.h"
#include "ui/ui_screen_album_list.h"
#include "ui/ui_screen_artist.h"
#include "ui/ui_screen_artist_menu.h"
#include "ui/ui_screen_net_info.h"
#include "ui/ui_screen_device_login.h"
#include "ui/ui_common.h"

static void *s_splash_pixels = NULL;
static int s_splash_w = 0;
static int s_splash_h = 0;
static SplashFlow s_splash_flow;
static ScreenId s_last_screen = SCREEN_COUNT;

/* Screen lifecycle table. Every screen declares its hooks here; transition
   handling (on_exit of the old screen, on_enter of the new one) happens in a
   single place — screens_sync_lifecycle() — so no transition can bypass the
   hooks regardless of whether it was triggered from input or from update.
   Hooks fire on the first frame that observes the transition. */
typedef struct {
    const char *name;
    void (*on_enter)(AppState *state);
    void (*on_exit)(AppState *state);
    void (*update)(AppState *state);
    void (*handle_input)(AppState *state, const InputState *input);
    void (*render)(const AppState *state);
    /* Kicks the screen's data load and reports whether its visible content is
       ready: 1 ready, 0 still loading, -1 load failed (enter anyway — the
       screen renders its own error state). Screens without the hook are
       always ready. Drives ui_screens_navigate(). */
    int (*content_ready)(AppState *state);
    int owns_back; /* screen handles CIRCLE itself; no generic pop */
} ScreenDesc;

static ScreenId s_nav_pending = SCREEN_COUNT;

/* --- signature-normalizing wrappers ------------------------------------- */

static void splash_update(AppState *state)
{
    ui_screen_splash_update(state, &s_splash_flow);
}

static void splash_input(AppState *state, const InputState *input)
{
    ui_screen_splash_handle_input(state, input, &s_splash_flow);
}

static void splash_render(const AppState *state)
{
    (void)state;
    ui_screen_splash_render(&s_splash_flow, s_splash_pixels, s_splash_w, s_splash_h);
}

static void album_list_render(const AppState *state)
{
    (void)state;
    ui_screen_album_list_render();
}

static void net_info_render(const AppState *state)
{
    (void)state;
    ui_screen_net_info_render();
}

static void playlist_list_on_exit(AppState *state)
{
    (void)state;
    /* Playlist covers are no longer visible — drop their ref counts. */
    cover_manager_set_visible_range(0, 0, NULL, 0);
}

static void now_playing_on_exit(AppState *state)
{
    (void)state;
    ui_screen_now_playing_release_video();
}

/* --- lifecycle table ----------------------------------------------------- */

static const ScreenDesc s_screen_table[SCREEN_COUNT] = {
    [SCREEN_SPLASH] = {
        .name = "splash",
        .update = splash_update,
        .handle_input = splash_input,
        .render = splash_render,
        .owns_back = 1,
    },
    [SCREEN_MENU] = {
        .name = "menu",
        .handle_input = ui_screen_menu_handle_input,
        .render = ui_screen_menu_render,
        .owns_back = 1,
    },
    [SCREEN_NOW_PLAYING] = {
        .name = "now_playing",
        .on_exit = now_playing_on_exit,
        .update = ui_screen_now_playing_update,
        .handle_input = ui_screen_now_playing_handle_input,
        .render = ui_screen_now_playing_render,
    },
    [SCREEN_ALBUM_LIST] = {
        .name = "album_list",
        .handle_input = ui_screen_album_list_handle_input,
        .render = album_list_render,
    },
    [SCREEN_PLAYLIST_LIST] = {
        .name = "playlist_list",
        .on_enter = ui_screen_playlist_list_on_enter,
        .on_exit = playlist_list_on_exit,
        .update = ui_screen_playlist_list_update,
        .handle_input = ui_screen_playlist_list_handle_input,
        .render = ui_screen_playlist_list_render,
        .content_ready = ui_screen_playlist_list_content_ready,
    },
    [SCREEN_TRACK_LIST] = {
        .name = "track_list",
        .on_exit = ui_screen_track_list_on_exit,
        .update = ui_screen_track_list_update,
        .handle_input = ui_screen_track_list_handle_input,
        .render = ui_screen_track_list_render,
    },
    [SCREEN_ACCOUNT] = {
        .name = "account",
        .handle_input = ui_screen_account_handle_input,
        .render = ui_screen_account_render,
    },
    [SCREEN_ARTIST] = {
        .name = "artist",
        .update = ui_screen_artist_update,
        .handle_input = ui_screen_artist_handle_input,
        .render = ui_screen_artist_render,
        .content_ready = ui_screen_artist_content_ready,
    },
    [SCREEN_ARTIST_MENU] = {
        .name = "artist_menu",
        .update = ui_screen_artist_menu_update,
        .handle_input = ui_screen_artist_menu_handle_input,
        .render = ui_screen_artist_menu_render,
        .content_ready = ui_screen_artist_menu_content_ready,
    },
    [SCREEN_NET_INFO] = {
        .name = "net_info",
        .render = net_info_render,
    },
    [SCREEN_DEVICE_LOGIN] = {
        .name = "device_login",
        .on_enter = ui_screen_device_login_on_enter,
        .on_exit = ui_screen_device_login_on_exit,
        .update = ui_screen_device_login_update,
        .handle_input = ui_screen_device_login_handle_input,
        .render = ui_screen_device_login_render,
    },
};

static const ScreenDesc *screen_desc(ScreenId id)
{
    if (id < 0 || id >= SCREEN_COUNT) {
        return NULL;
    }
    return &s_screen_table[id];
}

static const char *screen_name(ScreenId id)
{
    const ScreenDesc *desc = screen_desc(id);
    return (desc && desc->name) ? desc->name : "none";
}

void ui_screens_navigate(AppState *state, ScreenId target)
{
    const ScreenDesc *desc = screen_desc(target);

    if (!desc) {
        return;
    }
    if (net_ui_status_input_locked()) {
        return;
    }
    if (!desc->content_ready || desc->content_ready(state) != 0) {
        s_nav_pending = SCREEN_COUNT;
        app_state_push(state, target);
        return;
    }
    s_nav_pending = target;
}

ScreenId ui_screens_nav_pending(void)
{
    return s_nav_pending;
}

static void screens_service_pending_nav(AppState *state)
{
    const ScreenDesc *desc;

    if (s_nav_pending == SCREEN_COUNT) {
        return;
    }
    desc = screen_desc(s_nav_pending);
    if (!desc || !desc->content_ready) {
        s_nav_pending = SCREEN_COUNT;
        return;
    }
    if (desc->content_ready(state) != 0) {
        ScreenId target = s_nav_pending;
        s_nav_pending = SCREEN_COUNT;
        app_state_push(state, target);
    }
}

static void screens_sync_lifecycle(AppState *state)
{
    ScreenId current = app_state_get_current(state);
    const ScreenDesc *prev_desc;
    const ScreenDesc *cur_desc;

    if (current == s_last_screen) {
        return;
    }

    s_nav_pending = SCREEN_COUNT; /* the screen changed under the gate */

    logLine("ui: screen change %s -> %s\n",
            screen_name(s_last_screen), screen_name(current));
    logger_flush();

    prev_desc = screen_desc(s_last_screen);
    if (prev_desc && prev_desc->on_exit) {
        prev_desc->on_exit(state);
    }

    cur_desc = screen_desc(current);
    if (cur_desc && cur_desc->on_enter) {
        cur_desc->on_enter(state);
    }

    s_last_screen = current;
}

static void apply_resource_mode(const AppState *state)
{
    if (!state) {
        return;
    }

    int browsing_active = (app_state_get_resource_mode(state) == APP_MODE_BROWSING);
    cover_manager_set_browsing_active(browsing_active);
}

void ui_screens_update(AppState *state, const InputState *input)
{
    const ScreenDesc *desc;

    system_status_update();
    net_ui_status_update(input ? input->hold : 0,
                         input ? input->wlan_on : 0);
    screens_sync_lifecycle(state);
    if (net_ui_status_input_locked()) {
        s_nav_pending = SCREEN_COUNT;
        apply_resource_mode(state);
        return;
    }
    screens_service_pending_nav(state);
    screens_sync_lifecycle(state);
    apply_resource_mode(state);

    desc = screen_desc(app_state_get_current(state));
    if (desc && desc->update) {
        desc->update(state);
    }
}

int ui_screens_init(void)
{
    void *data = NULL;
    int w = 0;
    int h = 0;
    const char *rel_path = "assets/splash.jpg";
    int loaded = 0;

    logLine("ui: splash load try '%s'\n", rel_path);
    if (image_load_rgba8888(rel_path, &data, &w, &h, NULL) == 0) {
        loaded = 1;
    }

    if (loaded) {
        s_splash_pixels = data;
        s_splash_w = w;
        s_splash_h = h;
        logLine("ui: splash loaded %dx%d\n", w, h);
        // Log memory after splash load (using safe function)
        {
            int maxFree = sceKernelMaxFreeMemSize();
            logLine("mem: after splash max_free=%d\n", maxFree);
        }
    } else {
        logLine("ui: splash load failed (non-critical)\n");
    }

    splash_flow_init(&s_splash_flow);
    system_status_init();
    net_ui_status_init();

    audio_cache_init();
    playback_controller_init();

    // Initialize cover manager (includes worker thread) - для превью 30x30
    cover_manager_init();

    // Initialize now playing cover (guaranteed memory for 200x200)
    cover_now_playing_init();
    video_cover_init();

    // Initialize track bootstrap worker.
    net_client_playlist_load_init();
    net_client_artist_load_init();
    net_client_track_bootstrap_init();

    return 0;
}

int ui_screens_shutdown(void)
{
    int quiesced = 1;
    /* Phase one only: close admission and join every consumer. Nothing is
     * deleted or freed unless every join has succeeded. The process exits after
     * this function, so phase two is OS reclamation. */
    net_tls_quiesce_begin();
    if (splash_flow_quiesce(&s_splash_flow) < 0) quiesced = 0;
    if (playback_controller_quiesce() < 0) quiesced = 0;
    if (cover_manager_quiesce() < 0) quiesced = 0;
    if (cover_now_playing_quiesce() < 0) quiesced = 0;
    if (video_cover_quiesce() < 0) quiesced = 0;
    if (net_client_track_bootstrap_quiesce() < 0) quiesced = 0;
    if (net_client_artist_load_quiesce() < 0) quiesced = 0;
    if (net_client_playlist_load_quiesce() < 0) quiesced = 0;
    if (audio_cache_quiesce() < 0) quiesced = 0;
    if (net_client_shutdown() < 0) quiesced = 0;
    return quiesced ? 0 : -1;
}

void ui_screens_handle_input(AppState *state, const InputState *input)
{
    ScreenId current = app_state_get_current(state);
    const ScreenDesc *desc = screen_desc(current);

    if (net_ui_status_input_locked()) {
        return;
    }

    /* Any new input while a gated navigation waits re-asserts user control;
       the handler below may immediately re-request it (e.g. repeated X). */
    if (s_nav_pending != SCREEN_COUNT && input->pressed) {
        s_nav_pending = SCREEN_COUNT;
    }

    if (desc && !desc->owns_back && (input->pressed & PSP_CTRL_CIRCLE)) {
        logLine("ui: circle pop screen=%s\n", screen_name(current));
        app_state_pop(state);
        apply_resource_mode(state);
        return;
    }

    if (desc && desc->handle_input) {
        desc->handle_input(state, input);
    }
    apply_resource_mode(state);
}

void ui_screens_render(const AppState *state)
{
    ScreenId current = app_state_get_current(state);
    const ScreenDesc *desc = screen_desc(current);

    ui_draw_begin_frame();

    if (desc && desc->render) {
        desc->render(state);
    } else {
        ui_draw_clear(0xFF1A1A1A);
        ui_common_draw_header("Unknown");
    }

    if (current != SCREEN_SPLASH && current != SCREEN_ARTIST_MENU) {
        ui_common_draw_top_status();
    }
    net_ui_status_on_rendered_frame();

    ui_draw_end_frame();
}
