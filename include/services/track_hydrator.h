#ifndef YM_SERVICES_TRACK_HYDRATOR_H
#define YM_SERVICES_TRACK_HYDRATOR_H

#include "app/track.h"
#include "services/ym_api.h"

/* Background hydration of track metadata by id. Serves two requesters with
   fixed precedence: a single playback track (the next song must start) beats
   a list window (rows the user is about to see). One pending job per slot,
   newest request replaces the older one (latest-wins) — навигация меняет
   потребность, устаревшие заявки не имеют смысла.

   For every requested id the worker first consults the on-MS metadata store;
   only the misses go to the network (one POST /tracks per job). Every result
   is written through to the store and delivered via the sink callback with
   the list position and the request generation — the consumer drops stale
   generations. The sink is called from the worker thread. */

#define TRACK_HYDRATOR_JOB_MAX YM_API_HYDRATE_MAX

typedef void (*TrackHydratorSink)(const TrackEntry *entry, int position, int generation);

int track_hydrator_init(TrackHydratorSink sink);
int track_hydrator_shutdown(void);
int track_hydrator_quiesce(void);

/* Hydrate ids[0..count) representing list positions [start, start+count).
   Token is copied. Replaces any previously pending window job. */
int track_hydrator_request_window(const char *token,
                                  const ListIndexId *ids,
                                  int start,
                                  int count,
                                  int generation);

/* Hydrate one track for playback; served before any window job. Position is
   echoed back to the sink verbatim. */
int track_hydrator_request_track(const char *token,
                                 const char *track_id,
                                 int position,
                                 int generation);

#endif
