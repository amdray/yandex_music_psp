#ifndef YM_SERVICES_TIME_SYNC_H
#define YM_SERVICES_TIME_SYNC_H

#include <psptypes.h>

typedef enum TimeSyncStatus {
    TIME_SYNC_UNCHANGED = 0,
    TIME_SYNC_UPDATED = 1,
    TIME_SYNC_ERR_NETWORK = -1,
    TIME_SYNC_ERR_PROTOCOL = -2,
    TIME_SYNC_ERR_RTC = -3,
    TIME_SYNC_ERR_ARK = -4,
    TIME_SYNC_ERR_CANCELLED = -5,
    TIME_SYNC_ERR_OFFLINE = -6
} TimeSyncStatus;

typedef struct TimeSyncResult {
    int status;
    long long offset_us;
    int rtc_set_result;
} TimeSyncResult;

/* Performs one bounded SNTP synchronization against public pool servers.
 * The caller owns cancel and result for the complete duration of the call. */
int time_sync_run(volatile unsigned int *cancel, TimeSyncResult *result);

#endif /* YM_SERVICES_TIME_SYNC_H */
