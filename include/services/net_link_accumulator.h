#ifndef YM_SERVICES_NET_LINK_ACCUMULATOR_H
#define YM_SERVICES_NET_LINK_ACCUMULATOR_H

#include <stdint.h>

#define NET_LINK_STATE_COUNT 7

#define NET_LINK_ATTEMPT_PENDING 0
#define NET_LINK_ATTEMPT_SUCCESS 1
#define NET_LINK_ATTEMPT_FAILED  2

typedef struct NetLinkAccumulator {
    volatile int32_t balance[NET_LINK_STATE_COUNT];
    volatile uint32_t disconnected_generation;
    volatile uint32_t loss_generation;
    volatile uint32_t got_ip_generation;
    volatile uint32_t generation;
    volatile uint32_t inflight;
    volatile uint32_t completion_epoch;
    volatile uint32_t publication_invalid;
    volatile uint32_t wake_pending;
} NetLinkAccumulator;

static inline int net_link_state_valid(int state)
{
    return state >= 0 && state < NET_LINK_STATE_COUNT;
}

static inline int net_link_attempt_outcome(
    int state,
    uint32_t disconnected_generation, uint32_t start_disconnected_generation,
    uint32_t got_ip_generation, uint32_t start_got_ip_generation,
    int disconnected_state, int got_ip_state)
{
    if (state == disconnected_state &&
        disconnected_generation != start_disconnected_generation)
        return NET_LINK_ATTEMPT_FAILED;
    if (state == got_ip_state && got_ip_generation != start_got_ip_generation)
        return NET_LINK_ATTEMPT_SUCCESS;
    return NET_LINK_ATTEMPT_PENDING;
}

/* The APCTL callback surrounds this with inflight/epoch publication. Every
 * mutation is commutative, so overlapping callbacks need no ordering or ring. */
static inline void net_link_accumulate(NetLinkAccumulator *state,
                                       int old_state, int new_state,
                                       int disconnected_state, int got_ip_state)
{
    if (!net_link_state_valid(old_state) || !net_link_state_valid(new_state)) {
        (void)__sync_lock_test_and_set(&state->publication_invalid, 1U);
        return;
    }
    (void)__sync_fetch_and_sub(&state->balance[old_state], 1);
    (void)__sync_fetch_and_add(&state->balance[new_state], 1);
    if (old_state != disconnected_state && new_state == disconnected_state)
        (void)__sync_fetch_and_add(&state->disconnected_generation, 1U);
    if (old_state == got_ip_state && new_state != got_ip_state) {
        (void)__sync_fetch_and_add(&state->loss_generation, 1U);
        (void)__sync_fetch_and_add(&state->generation, 1U);
    }
    if (old_state != got_ip_state && new_state == got_ip_state)
        (void)__sync_fetch_and_add(&state->got_ip_generation, 1U);
}

#endif
