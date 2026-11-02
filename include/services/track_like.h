#ifndef SERVICES_TRACK_LIKE_H
#define SERVICES_TRACK_LIKE_H

#include "services/ym_api_reactions.h"

/* UI-thread API. Update on every screen, even while playback UI is hidden.
 * Account/track changes request sync; a running mutation keeps its own ID. */
void track_like_update(int uid, const char *track_id);
void track_like_request_toggle(int uid, const char *track_id);
YmReaction track_like_state(const char *track_id);
int track_like_is_on(const char *track_id);
int track_like_error(const char *track_id);
int track_like_quiesce(void);

#endif
