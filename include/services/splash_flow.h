#ifndef YM_SERVICES_SPLASH_FLOW_H
#define YM_SERVICES_SPLASH_FLOW_H

#include "app/app_state.h"
#include <psptypes.h>
#include <pspthreadman.h>

typedef struct AuthThreadArgs {
    const char *token;
    UserInfo *user_info;
    volatile int *result;
    volatile int *cancel;
} AuthThreadArgs;

typedef enum SplashState {
    SPLASH_STATE_STORAGE = 0,
    SPLASH_STATE_CACHE,
    SPLASH_STATE_INDEX,
    SPLASH_STATE_LOCALE,
    SPLASH_STATE_TOKEN,
    SPLASH_STATE_NET_START,
    SPLASH_STATE_NET_WAIT,
    SPLASH_STATE_WLAN_OFF,
    SPLASH_STATE_NET_ERROR,
    SPLASH_STATE_AUTH_START,
    SPLASH_STATE_AUTH_WAIT,
    SPLASH_STATE_AUTH_ERROR,
    SPLASH_STATE_READY
} SplashState;

typedef struct SplashFlow {
    const char *status;
    int ready;
    SplashState state;
    char token[256];
    u64 wlan_check_at_us;
    // Auth thread state
    SceUID auth_thread_id;
    volatile int auth_result;  // -2 = pending (not started / in progress), 0 = success, -1 = error
    volatile int auth_cancel;
    UserInfo auth_user_info;
    AuthThreadArgs auth_thread_args;  // Thread arguments (must outlive thread)
    // Retry requested while auth_thread_id hadn't reported STOPPED yet;
    // splash_flow_tick finishes the retry once it has (see splash_flow_retry).
    int retry_pending;
} SplashFlow;

void splash_flow_init(SplashFlow *flow);
void splash_flow_tick(SplashFlow *flow, AppState *app);
int splash_flow_has_error(const SplashFlow *flow);
void splash_flow_retry(SplashFlow *flow);
int splash_flow_quiesce(SplashFlow *flow);

#endif
