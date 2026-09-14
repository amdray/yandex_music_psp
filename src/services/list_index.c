#include "services/list_index.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "core/fs.h"
#include "core/logger.h"

#define LIST_INDEX_DIR "data/cache/idx"
#define LIST_INDEX_VERSION 1

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
    h->record_size = LIST_INDEX_ID_SIZE;
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

int list_index_writer_append(ListIndexWriter *w, const char *track_id)
{
    ListIndexId record;

    if (!w || w->fd < 0 || !track_id || !track_id[0]) {
        return -1;
    }
    if (strlen(track_id) >= LIST_INDEX_ID_SIZE) {
        logLine("list_index: track id too long (%d): '%s'\n",
                (int)strlen(track_id), track_id);
        return -1;
    }

    memset(record, 0, sizeof(record));
    strcpy(record, track_id);
    if (list_index_write_fully(w->fd, record, sizeof(record)) != 0) {
        return -1;
    }
    w->count++;
    return 0;
}

int list_index_writer_commit(ListIndexWriter *w)
{
    ListIndexHeader header;
    const char *name;

    if (!w || w->fd < 0) {
        return -1;
    }

    list_index_header_fill(&header, w->revision, w->count);
    if (fs_lseek(w->fd, 0, PSP_SEEK_SET) != 0 ||
        list_index_write_fully(w->fd, &header, sizeof(header)) != 0) {
        list_index_writer_abort(w);
        return -1;
    }
    fs_close(w->fd);
    w->fd = -1;

    if (fs_rename(w->part_path, w->final_path) < 0) {
        fs_remove(w->final_path); /* sceIoRename fails if target exists */
        if (fs_rename(w->part_path, w->final_path) < 0) {
            logLine("list_index: rename failed '%s' -> '%s'\n",
                    w->part_path, w->final_path);
            fs_remove(w->part_path);
            return -1;
        }
    }

    /* Evict older revisions: keep only <uuid>_<revision>.idx. The file name
       lives at the fixed directory-prefix offset of final_path. */
    name = w->final_path + strlen(LIST_INDEX_DIR) + 1;
    {
        char prefix[96];
        const char *rev_sep = strrchr(name, '_');
        size_t prefix_len = rev_sep ? (size_t)(rev_sep - name) + 1 : 0;
        if (prefix_len > 0 && prefix_len < sizeof(prefix)) {
            memcpy(prefix, name, prefix_len);
            prefix[prefix_len] = '\0';
            fs_remove_siblings(LIST_INDEX_DIR, prefix, name);
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

    n = fs_read(r->fd, &header, sizeof(header));
    if (n != (int)sizeof(header) ||
        memcmp(header.magic, "YMIX", 4) != 0 ||
        header.version != LIST_INDEX_VERSION ||
        header.record_size != LIST_INDEX_ID_SIZE ||
        header.revision != revision ||
        header.track_count < 0 ||
        (SceOff)st.st_size != (SceOff)sizeof(header) +
            (SceOff)header.track_count * LIST_INDEX_ID_SIZE) {
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

int list_index_read_range(ListIndexReader *r, int start, int n, ListIndexId *ids)
{
    SceOff offset;
    int to_read;
    int got = 0;

    if (!r || r->fd < 0 || !ids || start < 0 || n <= 0) {
        return -1;
    }
    if (start >= r->count) {
        return 0;
    }
    to_read = n;
    if (start + to_read > r->count) {
        to_read = r->count - start;
    }

    offset = (SceOff)sizeof(ListIndexHeader) + (SceOff)start * LIST_INDEX_ID_SIZE;
    if (fs_lseek(r->fd, offset, PSP_SEEK_SET) != offset) {
        return -1;
    }

    while (got < to_read) {
        int rc = fs_read(r->fd, ids[got],
                         (size_t)(to_read - got) * LIST_INDEX_ID_SIZE);
        if (rc <= 0 || rc % LIST_INDEX_ID_SIZE != 0) {
            return -1;
        }
        got += rc / LIST_INDEX_ID_SIZE;
    }
    return to_read;
}

/* Scan one block for track_id; positions [start, start+count). */
static int list_index_scan_block(ListIndexReader *r, const char *track_id,
                                 int start, int count)
{
    ListIndexId block[LIST_INDEX_SCAN_BLOCK];
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
        if (strcmp(block[i], track_id) == 0) {
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
