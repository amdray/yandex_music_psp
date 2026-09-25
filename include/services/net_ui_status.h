#ifndef YM_SERVICES_NET_UI_STATUS_H
#define YM_SERVICES_NET_UI_STATUS_H

#include "services/net_client.h"
#include "services/net_stack.h"

typedef enum NetUiEffectiveState {
    NET_UI_DISCONNECTED = 0,
    NET_UI_ONLINE,
    NET_UI_RECOVERING,
    NET_UI_STUCK
} NetUiEffectiveState;

typedef struct NetUiStatusSnapshot {
    NetUiEffectiveState state;
    NetSupervisorPhase supervisor_phase;
    NetApctlInfo apctl;
    unsigned int generation;
    int strength_valid;
    int hold;
    int wlan_on;
    unsigned int download_alpha;
} NetUiStatusSnapshot;

void net_ui_status_init(void);
void net_ui_status_update(int hold, int wlan_on);
void net_ui_status_get_snapshot(NetUiStatusSnapshot *out);
int net_ui_status_input_locked(void);

#endif
