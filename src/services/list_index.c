#include "services/list_index.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"

#define LIST_INDEX_DIR "data/cache/idx"
#define LIST_INDEX_VERSION 2

/* Block of records read at a time while scanning (find). */
#define LIST_INDEX_SCAN_BLOCK 64

static int list_index_paths(const char *uuid, int revision,
                            char *name, size_t name_size,
                            char *path, size_t path_size)
{
    int n;

    if (!uuid || !uuid[0]) {
        return -1;
    }
    n = snprintf(name, name_size, "%s_%d.idx", uuid, revision);
    if (n < 0 || (size_t)n >= name_size) {
        return -1;
    }
    n = snprintf(path, path_size, "%s/%s", LIST_INDEX_DIR, name);
    if (n < 0 || (size_t)n >= path_size) {
        return -1;
    }
    return 0;
}

static void list_index_header_fill(ListIndexHeader *h, int revision, int track_count)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "YMIX", 4);
    h->version = LIST_INDEX_VERSION;
    h->record_size = sizeof(TrackRef);
    h->revision = revision;
    h->track_count = track_count;
}

static int list_index_write_fully(SceUID fd, const void *buf, int size)
{
    const char *p = (const char *)buf;
    int remaining = size;

    while (remaining > 0) {
        int rc = fs_write(fd, p, (size_t)remaining);
        if (rc <= 0) {
            return -1;
        }
        p += rc;
        remaining -= rc;
    }
    return 0;
}

int list_index_writer_open(ListIndexWriter *w, const char *uuid, int revision)
{
    ListIndexHeader header;
    char name[96];

    if (!w) {
        return -1;
    }
    memset(w, 0, sizeof(*w));
    w->fd = -1;
    w->revision = revision;

    if (list_index_paths(uuid, revision, name, sizeof(name),
                         w->final_path, sizeof(w->final_path)) != 0) {
        return -1;
    }
    if (snprintf(w->part_path, sizeof(w->part_path), "%s.part", w->final_path) < 0 ||
        strlen(w->final_path) + 5 >= sizeof(w->part_path)) {
        return -1;
    }

    fs_ensure_dir(w->part_path);
    w->fd = fs_open(w->part_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (w->fd < 0) {
        logLine("list_index: open part failed '%s' 0x%08X\n", w->part_path, w->fd);
        return -1;
    }

    /* Placeholder header; commit rewrites it with the final count. */
    list_index_header_fill(&header, revision, -1);
    if (list_index_write_fully(w->fd, &header, sizeof(header)) != 0) {
        list_index_writer_abort(w);
        return -1;
    }
    return 0;
}

int list_index_writer_append(ListIndexWriter *w, const TrackRef *ref)
{
    if (!w || w->fd < 0 || !ref || !ref->id[0] ||
        !memchr(ref->id, '\0', sizeof(ref->id)) || ref->album_id <= 0)
        return -1;
    w->pending[w->pending_count++] = *ref;
    if (w->pending_count == (int)(sizeof(w->pending) / sizeof(w->pending[0]))) {
        if (list_index_write_fully(w->fd, w->pending, sizeof(w->pending)) != 0) return -1;
        w->pending_count = 0;
    }
    w->count++;
    return 0;
}
int list_index_writer_commit(ListIndexWriter *w)
{
    ListIndexHeader header;

    if (!w || w->fd < 0) {
        return -1;
    }

    if (w->pending_count && list_index_write_fully(w->fd, w->pending,
            w->pending_count * sizeof(w->pending[0])) != 0) {
        list_index_writer_abort(w);
        return -1;
    }
    w->pending_count = 0;
    list_index_header_fill(&header, w->revision, w->count);
    if (fs_lseek(w->fd, 0, PSP_SEEK_SET) != 0 ||
        list_index_write_fully(w->fd, &header, sizeof(header)) != 0) {
        list_index_writer_abort(w);
        return -1;
    }
    int close_rc = fs_close(w->fd);
    w->fd = -1;
    if (close_rc < 0) return -1;

    if (fs_rename(w->part_path, w->final_path) < 0) {
        fs_remove(w->final_path); /* sceIoRename fails if target exists */
        if (fs_rename(w->part_path, w->final_path) < 0) {
            logLine("list_index: rename failed '%s' -> '%s'\n",
                    w->part_path, w->final_path);
            fs_remove(w->part_path);
            return -1;
        }
    }

    return 0;
}

void list_index_writer_abort(ListIndexWriter *w)
{
    if (!w) {
        return;
    }
    if (w->fd >= 0) {
        fs_close(w->fd);
        w->fd = -1;
    }
    if (w->part_path[0]) {
        fs_remove(w->part_path);
    }
}

int list_index_open(ListIndexReader *r, const char *uuid, int revision)
{
    ListIndexHeader header;
    SceIoStat st;
    char name[96];
    char path[160];
    int n;

    if (!r) {
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->fd = -1;

    if (list_index_paths(uuid, revision, name, sizeof(name), path, sizeof(path)) != 0) {
        return -1;
    }
    if (fs_getstat(path, &st) != 0) {
        return -1; /* miss: not cached at this revision */
    }

    r->fd = fs_open(path, PSP_O_RDONLY, 0777);
    if (r->fd < 0) {
        return -1;
    }

    n = 0;
    while (n < (int)sizeof(header)) {
        int got = fs_read(r->fd, (unsigned char *)&header + n, sizeof(header) - (size_t)n);
        if (got <= 0) break;
        n += got;
    }
    if (n != (int)sizeof(header) ||
        memcmp(header.magic, "YMIX", 4) != 0 ||
        header.version != LIST_INDEX_VERSION ||
        header.record_size != sizeof(TrackRef) ||
        header.revision != revision ||
        header.track_count < 0 ||
        (SceOff)st.st_size != (SceOff)sizeof(header) +
            (SceOff)header.track_count * (SceOff)sizeof(TrackRef)) {
        logLine("list_index: invalid index '%s', removing\n", path);
        fs_close(r->fd);
        r->fd = -1;
        fs_remove(path);
        return -1;
    }

    r->count = header.track_count;
    r->revision = header.revision;
    return 0;
}

void list_index_close(ListIndexReader *r)
{
    if (r && r->fd >= 0) {
        fs_close(r->fd);
        r->fd = -1;
    }
}

int list_index_read_range(ListIndexReader *r, int start, int n, TrackRef *refs)
{
    SceOff offset;
    int to_read;
    int got = 0;

    if (!r || r->fd < 0 || !refs || start < 0 || n <= 0) {
        return -1;
    }
    if (start >= r->count) {
        return 0;
    }
    to_read = n;
    if (start + to_read > r->count) {
        to_read = r->count - start;
    }

    offset = (SceOff)sizeof(ListIndexHeader) + (SceOff)start * sizeof(TrackRef);
    if (fs_lseek(r->fd, offset, PSP_SEEK_SET) != offset) {
        return -1;
    }

    size_t bytes = (size_t)to_read * sizeof(*refs);
    while ((size_t)got < bytes) {
        int rc = fs_read(r->fd, (unsigned char *)refs + got, bytes - (size_t)got);
        if (rc <= 0) return -1;
        got += rc;
    }
    for (int i = 0; i < to_read; ++i)
        if (!refs[i].id[0] || !memchr(refs[i].id, '\0', sizeof(refs[i].id)) ||
            refs[i].album_id <= 0) return -1;
    return to_read;
}

/* Scan one block for track_id; positions [start, start+count). */
static int list_index_scan_block(ListIndexReader *r, const char *track_id,
                                 int start, int count)
{
    TrackRef block[LIST_INDEX_SCAN_BLOCK];
    int got;
    int i;

    if (count > LIST_INDEX_SCAN_BLOCK) {
        count = LIST_INDEX_SCAN_BLOCK;
    }
    got = list_index_read_range(r, start, count, block);
    if (got <= 0) {
        return -1;
    }
    for (i = 0; i < got; i++) {
        if (strcmp(block[i].id, track_id) == 0) {
            return start + i;
        }
    }
    return -1;
}

int list_index_find(ListIndexReader *r, const char *track_id, int hint_pos)
{
    int below;
    int above;

    if (!r || r->fd < 0 || !track_id || !track_id[0] || r->count <= 0) {
        return -1;
    }

    if (hint_pos < 0) {
        hint_pos = 0;
    }
    if (hint_pos >= r->count) {
        hint_pos = r->count - 1;
    }

    /* Expand block-by-block outward from the hint: an externally edited list
       usually shifts the anchor by a few positions, so the hit is near. */
    below = hint_pos;
    above = hint_pos;
    while (below > 0 || above < r->count) {
        if (above < r->count) {
            int pos = list_index_scan_block(r, track_id, above,
                                            r->count - above);
            if (pos >= 0) {
                return pos;
            }
            above += LIST_INDEX_SCAN_BLOCK;
        }
        if (below > 0) {
            int start = below - LIST_INDEX_SCAN_BLOCK;
            int count = LIST_INDEX_SCAN_BLOCK;
            if (start < 0) {
                count += start;
                start = 0;
            }
            {
                int pos = list_index_scan_block(r, track_id, start, count);
                if (pos >= 0) {
                    return pos;
                }
            }
            below = start;
        }
    }
    return -1;
}
