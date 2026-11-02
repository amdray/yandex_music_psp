#ifndef YM_API_REACTIONS_H
#define YM_API_REACTIONS_H

#include "services/ym_api_types.h"

typedef enum {
    YM_REACTION_UNKNOWN = -1,
    YM_REACTION_NONE = 0,
    YM_REACTION_LIKE,
    YM_REACTION_DISLIKE
} YmReaction;

typedef struct {
    char track_id[TRACK_ID_SIZE];
    unsigned char reaction;
} YmReactionEntry;

/* Owned by the reactions worker while running, by the UI when stopped. */
typedef struct {
    YmReactionEntry *entries;
    size_t count;
    size_t capacity;
    double revision;
    int loaded;
} YmReactions;

typedef enum {
    YM_REACTION_ADD_LIKE,
    YM_REACTION_REMOVE_LIKE,
    YM_REACTION_ADD_DISLIKE,
    YM_REACTION_REMOVE_DISLIKE
} YmReactionAction;

void ym_reactions_free(YmReactions *reactions);
YmReaction ym_reactions_get(const YmReactions *reactions, const char *track_id);
/* Failed parsing/sync preserves the previous snapshot and revision. */
int ym_reactions_parse(YmReactions *reactions, const char *json, size_t length);
int ym_api_reactions_sync(YmApiContext *ctx, YmReactions *reactions);
/* Updates the local entry only after HTTP and JSON confirmation. Revision
 * remains unchanged until the following collection/sync succeeds. */
int ym_api_reaction_set(YmApiContext *ctx, int uid, YmReactions *reactions,
                        const char *track_id, YmReactionAction action);

#endif
