#ifndef YM_SERVICES_RESOURCE_POLICY_H
#define YM_SERVICES_RESOURCE_POLICY_H

typedef enum {
    RESOURCE_CLASS_PLAYLIST_BOOTSTRAP = 0,
    RESOURCE_CLASS_BROWSER_COVER = 1
} ResourceClass;

void resource_policy_set_audio_active(int active);
int resource_policy_is_audio_active(void);
int resource_policy_may_start(ResourceClass cls);
void resource_policy_cooperate(ResourceClass cls, int bytes);

#endif