#include "app/app_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pspkernel.h>

#include "core/logger.h"

static AppResourceMode app_state_mode_for_screen(ScreenId screen)
{
    return (screen == SCREEN_NOW_PLAYING) ? APP_MODE_PLAYBACK : APP_MODE_BROWSING;
}

static void app_state_stack_reset(AppState *state)
{
    state->stack_size = 0;
    state->stack[state->stack_size++] = SCREEN_SPLASH;
    state->resource_mode = app_state_mode_for_screen(SCREEN_SPLASH);
}

int app_state_init(AppState *state)
{
    if (!state) {
        return -1;
    }
    memset(state, 0, sizeof(AppState));

    u64 usec = sceKernelGetSystemTimeWide();
    state->splash_start_ms = (u32)(usec / 1000ULL);
    app_state_stack_reset(state);
    return 0;
}

void app_state_shutdown(AppState *state)
{
    if (!state) {
        return;
    }
    app_state_free_tracks(state);
    free(state->playlists);
    state->playlists = NULL;
    state->playlist_count = 0;
    state->playlist_capacity = 0;
    free(state->liked_playlists);
    state->liked_playlists = NULL;
    state->liked_playlist_count = 0;
    state->liked_playlist_capacity = 0;
}

void app_state_push(AppState *state, ScreenId screen)
{
    assert(state);
    if (state->stack_size < (int)(sizeof(state->stack) / sizeof(state->stack[0]))) {
        state->stack[state->stack_size++] = screen;
        state->resource_mode = app_state_mode_for_screen(screen);
    } else {
        logLine("app_state: stack overflow on push to screen=%d\n", screen);
    }
}

void app_state_pop(AppState *state)
{
    assert(state);
    if (state->stack_size > 1) {
        state->stack_size--;
        state->resource_mode = app_state_mode_for_screen(state->stack[state->stack_size - 1]);
    }
}

void app_state_set(AppState *state, ScreenId screen)
{
    assert(state);
    if (state->stack_size > 0) {
        state->stack[state->stack_size - 1] = screen;
        state->resource_mode = app_state_mode_for_screen(screen);
    }
}

void app_state_reset(AppState *state, ScreenId screen)
{
    assert(state);
    state->stack_size = 1;
    state->stack[0] = screen;
    state->resource_mode = app_state_mode_for_screen(screen);
}

void app_state_free_tracks(AppState *state)
{
    assert(state);
    if (state->track_store.ids) {
        free(state->track_store.ids);
        state->track_store.ids = NULL;
    }
    state->track_store.ids_capacity = 0;
    state->track_store.count = 0;
    state->track_store.ids_complete = 0;
    memset(state->track_store.window_valid, 0, sizeof(state->track_store.window_valid));
    state->track_store.window_start = 0;
    state->track_ui.track_selected = 0;
    state->track_ui.track_scroll = 0;
}

void app_state_reset_track_bootstrap(AppState *state, int playlist_kind, int playlist_selected_index,
                                     const char *playlist_uuid)
{
    assert(state);
    app_state_free_tracks(state);

    state->track_boot.target_playlist_kind = playlist_kind;
    state->track_boot.target_uuid[0] = '\0';
    if (playlist_uuid) {
        snprintf(state->track_boot.target_uuid, sizeof(state->track_boot.target_uuid),
                 "%s", playlist_uuid);
    }
    state->track_boot.status = TRACK_BOOT_LOADING;
    state->track_boot.loaded_count = 0;
    state->track_boot.error_code = 0;

    state->track_ui.pending_playlist_kind = playlist_kind;
    state->track_ui.pending_playlist_selected_index = playlist_selected_index;
    state->track_ui.track_screen_transition_done = 0;
    state->track_ui.track_bootstrap_indicator_visible = 1;
}

void app_state_anchor_save(AppState *state, const char *uuid, const char *track_id, int pos)
{
    TrackAnchor *slot = NULL;
    int i;

    assert(state);
    if (!uuid || !uuid[0] || !track_id || !track_id[0] || pos < 0) {
        return;
    }

    for (i = 0; i < state->track_anchor_count; i++) {
        if (strcmp(state->track_anchors[i].uuid, uuid) == 0) {
            slot = &state->track_anchors[i];
            break;
        }
    }
    if (!slot) {
        if (state->track_anchor_count < TRACK_ANCHOR_CAPACITY) {
            slot = &state->track_anchors[state->track_anchor_count++];
        } else {
            slot = &state->track_anchors[state->track_anchor_next];
            state->track_anchor_next =
                (state->track_anchor_next + 1) % TRACK_ANCHOR_CAPACITY;
        }
    }

    snprintf(slot->uuid, sizeof(slot->uuid), "%s", uuid);
    snprintf(slot->track_id, sizeof(slot->track_id), "%s", track_id);
    slot->pos = pos;
}

const TrackAnchor *app_state_anchor_find(const AppState *state, const char *uuid)
{
    int i;

    assert(state);
    if (!uuid || !uuid[0]) {
        return NULL;
    }
    for (i = 0; i < state->track_anchor_count; i++) {
        if (strcmp(state->track_anchors[i].uuid, uuid) == 0) {
            return &state->track_anchors[i];
        }
    }
    return NULL;
}

void app_state_cancel_track_bootstrap(AppState *state)
{
    assert(state);
    state->track_boot.generation++;
    state->track_boot.target_playlist_kind = 0;
    state->track_boot.status = TRACK_BOOT_IDLE;
    state->track_boot.loaded_count = 0;
    state->track_boot.error_code = 0;
    state->track_ui.pending_playlist_kind = 0;
    state->track_ui.pending_playlist_selected_index = -1;
    state->track_ui.track_screen_transition_done = 0;
    state->track_ui.track_bootstrap_indicator_visible = 0;
}
