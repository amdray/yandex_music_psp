#ifndef YM_SERVICES_NET_ACTIVITY_H
#define YM_SERVICES_NET_ACTIVITY_H

/* Record a socket operation result; zero and errors are not activity. */
void net_activity_record_transfer(int byte_count);
/* Event sequence, not a byte count. Safe to read from the UI thread. */
unsigned int net_activity_get_epoch(void);

#endif
