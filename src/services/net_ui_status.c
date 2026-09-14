#include "services/net_ui_status.h"

#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <string.h>

#include "services/net_activity.h"

static NetUiStatusSnapshot s_status;
static unsigned int s_info_generation;
static int s_info_attempted;
static unsigned long long s_strength_next_us;
static unsigned int s_seen_body_epoch;
static int s_download_frames;

void net_ui_status_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = NET_UI_DISCONNECTED;
    s_info_generation = 0;
    s_info_attempted = 0;
    s_strength_next_us = 0;
    s_seen_body_epoch = 0;
    s_download_frames = 0;
}

void net_ui_status_update(int hold, int wlan_on)
{
    NetStackSnapshot link;
    NetActivitySnapshot activity;
    unsigned long long now = sceKernelGetSystemTimeWide();

    s_status.hold = hold ? 1 : 0;
    s_status.wlan_on = wlan_on ? 1 : 0;
    s_status.supervisor_phase = net_stack_get_supervisor_phase();

    if (!s_status.wlan_on || !net_client_runtime_started()) {
        s_status.state = NET_UI_DISCONNECTED;
        s_status.apctl.valid = 0;
        s_status.strength_valid = 0;
        s_info_attempted = 0;
    } else if (net_stack_get_snapshot(&link) == 0 &&
               link.apctl_state == PSP_NET_APCTL_STATE_GOT_IP) {
        s_status.state = NET_UI_ONLINE;
        s_status.generation = link.generation;
        if (!s_info_attempted || s_info_generation != link.generation) {
            NetApctlInfo fresh;
            s_info_attempted = 1;
            s_info_generation = link.generation;
            net_client_get_apctl_info(&fresh);
            s_status.apctl = fresh;
            s_status.strength_valid = fresh.valid;
            s_strength_next_us = now + 1000000ULL;
        } else if (s_status.apctl.valid && now >= s_strength_next_us) {
            unsigned int strength;
            unsigned int generation;
            s_strength_next_us = now + 1000000ULL;
            if (net_client_get_apctl_strength(&strength, &generation) == 0 &&
                generation == s_info_generation) {
                s_status.apctl.strength = strength;
                s_status.strength_valid = 1;
            } else {
                s_status.strength_valid = 0;
            }
        }
    } else {
        s_status.apctl.valid = 0;
        s_status.strength_valid = 0;
        s_info_attempted = 0;
        s_status.state =
            s_status.supervisor_phase == NET_SUPERVISOR_STUCK
                ? NET_UI_STUCK : NET_UI_RECOVERING;
    }

    net_activity_get_snapshot(&activity);
    if (activity.body_activity_epoch != s_seen_body_epoch) {
        s_seen_body_epoch = activity.body_activity_epoch;
        s_download_frames = 2;
    }
    s_status.download_visible =
        activity.active_response_count != 0U || s_download_frames > 0;
}

void net_ui_status_on_rendered_frame(void)
{
    if (s_download_frames > 0) --s_download_frames;
}

void net_ui_status_get_snapshot(NetUiStatusSnapshot *out)
{
    if (out) *out = s_status;
}

int net_ui_status_input_locked(void)
{
    return s_status.state == NET_UI_RECOVERING ||
           s_status.state == NET_UI_STUCK;
}
