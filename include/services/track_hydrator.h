#ifndef YM_SERVICES_TRACK_HYDRATOR_H
#define YM_SERVICES_TRACK_HYDRATOR_H
#include "services/ym_api.h"
#define TRACK_HYDRATOR_JOB_MAX YM_API_HYDRATE_MAX
#define TRACK_HYDRATOR_TRANSPORT_ERROR (-2)
typedef enum { TRACK_HYDRATOR_PLAYBACK, TRACK_HYDRATOR_PREPARE, TRACK_HYDRATOR_SCREEN, TRACK_HYDRATOR_CONSUMERS } TrackHydratorConsumer;
typedef void (*TrackHydratorSink)(const TrackRef *, const TrackEntry *, int position, unsigned int generation);
int track_hydrator_init(TrackHydratorSink sink);
int track_hydrator_quiesce(void);
int track_hydrator_shutdown(void);
/* 1 pending, 0 complete, -1 failed, -2 transport failed. Repeating a completed/failed demand does
 * not issue network requests. Each consumer owns one independent demand. */
int track_hydrator_request(TrackHydratorConsumer consumer, const char *token,
                           const TrackRef *refs, const int *positions,
                           int count, unsigned int generation);
void track_hydrator_cancel(TrackHydratorConsumer consumer);
#endif
