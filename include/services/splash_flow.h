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

typedef struct SplashFlow {
    const char *status;
    int ready;
    // Internal state
    int stage;
    char token[256];
    int net_started;
    int auth_attempted;
    // Auth thread state
    SceUID auth_thread_id;
    volatile int auth_result;  // -2 = pending (not started / in progress), 0 = success, -1 = error
    volatile int auth_cancel;
    UserInfo auth_user_info;
    AuthThreadArgs auth_thread_args;  // Thread arguments (must outlive thread)
    // Error stage: 0 = no error, 4 = token error, 5 = net error, 6 = auth error
    int error_stage;
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
