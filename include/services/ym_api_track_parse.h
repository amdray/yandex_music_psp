#ifndef YM_SERVICES_YM_API_TRACK_PARSE_H
#define YM_SERVICES_YM_API_TRACK_PARSE_H

#include <stddef.h>

#include <cjson/cJSON.h>

#include "app/track.h"

/* Shared full-track parser for playlist hydration and Rotor sequence[].
 *
 * Extracted from src/services/ym_api_playlists.c (hydrate path); existing
 * callers were refactored to use it without behavior change except for two
 * documented fixes below.
 *
 * Return codes for ym_api_parse_track_from_object():
 *   0  track parsed into *entry;
 *   1  item skipped by design ("error":"no-rights" or unusable id);
 *   -1 bad arguments or track_obj is not an object.
 *
 * Fixes vs the original static copy:
 *   - numeric "id" nodes are accepted (the old code filled entry->id only
 *     for string ids and left it empty for numeric ones);
 *   - numeric-or-string "albums[0].id" is accepted;
 *   - entry->available defaults to 1 and honors an explicit boolean/number
 *     "available" field (the old code always left it 0; nothing in the tree
 *     reads TrackEntry.available yet, so no caller changes behavior).
 *
 * artist_cutout_uri ("artists[].cutoutCover.uri", first non-empty in server
 * order) is intentionally NOT stored in TrackEntry: the headers under
 * include app are frozen, so it is extracted separately with
 * ym_api_parse_track_cutout_uri() and kept in the Rotor item (YmRotorSequenceItem)
 * with its own cache key, never overwriting cover_uri.
 */

#define YM_TRACK_PARSE_OK 0
#define YM_TRACK_PARSE_SKIP 1
#define YM_TRACK_PARSE_ERR (-1)

int ym_api_parse_track_from_object(cJSON *track_obj, TrackEntry *entry);

/* First non-empty artists[].cutoutCover.uri in server order.
 *   0 found (out filled), 1 absent, -1 bad args or value too long
 * (overflow is an error, not a truncation: a cut URI would only 404). */
int ym_api_parse_track_cutout_uri(cJSON *track_obj, char *out, size_t out_size);

/* Format a track "id" node (string or number) into out. 0 ok, -1 fail. */
int ym_api_parse_track_id_node(const cJSON *id_node, char *out, size_t out_size);

#endif
