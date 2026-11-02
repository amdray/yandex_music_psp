#ifndef YM_SERVICES_LIST_INDEX_H
#define YM_SERVICES_LIST_INDEX_H

#include <pspkernel.h>
#include <stdint.h>
#include "app/track.h"

/* Playlist membership persisted as TrackRef records under (uuid, revision).
   Metadata is stored separately. Files are PSP-local; each published revision
   contains the complete ordered membership. */
typedef struct {
    char magic[4];        /* "YMIX" */
    uint16_t version;     /* LIST_INDEX_VERSION */
    uint16_t record_size; /* sizeof(TrackRef) */
    int32_t revision;
    int32_t track_count;  /* final count; -1 while the .part is being written */
    uint32_t reserved[4];
} ListIndexHeader;

typedef struct {
    SceUID fd;
    int count;
    int revision;
    TrackRef pending[64];
    int pending_count;
    char part_path[160];
    char final_path[160];
} ListIndexWriter;

typedef struct {
    SceUID fd;
    int count;
    int revision;
} ListIndexReader;

/* Writer (worker thread). open -> append xN -> commit; abort on any failure.
   Commit finalizes the header, renames .part to the immutable name and evicts
   older revisions of the same uuid. */
int list_index_writer_open(ListIndexWriter *w, const char *uuid, int revision);
int list_index_writer_append(ListIndexWriter *w, const TrackRef *ref);
int list_index_writer_commit(ListIndexWriter *w);
void list_index_writer_abort(ListIndexWriter *w);

/* Reader. Open validates magic/version/record size/revision and that the file
   size matches the declared track count. */
int list_index_open(ListIndexReader *r, const char *uuid, int revision);
void list_index_close(ListIndexReader *r);

/* Copy references for positions [start, start+n) into refs[]; returns the number of
   records actually read (clamped to track_count), or -1 on I/O error. */
int list_index_read_range(ListIndexReader *r, int start, int n, TrackRef *refs);

/* Locate track_id, scanning outward from hint_pos (the position the track had
   when the anchor was saved). Returns the position or -1 if absent. */
int list_index_find(ListIndexReader *r, const char *track_id, int hint_pos);

#endif
