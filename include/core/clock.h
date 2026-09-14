#ifndef YM_CORE_CLOCK_H
#define YM_CORE_CLOCK_H

// Initialize and verify RTC availability (should be called once at startup)
// Verifies both RTC hardware availability and date sanity
// Required for TLS/SSL certificate validation (needs correct system time)
// Returns 0 on success, <0 on error (RTC unavailable or date invalid)
int clock_init(void);

#endif
