#ifndef SERVICES_SYSTEM_STATUS_H
#define SERVICES_SYSTEM_STATUS_H

typedef struct SystemStatusSnapshot {
    int volume_level;
    int volume_available;
    int volume_button_down;
    int battery_percent;
    /* Power queries: 1 yes, 0 no, negative value on read error. */
    int battery_charging;
    int power_online;
    int battery_available;
} SystemStatusSnapshot;

void system_status_init(void);
void system_status_shutdown(void);
void system_status_update(int volume_button_down);
void system_status_get_snapshot(SystemStatusSnapshot *out);

#endif
