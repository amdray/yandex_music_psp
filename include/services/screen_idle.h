#ifndef YM_SERVICES_SCREEN_IDLE_H
#define YM_SERVICES_SCREEN_IDLE_H

int screen_idle_init(void);
int screen_idle_delay_s(void);
void screen_idle_cycle_delay(int direction);
int screen_idle_off_on_power(void);
void screen_idle_set_off_on_power(int enabled);
void screen_idle_service(void);

#endif
