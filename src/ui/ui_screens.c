#include "services/track_like.h"
#include "ui/ui_screens.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "core/fs.h"
#include "ui/ui_draw.h"
#include "core/logger.h"
#include "services/splash_flow.h"
#include "services/audio_cache.h"
#include "services/cover_manager.h"
#include "services/cover_now_playing.h"
#include "services/net_client.h"
#include "services/library.h"
#include "services/net_tls.h"
#include "services/last_play.h"
#include "services/net_stack.h"
#include "services/net_ui_status.h"
#include "services/playback_controller.h"
#include "services/playback_queue.h"
#include "services/track_hydrator.h"
#include "services/track_meta_store.h"
#include "services/token_loader.h"
#include "services/wave.h"
#include "services/playback_reporter.h"

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
#include "ui/ui_screen_eq.h"
#include "ui/ui_screen_search.h"
#include "ui/ui_screen_help.h"
#include "ui/ui_screen_memory.h"
#include "ui/ui_common.h"
#include "ui/ui_icon_atlas.h"
#include "ui/ui_glow_bg.h"
#include "ui/ui_idle.h"
#include "ui/ui_theme.h"

static SplashFlow s_splash_flow;
static ScreenId s_last_screen = SCREEN_COUNT;
static int s_fatal_resource_error = 0;
static const char *s_fatal_resource_path = NULL;

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
       ready: 1 ready, 0 still loading, -1 load failed. On failure the source
       screen stays visible. Screens without the hook are
       always ready. Drives ui_screens_navigate(). */
    int (*content_ready)(AppState *state);
    int owns_back; /* screen handles CIRCLE itself; no generic pop */
} ScreenDesc;

typedef enum { NAV_SCREEN_CONTENT, NAV_WAVE_REQUEST, NAV_PLAYLIST_REQUEST } NavContentKind;

typedef struct {
    UiNavAction action;
    NavContentKind content_kind;
    ScreenId source;
    ScreenId destination;
    unsigned int request_id;
    int position;
    PlaybackQueueDraft *draft;
    int logged_phase;
    TrackRef logged_ref;
} PendingNavigation;

static PendingNavigation s_nav = { .destination = SCREEN_COUNT };
static int s_consume_repeat_input;

/* --- signature-normalizing wrappers ------------------------------------- */

static void splash_update(AppState *state)
{
    splash_flow_tick(&s_splash_flow, state);
    if (!s_splash_flow.ready) return;

    if (s_splash_flow.playback_resume_requested) {
        logLine("splash: ready -> menu+now_playing restored\n");
        logger_flush();
        app_state_reset(state, SCREEN_MENU);
        ui_screens_navigate(state, SCREEN_NOW_PLAYING);
        logLine("splash: restore stack done\n");
        logger_flush();
        return;
    }
    logLine("splash: ready -> app_state_reset(MENU) begin\n");
    logger_flush();
    app_state_reset(state, SCREEN_MENU);
    logLine("splash: app_state_reset(MENU) done\n");
    logger_flush();
}

static void splash_input(AppState *state, const InputState *input)
{
    ui_screen_splash_handle_input(state, input, &s_splash_flow);
}

static void splash_render(const AppState *state)
{
    (void)state;
    ui_screen_splash_render(&s_splash_flow);
}

static void splash_on_exit(AppState *state)
{
    (void)state;
    splash_flow_release_image(&s_splash_flow);
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
    library_playlists_refresh(state);
    /* Playlist covers are no longer visible — drop their ref counts. */
    cover_manager_set_visible_range(0, 0, NULL, 0);
}

/* --- lifecycle table ----------------------------------------------------- */

static const ScreenDesc s_screen_table[SCREEN_COUNT] = {
    [SCREEN_SPLASH] = {
        .name = "splash",
        .on_exit = splash_on_exit,
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
    [SCREEN_SETTINGS] = {
        .name = "settings",
        .handle_input = ui_screen_settings_handle_input,
        .render = ui_screen_settings_render,
    },
    [SCREEN_SETTINGS_PLAYBACK] = {
        .name = "settings_playback",
        .handle_input = ui_screen_playback_settings_handle_input,
        .render = ui_screen_playback_settings_render,
    },
    [SCREEN_SETTINGS_DISPLAY] = {
        .name = "settings_display",
        .handle_input = ui_screen_display_settings_handle_input,
        .render = ui_screen_display_settings_render,
    },
    [SCREEN_SETTINGS_DIAGNOSTICS] = {
        .name = "settings_diagnostics",
        .handle_input = ui_screen_diagnostics_settings_handle_input,
        .render = ui_screen_diagnostics_settings_render,
    },
    [SCREEN_NOW_PLAYING] = {
        .name = "now_playing",
        .on_exit = ui_screen_now_playing_on_exit,
        .update = ui_screen_now_playing_update,
        .handle_input = ui_screen_now_playing_handle_input,
        .render = ui_screen_now_playing_render,
        .content_ready = ui_screen_now_playing_content_ready,
        .owns_back = 1,  // круг перехватывает дропдаун SELECT
    },
    [SCREEN_ALBUM_LIST] = {
        .name = "album_list",
        .update = ui_screen_album_list_update,
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
        .content_ready = ui_screen_track_list_content_ready,
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
    [SCREEN_EQ] = {
        .name = "eq",
        .on_enter = ui_screen_eq_on_enter,
        .on_exit = ui_screen_eq_on_exit,
        .handle_input = ui_screen_eq_handle_input,
        .render = ui_screen_eq_render,
    },
    [SCREEN_SEARCH] = {
        .name = "search",
        .on_enter = ui_screen_search_on_enter,
        .on_exit = ui_screen_search_on_exit,
        .update = ui_screen_search_update,
        .handle_input = ui_screen_search_handle_input,
        .render = ui_screen_search_render,
    },
    [SCREEN_HELP] = {
        .name = "help",
        .render = ui_screen_help_render,
    },
    [SCREEN_MEMORY] = {
        .name = "memory",
        .render = ui_screen_memory_render,
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

static void screens_cancel_navigation(AppState *state)
{
    if (s_nav.action == UI_NAV_WAVE && s_nav.content_kind == NAV_WAVE_REQUEST)
        wave_cancel(s_nav.request_id);
    else if (s_nav.action == UI_NAV_PLAYLIST) {
        logLine("track_play: cancel request=%u position=%d phase=%d source=%d current=%d\n",
            s_nav.request_id, s_nav.position, s_nav.logged_phase, s_nav.source, app_state_get_current(state));
        logger_flush();
        track_hydrator_cancel(TRACK_HYDRATOR_PREPARE);
        if (s_nav.source == SCREEN_PLAYLIST_LIST) app_state_cancel_track_bootstrap(state);
    } else if (s_nav.action == UI_NAV_SCREEN &&
             s_nav.destination == SCREEN_TRACK_LIST)
        app_state_cancel_track_bootstrap(state);
    playback_queue_discard(s_nav.draft);
    memset(&s_nav, 0, sizeof(s_nav));
    s_nav.destination = SCREEN_COUNT;
}

static void screens_navigate_content(AppState *state, UiNavAction action,
                                     ScreenId target)
{
    const ScreenDesc *desc = screen_desc(target);
    int ready;

    if (!desc) {
        return;
    }
    if (net_ui_status_input_locked()) {
        return;
    }
    if (ui_screens_nav_matches(action, target) &&
        s_nav.content_kind == NAV_SCREEN_CONTENT &&
        s_nav.source == app_state_get_current(state)) return;
    screens_cancel_navigation(state);
    if (action == UI_NAV_WAVE && app_state_get_current(state) == target) return;
    if (!desc->content_ready) {
        app_state_push(state, target);
        return;
    }
    ready = desc->content_ready(state);
    if (ready > 0) {
        app_state_push(state, target);
        return;
    }
    if (ready < 0) {
        return;
    }
    s_nav.action = action;
    s_nav.content_kind = NAV_SCREEN_CONTENT;
    s_nav.source = app_state_get_current(state);
    s_nav.destination = target;
}

void ui_screens_navigate(AppState *state, ScreenId target)
{
    if (target == SCREEN_PLAYLIST_LIST &&
        !ui_screens_nav_matches(UI_NAV_SCREEN, target))
        library_playlists_refresh(state);
    screens_navigate_content(state, UI_NAV_SCREEN, target);
}

void ui_screens_play_playlist(AppState *state, int position)
{
    screens_cancel_navigation(state);
    s_nav.action = UI_NAV_PLAYLIST;
    s_nav.content_kind = NAV_PLAYLIST_REQUEST;
    s_nav.source = app_state_get_current(state);
    s_nav.destination = SCREEN_NOW_PLAYING;
    s_nav.request_id = state->track_boot.generation;
    s_nav.position = position;
    logLine("track_play: request generation=%u position=%d source=%d\n", s_nav.request_id, position, s_nav.source);
    logger_flush();
}
void ui_screens_start_wave(AppState *state, const WaveRequestParams *params)
{
    WavePrepareResult prepared;
    if (!state || net_ui_status_input_locked() ||
        (app_state_get_current(state) != SCREEN_NOW_PLAYING &&
         state->stack_size >= APP_SCREEN_STACK_CAPACITY)) return;
    if (s_nav.action != UI_NAV_NONE &&
        s_nav.source != app_state_get_current(state))
        screens_cancel_navigation(state);
    prepared = wave_prepare(params);
    if (prepared.status == WAVE_PREPARE_FAILED) return;
    if (prepared.status == WAVE_PREPARE_EXISTING) {
        screens_navigate_content(state, UI_NAV_WAVE, SCREEN_NOW_PLAYING);
        return;
    }
    if (s_nav.action == UI_NAV_WAVE && s_nav.request_id == prepared.request_id &&
        s_nav.source == app_state_get_current(state)) return;
    screens_cancel_navigation(state);
    s_nav.action = UI_NAV_WAVE;
    s_nav.content_kind = NAV_WAVE_REQUEST;
    s_nav.source = app_state_get_current(state);
    s_nav.destination = SCREEN_NOW_PLAYING;
    s_nav.request_id = prepared.request_id;
    playback_controller_request_stop();
}

UiNavAction ui_screens_nav_action(void)
{
    return s_nav.action;
}

int ui_screens_nav_matches(UiNavAction action, ScreenId destination)
{
    return s_nav.action != UI_NAV_NONE && s_nav.action == action &&
           s_nav.destination == destination;
}

ScreenId ui_screens_nav_pending(void)
{
    return s_nav.action == UI_NAV_SCREEN ? s_nav.destination : SCREEN_COUNT;
}

static void screens_service_pending_nav(AppState *state)
{
    const ScreenDesc *desc;
    int ready;

    if (s_nav.action == UI_NAV_NONE) {
        return;
    }
    if (s_nav.source != app_state_get_current(state)) {
        screens_cancel_navigation(state);
        return;
    }
    if (s_nav.content_kind == NAV_PLAYLIST_REQUEST) {
        int failed = 0, complete = 0;
        net_client_track_store_lock();
        failed = s_nav.request_id != (unsigned int)state->track_boot.generation ||
            state->track_boot.status == TRACK_BOOT_ERROR;
        complete = state->track_context.complete;
        if (failed) logLine("track_play: reject membership request=%u generation=%d boot=%d error=%d\n",
            s_nav.request_id, state->track_boot.generation, state->track_boot.status, state->track_boot.error_code);
        net_client_track_store_unlock();
        if (!failed && complete && !s_nav.draft) {
            s_nav.draft = playback_queue_prepare(state->track_context.refs,
                state->track_context.count, s_nav.position, PLAYBACK_QUEUE_SOURCE_PLAYLIST,
                state->track_boot.target_playlist_kind, state->track_boot.generation);
            failed = !s_nav.draft;
            logLine("track_play: draft count=%d position=%d result=%d\n", state->track_context.count, s_nav.position, failed ? -1 : 0);
            logger_flush();
        }
        if (failed) { screens_cancel_navigation(state); return; }
        if (!complete) {
            if (s_nav.logged_phase != 1) {
                s_nav.logged_phase = 1;
                logLine("track_play: wait membership request=%u\n", s_nav.request_id);
                logger_flush();
            }
            return;
        }
        TrackEntry first;
        const TrackRef *ref = playback_queue_draft_current(s_nav.draft);
        if (!ref) { logLine("track_play: reject empty draft\n"); screens_cancel_navigation(state); return; }
        if (track_meta_store_get_for(ref->id, ref->album_id, &first) != 0) {
            char token[256];
            int position = -1;
            if (token_loader_read(token, sizeof(token)) != 0) { logLine("track_play: reject token read\n"); screens_cancel_navigation(state); return; }
            int rc = track_hydrator_request(TRACK_HYDRATOR_PREPARE, token, ref,
                                            &position, 1, s_nav.request_id);
            memset(token, 0, sizeof(token));
            if (s_nav.logged_phase != 2 || strcmp(s_nav.logged_ref.id, ref->id) != 0 ||
                s_nav.logged_ref.album_id != ref->album_id || rc < 0) {
                s_nav.logged_phase = 2;
                s_nav.logged_ref = *ref;
                logLine("track_play: metadata request=%u track=%s album=%d result=%d\n",
                    s_nav.request_id, ref->id, ref->album_id, rc);
                logger_flush();
            }
            if (rc < 0) screens_cancel_navigation(state);
            return;
        }
        if (!first.available) {
            logLine("track_play: skip unavailable track=%s album=%d\n", ref->id, ref->album_id);
            logger_flush();
            if (playback_queue_draft_next(s_nav.draft) < 0) screens_cancel_navigation(state);
            return;
        }
        logLine("track_play: commit request=%u track=%s album=%d\n", s_nav.request_id, first.id, first.album_id);
        logger_flush();
        playback_queue_commit(s_nav.draft);
        s_nav.draft = NULL;
        state->now_playing_track = first;
        memset(&s_nav, 0, sizeof(s_nav));
        s_nav.destination = SCREEN_COUNT;
        app_state_push(state, SCREEN_NOW_PLAYING);
        playback_controller_request_play_prepared(&first);
        logLine("track_play: handed to player screen=%d track=%s\n", app_state_get_current(state), first.id);
        logger_flush();
        return;
    }
    if (s_nav.content_kind == NAV_WAVE_REQUEST) {
        TrackEntry first;
        ready = wave_poll(s_nav.request_id, &first);
        if (ready == 0) return;
        if (ready < 0 || wave_commit(s_nav.request_id, &first) != 0) {
            screens_cancel_navigation(state);
            return;
        }
        state->now_playing_track = first;
        screens_cancel_navigation(state);
        if (app_state_get_current(state) != SCREEN_NOW_PLAYING)
            app_state_push(state, SCREEN_NOW_PLAYING);
        playback_controller_request_play_prepared(&first);
        return;
    }
    desc = screen_desc(s_nav.destination);
    if (!desc || !desc->content_ready) {
        screens_cancel_navigation(state);
        return;
    }
    ready = desc->content_ready(state);
    if (ready > 0) {
        ScreenId target = s_nav.destination;
        memset(&s_nav, 0, sizeof(s_nav));
        s_nav.destination = SCREEN_COUNT;
        app_state_push(state, target);
    } else if (ready < 0) {
        screens_cancel_navigation(state);
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

    screens_cancel_navigation(state);

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

/* Ручной выход экрана (owns_back): то же, что generic-pop. */
void ui_screens_pop_screen(AppState *state)
{
    screens_cancel_navigation(state);
    logLine("ui: manual pop\n");
    app_state_pop(state);
    apply_resource_mode(state);
}

void ui_screens_update(AppState *state, const InputState *input)
{
    const ScreenDesc *desc;

    if (s_fatal_resource_error) return;

    s_consume_repeat_input = 0;

    track_like_update(state->currentUser.uid, g_playback.current_track.id);

    net_ui_status_update(input ? input->hold : 0,
                         input ? input->wlan_on : 0);
    screens_sync_lifecycle(state);
    if (net_ui_status_input_locked()) {
        if (s_nav.action == UI_NAV_PLAYLIST) logLine("track_play: interrupted network input lock\n");
        screens_cancel_navigation(state);
        apply_resource_mode(state);
        return;
    }
    /* Input from this frame takes precedence over a completed request. */
    if (s_nav.action != UI_NAV_NONE && input && input->pressed) {
        if (s_nav.action == UI_NAV_PLAYLIST) logLine("track_play: interrupted input pressed=0x%08X\n", input->pressed);
        if (s_nav.source == SCREEN_MENU && input->pressed == PSP_CTRL_CROSS)
            s_consume_repeat_input = 1;
        else
            screens_cancel_navigation(state);
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
    s_fatal_resource_error = 0;
    s_fatal_resource_path = NULL;
    if (ui_icon_atlas_init("assets/ui_icons.t4") != 0) {
        s_fatal_resource_error = 1;
        s_fatal_resource_path = "assets/ui_icons.t4";
        logLine("ui: fatal resource error path='assets/ui_icons.t4'\n");
        return 0;
    }

    splash_flow_init(&s_splash_flow);
    ui_screen_memory_init();
    net_ui_status_init();
    glow_bg_init();
    idle_init();

    audio_cache_init();
    if (playback_reporter_init() < 0) {
        logLine("ui: reporter init failed\n");
        return -1;
    }
    playback_controller_init();

    // Initialize cover manager (includes worker thread) - для превью 30x30
    cover_manager_init();

    // Initialize now playing cover (guaranteed memory for 200x200)
    cover_now_playing_init();

    // Initialize track bootstrap worker.
    net_client_playlist_load_init();
    net_client_artist_load_init();
    net_client_track_bootstrap_init();

    return 0;
}

int ui_screens_has_fatal_resource_error(void)
{
    return s_fatal_resource_error;
}

int ui_screens_shutdown(void)
{
    int quiesced = 1;
    if (s_fatal_resource_error) {
        ui_icon_atlas_shutdown();
        return 0;
    }
    /* Phase one only: close admission and join every consumer. Nothing is
     * deleted or freed unless every join has succeeded. The process exits after
     * this function, so phase two is OS reclamation. */
    last_play_save();  // чистый выход посреди трека — запомнили
    glow_bg_free();
    net_tls_quiesce_begin();
    if (wave_quiesce() < 0) quiesced = 0;
    if (splash_flow_quiesce(&s_splash_flow) < 0) quiesced = 0;
    if (track_like_quiesce() < 0) quiesced = 0;
    if (playback_controller_quiesce() < 0) quiesced = 0;
    if (playback_reporter_quiesce() < 0) quiesced = 0;
    else playback_reporter_shutdown();
    if (cover_manager_quiesce() < 0) quiesced = 0;
    if (cover_now_playing_quiesce() < 0) quiesced = 0;
    if (net_client_track_bootstrap_quiesce() < 0) quiesced = 0;
    if (net_client_artist_load_quiesce() < 0) quiesced = 0;
    if (net_client_playlist_load_quiesce() < 0) quiesced = 0;
    if (audio_cache_quiesce() < 0) quiesced = 0;
    if (net_client_shutdown() < 0) quiesced = 0;
    if (last_play_quiesce() < 0) quiesced = 0;
    if (quiesced && track_meta_store_quiesce() < 0) quiesced = 0;
    ui_icon_atlas_shutdown();
    return quiesced ? 0 : -1;
}

void ui_screens_handle_input(AppState *state, const InputState *input)
{
    ScreenId current = app_state_get_current(state);
    const ScreenDesc *desc = screen_desc(current);

    if (s_fatal_resource_error || net_ui_status_input_locked() ||
        s_consume_repeat_input) {
        if (input && (input->pressed & PSP_CTRL_CROSS) && current == SCREEN_TRACK_LIST) {
            logLine("track_play: cross blocked fatal=%d network_lock=%d repeat=%d\n",
                s_fatal_resource_error, net_ui_status_input_locked(), s_consume_repeat_input);
            logger_flush();
        }
        return;
    }

    /* SELECT — быстрый доступ к настройкам с любого экрана; повторное нажатие
       закрывает их. Не на заставке, входе по коду и в поиске (ввод текста). */
    if ((input->pressed & PSP_CTRL_SELECT) &&
        current != SCREEN_SPLASH && current != SCREEN_DEVICE_LOGIN &&
        current != SCREEN_SEARCH) {
        if (current == SCREEN_SETTINGS_PLAYBACK ||
            current == SCREEN_SETTINGS_DISPLAY ||
            current == SCREEN_SETTINGS_DIAGNOSTICS) {
            app_state_pop(state);
            current = app_state_get_current(state);
        }
        if (current == SCREEN_SETTINGS) {
            logLine("ui: select -> close settings\n");
            app_state_pop(state);
        } else if (state->stack_size < APP_SCREEN_STACK_CAPACITY) {
            logLine("ui: select -> settings from %s\n", screen_name(current));
            app_state_push(state, SCREEN_SETTINGS);
        }
        apply_resource_mode(state);
        return;
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

    ui_theme_tick();
    ui_draw_begin_frame();

    if (s_fatal_resource_error) {
        char fatal_line[96];
        ui_draw_clear(UI_COLOR_BACKGROUND);
        snprintf(fatal_line, sizeof(fatal_line), "FATAL: %s",
                 s_fatal_resource_path ? s_fatal_resource_path : "UI resource");
        ui_draw_text(16.0f, 118.0f, fatal_line, UI_COLOR_PRIMARY);
        ui_draw_text(16.0f, 138.0f, "Reinstall application resources",
                     UI_COLOR_ACTIVE);
        ui_draw_end_frame();
        return;
    }

    if (desc && desc->render) {
        desc->render(state);
    } else {
        ui_draw_clear(UI_COLOR_BACKGROUND);
        ui_common_draw_header("Unknown");
    }

    if (current != SCREEN_SPLASH && current != SCREEN_ARTIST_MENU) {
        ui_common_draw_top_status();
    }
    ui_common_draw_eq_toast();

    ui_draw_end_frame();
}
