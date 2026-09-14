#include "services/resource_policy.h"

#include <pspthreadman.h>

#define RESOURCE_POLICY_PLAYLIST_BOOTSTRAP_SLICE_BYTES (32 * 1024)
#define RESOURCE_POLICY_PLAYLIST_BOOTSTRAP_DELAY_US    2000
#define RESOURCE_POLICY_BROWSER_COVER_DELAY_US         12000

static volatile int s_audio_active = 0;
static int s_playlist_bootstrap_bytes = 0;

void resource_policy_set_audio_active(int active)
{
    int new_active = active ? 1 : 0;

    if (s_audio_active == new_active) {
        return;
    }

    s_audio_active = new_active;
    s_playlist_bootstrap_bytes = 0;
}

int resource_policy_is_audio_active(void)
{
    return s_audio_active ? 1 : 0;
}

int resource_policy_may_start(ResourceClass cls)
{
    if (!resource_policy_is_audio_active()) {
        return 1;
    }

    if (cls == RESOURCE_CLASS_BROWSER_COVER) {
        return 1;
    }

    return 1;
}

void resource_policy_cooperate(ResourceClass cls, int bytes)
{
    if (cls == RESOURCE_CLASS_BROWSER_COVER) {
        if (resource_policy_is_audio_active()) {
            sceKernelDelayThread(RESOURCE_POLICY_BROWSER_COVER_DELAY_US);
        }
        return;
    }

    if (cls != RESOURCE_CLASS_PLAYLIST_BOOTSTRAP || bytes <= 0) {
        return;
    }

    if (!resource_policy_is_audio_active()) {
        s_playlist_bootstrap_bytes = 0;
        return;
    }

    s_playlist_bootstrap_bytes += bytes;
    if (s_playlist_bootstrap_bytes < RESOURCE_POLICY_PLAYLIST_BOOTSTRAP_SLICE_BYTES) {
        return;
    }

    s_playlist_bootstrap_bytes = 0;
    sceKernelDelayThread(RESOURCE_POLICY_PLAYLIST_BOOTSTRAP_DELAY_US);
}