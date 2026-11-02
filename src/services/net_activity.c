/* Файл зафиксирован и проверен. */
#include "services/net_activity.h"

static volatile unsigned int s_transfer_epoch;

void net_activity_record_transfer(int byte_count)
{
    if (byte_count > 0)
        (void)__sync_fetch_and_add(&s_transfer_epoch, 1U);
}

unsigned int net_activity_get_epoch(void)
{
    return __sync_fetch_and_add(&s_transfer_epoch, 0U);
}
