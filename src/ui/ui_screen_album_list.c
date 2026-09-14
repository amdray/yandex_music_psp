#include "ui/ui_screen_album_list.h"
#include "ui/ui_draw.h"
#include "ui/ui_common.h"
#include "ui/ui_screen_track_list.h"  // Для ui_screen_track_list_clear_cache
#include "services/locale.h"
#include "services/net_client.h"
#include "app/app_state.h"
#include <pspctrl.h>
#include <stdio.h>

void ui_screen_album_list_handle_input(AppState *state, const InputState *input)
{
    if (input->pressed & PSP_CTRL_CROSS) {
        // Очищаем старые треки сразу при переходе, чтобы не показывать их на 1 кадр
        net_client_track_store_lock();
        app_state_free_tracks(state);
        state->track_ui.track_selected = 0;
        state->track_ui.track_scroll = 0;
        net_client_track_store_unlock();
        // Также очищаем кэш указателей на обложки треков
        ui_screen_track_list_clear_cache();
        app_state_push(state, SCREEN_TRACK_LIST);
    }
}

void ui_screen_album_list_render(void)
{
    ui_draw_clear(0xFF1A1A1A);
    ui_common_draw_header(locale_get(LOCALE_SCREEN_ALBUMS));
    ui_draw_text(16.0f, 48.0f, locale_get(LOCALE_SCREEN_EMPTY), 0xFFBBBBBB);
    ui_common_draw_prompts(LOCALE_ALBUM_OPEN_PROMPT, LOCALE_ALBUM_BACK_PROMPT);
}
