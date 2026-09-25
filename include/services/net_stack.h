#ifndef YM_SERVICES_NET_STACK_H
#define YM_SERVICES_NET_STACK_H

typedef struct {
    int pool_size;
    int callout_prio;
    int callout_stack;
    int netintr_prio;
    int netintr_stack;
    int apctl_stack_size;
    int apctl_priority;
    int wifi_profile_id;
} NetTlsNetworkConfig;

typedef enum NetStackStartResult {
    NET_STACK_START_OK = 0,
    NET_STACK_ERR_FATAL_LIVE_RUNTIME = -21001,
    NET_STACK_ERR_WLAN_OFF = -21002,
    NET_STACK_ERR_NO_PROFILE = -21003
} NetStackStartResult;

typedef enum NetSupervisorPhase {
    NET_SUPERVISOR_IDLE = 0,
    NET_SUPERVISOR_CONNECTING,
    NET_SUPERVISOR_BACKOFF,
    NET_SUPERVISOR_STUCK
} NetSupervisorPhase;

typedef struct NetStackSnapshot {
    int apctl_state;
    unsigned int disconnected_generation;
    unsigned int loss_generation;
    unsigned int got_ip_generation;
    unsigned int generation;
} NetStackSnapshot;

int net_stack_init(const NetTlsNetworkConfig *config);
int net_stack_wlan_switch_is_on(void);
int net_stack_stop_supervisor(void);
int net_stack_is_ready(void);
int net_stack_is_stuck(void);
/* Returns 0 for a stable one-hot publication, or -1 if the handler published
 * invalid state arguments / an inconsistent transition set. */
int net_stack_get_snapshot(NetStackSnapshot *out);
unsigned int net_stack_get_generation(void);
NetSupervisorPhase net_stack_get_supervisor_phase(void);

#endif
