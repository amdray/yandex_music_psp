#ifndef SERVICES_TRACK_LIKE_H
#define SERVICES_TRACK_LIKE_H

/* Call from the UI thread. A request already in flight is left undisturbed. */
void track_like_request_toggle(int uid, const char *track_id);
void track_like_poll(void);
int track_like_is_on(const char *track_id);

#endif
