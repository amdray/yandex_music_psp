#include "services/library.h"

#include <string.h>
#include <stdio.h>
#include <psptypes.h>
#include <pspkernel.h>

#include "services/net_client.h"
#include "services/token_loader.h"
#include "services/locale.h"
#include "core/logger.h"

/* Freshness-метки по вкладкам: когда загрузка последний раз достигла READY.
 * Повторный вход в окно свежести пропускает рефетч (gated-вход из меню только
 * что загрузил список); всё старше — обновляется, чтобы поймать внешние правки
 * (revision — ключ трек-кэша). Раньше это жило в статике экрана — здесь ему
 * место, т.к. это состояние загрузки, а не отрисовки. */
#define PLAYLIST_LIST_FRESH_US 3000000ULL
static u64 s_ready_stamp_us[2];
static PlaylistLoadStatus s_prev_status[2];

/* Токен, которым подкармливается гидрация окна перехода; перечитывается при
 * смене generation. */
static char s_gate_token[256];
static int  s_gate_token_generation = -1;

/* --- плейлисты ----------------------------------------------------------- */

static void observe(AppState *s)
{
    PlaylistLoadState *loads[2];
    int i;

    net_client_playlist_load_poll(s);
    if (s->playlists_load.status == PLAYLIST_LOAD_READY &&
        s->playlists && s->playlist_count > 0 &&
        s->playlists[0].playlist_id == PLAYLIST_KIND_LIKED_TRACKS) {
        snprintf(s->playlists[0].title, sizeof(s->playlists[0].title), "%s",
                 locale_get(LOCALE_PLAYLIST_LIKED_TRACKS));
    }
    loads[0] = &s->playlists_load;
    loads[1] = &s->liked_playlists_load;
    for (i = 0; i < 2; i++) {
        if (loads[i]->status == PLAYLIST_LOAD_READY &&
            s_prev_status[i] != PLAYLIST_LOAD_READY) {
            s_ready_stamp_us[i] = (u64)sceKernelGetSystemTimeWide();
        }
        s_prev_status[i] = loads[i]->status;
    }
}

static void ensure_loading(AppState *s, NetPlaylistLoadKind kind)
{
    PlaylistLoadState *load = (kind == NET_PLAYLIST_LOAD_MY)
                                  ? &s->playlists_load
                                  : &s->liked_playlists_load;
    char token[256];

    if (s->currentUser.uid <= 0 || load->status != PLAYLIST_LOAD_IDLE) {
        return;
    }

    if (token_loader_read(token, sizeof(token)) != 0) {
        load->status = PLAYLIST_LOAD_ERROR;
        load->error_code = -1;
        return;
    }

    int generation = load->generation + 1;
    int rc = net_client_playlist_load_start(kind, token, s->currentUser.uid, generation);
    memset(token, 0, sizeof(token));

    if (rc == 0) {
        load->generation = generation;
        load->status = PLAYLIST_LOAD_LOADING;
        load->error_code = 0;
        logLine("library: playlist load start kind=%d uid=%d generation=%d\n",
                kind, s->currentUser.uid, generation);
    } else if (rc < 0) {
        load->generation = generation;
        load->status = PLAYLIST_LOAD_ERROR;
        load->error_code = rc;
        logLine("library: playlist load start failed kind=%d rc=%d\n", kind, rc);
    }
}

PlaylistLoadStatus library_playlists_service(AppState *s, int tab)
{
    PlaylistLoadState *load;

    observe(s);

    load = (tab == 0) ? &s->playlists_load : &s->liked_playlists_load;
    if (load->status == PLAYLIST_LOAD_IDLE) {
        ensure_loading(s, (tab == 0) ? NET_PLAYLIST_LOAD_MY : NET_PLAYLIST_LOAD_LIKED);
    }
    return load->status;
}

void library_playlists_refresh_on_enter(AppState *s)
{
    PlaylistLoadState *loads[2];
    u64 now;
    int i;

    if (!s) {
        return;
    }

    loads[0] = &s->playlists_load;
    loads[1] = &s->liked_playlists_load;
    now = (u64)sceKernelGetSystemTimeWide();
    for (i = 0; i < 2; i++) {
        if (loads[i]->status == PLAYLIST_LOAD_ERROR ||
            (loads[i]->status == PLAYLIST_LOAD_READY &&
             (now - s_ready_stamp_us[i]) > PLAYLIST_LIST_FRESH_US)) {
            loads[i]->status = PLAYLIST_LOAD_IDLE;
        }
    }
}

/* --- открыть плейлист ---------------------------------------------------- */

int library_open_playlist(AppState *s, const PlaylistEntry *entry, int tab, int selected_index)
{
    if (!s || !entry) {
        return -1;
    }

    int playlist_kind = entry->playlist_id;
    /* Для лайкнутых плейлистов — owner_uid из записи; 0 = текущий пользователь. */
    int owner_uid = (tab == 1 && entry->owner_uid != 0)
                        ? entry->owner_uid
                        : s->currentUser.uid;
    char token[256];

    net_client_track_store_lock();
    s->track_boot.generation++;
    app_state_reset_track_bootstrap(s, playlist_kind, selected_index, entry->uuid);
    net_client_track_store_unlock();

    if (token_loader_read(token, sizeof(token)) != 0) {
        net_client_track_store_lock();
        s->track_boot.status = TRACK_BOOT_ERROR;
        s->track_boot.error_code = -10;
        s->track_ui.track_bootstrap_indicator_visible = 0;
        net_client_track_store_unlock();
        return -1;
    }

    int rc = net_client_track_bootstrap_start(token, owner_uid, playlist_kind,
                                              entry->revision, s->track_boot.generation,
                                              entry->uuid, s);
    memset(token, 0, sizeof(token));

    if (rc != 0) {
        net_client_track_store_lock();
        s->track_boot.status = TRACK_BOOT_ERROR;
        s->track_boot.error_code = -11;
        s->track_ui.track_bootstrap_indicator_visible = 0;
        net_client_track_store_unlock();
        return -1;
    }
    return 0;
}

/* --- гейт перехода в track_list ------------------------------------------ */

int library_track_entry_ready(AppState *s, int *out_pos, int *out_count)
{
    int enter_pos = 0;
    int pos_known = 0;
    int window_ready = 0;
    int generation = 0;
    int count_snap = 0;

    if (!s) {
        return 0;
    }

    net_client_track_store_lock();
    if ((s->track_boot.status == TRACK_BOOT_LOADING ||
         s->track_boot.status == TRACK_BOOT_FULL_READY) &&
        !s->track_ui.track_screen_transition_done &&
        s->track_store.count > 0) {
        const TrackAnchor *anchor =
            app_state_anchor_find(s, s->track_boot.target_uuid);
        int count = s->track_store.count;
        int complete = s->track_store.ids_complete;

        pos_known = 1;
        if (anchor) {
            int found = -1;
            int i;
            for (i = 0; i < count; i++) {
                if (strcmp(s->track_store.ids[i], anchor->track_id) == 0) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                enter_pos = found;
            } else if (complete) {
                /* Трек удалён извне: ближайшая уцелевшая позиция. */
                enter_pos = anchor->pos;
                if (enter_pos > count - 1) {
                    enter_pos = count - 1;
                }
                if (enter_pos < 0) {
                    enter_pos = 0;
                }
            } else {
                pos_known = 0; /* anchor может быть ниже по стриму */
            }
        }
        if (pos_known) {
            window_ready = net_client_track_window_ready_locked(
                s, enter_pos, TRACK_LIST_VISIBLE_ROWS);
            generation = s->track_boot.generation;
            count_snap = count;
        }
    }
    net_client_track_store_unlock();

    if (!pos_known) {
        return 0;
    }

    /* Держим гидрацию текущей к входному окну, пока ждём.
     * Горячий путь: только видимые строки (5, а не 48) — первый экран
     * в разы раньше; полное окно доберёт обычный update после входа. */
    if (s_gate_token_generation != generation) {
        s_gate_token[0] = '\0';
        if (token_loader_read(s_gate_token, sizeof(s_gate_token)) == 0) {
            s_gate_token_generation = generation;
        }
    }
    net_client_track_window_focus_span(s, s_gate_token, enter_pos,
                                       TRACK_LIST_VISIBLE_ROWS);

    if (!window_ready) {
        return 0;
    }

    if (out_pos) {
        *out_pos = enter_pos;
    }
    if (out_count) {
        *out_count = count_snap;
    }
    return 1;
}
