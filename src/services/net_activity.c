#include "services/net_activity.h"

static volatile unsigned int s_active_response_count;
static volatile unsigned int s_body_activity_epoch;

void net_activity_begin(void)
{
    (void)__sync_fetch_and_add(&s_active_response_count, 1U);
}

void net_activity_end(void)
{
    unsigned int current;
    do {
        current = s_active_response_count;
        if (current == 0U) return;
    } while (!__sync_bool_compare_and_swap(&s_active_response_count,
                                            current, current - 1U));
}

void net_activity_body_bytes(int byte_count)
{
    if (byte_count > 0)
        (void)__sync_fetch_and_add(&s_body_activity_epoch, 1U);
}

void net_activity_get_snapshot(NetActivitySnapshot *out)
{
    if (!out) return;
    out->active_response_count =
        __sync_fetch_and_add(&s_active_response_count, 0U);
    out->body_activity_epoch =
        __sync_fetch_and_add(&s_body_activity_epoch, 0U);
}
