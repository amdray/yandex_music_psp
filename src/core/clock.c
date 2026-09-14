#include "core/clock.h"

#include <pspkernel.h>
#include <psprtc.h>
#include "core/logger.h"

int clock_init(void)
{
    // Verify RTC availability - required for TLS/SSL certificate validation
    ScePspDateTime test_date;
    int rc = sceRtcGetCurrentClock(&test_date, 0);  // 0 = UTC timezone
    if (rc < 0) {
        logLine("clock: ERROR - RTC not available (rc=%d)\n", rc);
        return -1;
    }
    
    // Check that the system date is suitable for certificate validation.
    // TLS certificate validation requires correct date (certificates have validity periods)
    if (test_date.year < 2024) {
        logLine("clock: ERROR - RTC date is %d-%02d-%02d (set date and time in PSP System Settings)\n",
                test_date.year, test_date.month, test_date.day);
        return -1;
    }
    
    return 0;
}
