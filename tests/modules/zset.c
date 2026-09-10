#include "valkeymodule.h"
#include <math.h>
#include <errno.h>
#include <stdlib.h>
#include <strings.h>

#define UNUSED(V) ((void) V)

/* ZSET.REM key element
 *
 * Removes an occurrence of an element from a sorted set. Replies with the
 * number of removed elements (0 or 1).
 */
int zset_rem(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    int keymode = VALKEYMODULE_READ | VALKEYMODULE_WRITE;
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], keymode);
    int deleted;
    if (ValkeyModule_ZsetRem(key, argv[2], &deleted) == VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithLongLong(ctx, deleted);
    else
        return ValkeyModule_ReplyWithError(ctx, "ERR ZsetRem failed");
}

/* ZSET.ADD key score member
 *
 * Adds a specified member with the specified score to the sorted
 * set stored at key.
 */
int zset_add(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    int keymode = VALKEYMODULE_READ | VALKEYMODULE_WRITE;
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], keymode);

    size_t len;
    double score;
    char *endptr;
    const char *str = ValkeyModule_StringPtrLen(argv[2], &len);
    score = strtod(str, &endptr);
    if (*endptr != '\0' || errno == ERANGE)
        return ValkeyModule_ReplyWithError(ctx, "value is not a valid float");

    if (ValkeyModule_ZsetAdd(key, score, argv[3], NULL) == VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    else
        return ValkeyModule_ReplyWithError(ctx, "ERR ZsetAdd failed");
}

/* ZSET.INCRBY key member increment
 *
 * Increments the score stored at member in the sorted set stored at key by increment.
 * Replies with the new score of this element.
 */
int zset_incrby(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    int keymode = VALKEYMODULE_READ | VALKEYMODULE_WRITE;
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], keymode);

    size_t len;
    double score, newscore;
    char *endptr;
    const char *str = ValkeyModule_StringPtrLen(argv[3], &len);
    score = strtod(str, &endptr);
    if (*endptr != '\0' || errno == ERANGE)
        return ValkeyModule_ReplyWithError(ctx, "value is not a valid float");

    if (ValkeyModule_ZsetIncrby(key, score, argv[2], NULL, &newscore) == VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithDouble(ctx, newscore);
    else
        return ValkeyModule_ReplyWithError(ctx, "ERR ZsetIncrby failed");
}

static int zset_internal_rangebylex(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int reverse) {
    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_ZSET) {
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    if (reverse) {
        if (ValkeyModule_ZsetLastInLexRange(key, argv[2], argv[3]) != VALKEYMODULE_OK) {
            return ValkeyModule_ReplyWithError(ctx, "invalid range");
        }
    } else {
        if (ValkeyModule_ZsetFirstInLexRange(key, argv[2], argv[3]) != VALKEYMODULE_OK) {
            return ValkeyModule_ReplyWithError(ctx, "invalid range");
        }
    }

    int arraylen = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    while (!ValkeyModule_ZsetRangeEndReached(key)) {
        ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(key, NULL);
        ValkeyModule_ReplyWithString(ctx, ele);
        ValkeyModule_FreeString(ctx, ele);
        if (reverse) {
            ValkeyModule_ZsetRangePrev(key);
        } else {
            ValkeyModule_ZsetRangeNext(key);
        }
        arraylen += 1;
    }
    ValkeyModule_ZsetRangeStop(key);
    ValkeyModule_CloseKey(key);
    ValkeyModule_ReplySetArrayLength(ctx, arraylen);
    return VALKEYMODULE_OK;
}

/* ZSET.rangebylex key min max
 *
 * Returns members in a sorted set within a lexicographical range.
 */
int zset_rangebylex(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4)
        return ValkeyModule_WrongArity(ctx);
    return zset_internal_rangebylex(ctx, argv, 0);
}

/* ZSET.revrangebylex key min max
 *
 * Returns members in a sorted set within a lexicographical range in reverse order.
 */
int zset_revrangebylex(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4)
        return ValkeyModule_WrongArity(ctx);
    return zset_internal_rangebylex(ctx, argv, 1);
}

static void zset_members_cb(ValkeyModuleKey *key, ValkeyModuleString *field, ValkeyModuleString *value, void *privdata) {
    UNUSED(key);
    UNUSED(value);
    ValkeyModuleCtx *ctx = (ValkeyModuleCtx *)privdata;
    ValkeyModule_ReplyWithString(ctx, field);
}

/* ZSET.members key
 *
 * Returns members in a sorted set.
 */
int zset_members(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2)
        return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_ZSET) {
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    ValkeyModule_ReplyWithArray(ctx, ValkeyModule_ValueLength(key));
    ValkeyModuleScanCursor *c = ValkeyModule_ScanCursorCreate();
    while (ValkeyModule_ScanKey(key, c, zset_members_cb, ctx));
    ValkeyModule_CloseKey(key);
    ValkeyModule_ScanCursorDestroy(c);
    return VALKEYMODULE_OK;
}

static void zset_walk_reply_current(ValkeyModuleCtx *ctx, ValkeyModuleKey *key) {
    double score;
    ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(key, &score);
    if (ele)
        ValkeyModule_ReplyWithString(ctx, ele);
    else
        ValkeyModule_ReplyWithNull(ctx);
}

/* ZSET.WALK key score|lex|score-last|lex-last min max pattern
 *
 * Seeks the first (or, with a -last type, the last) element of the range,
 * then applies 'pattern' one character at a time ('n' = ZsetRangeNext,
 * 'p' = ZsetRangePrev). Replies with the current element after the seek and
 * after each step, or END for a step that did not move.
 */
int zset_walk(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 6) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    if (key == NULL) return ValkeyModule_ReplyWithError(ctx, "ERR no such key");

    size_t len;
    const char *type = ValkeyModule_StringPtrLen(argv[2], &len);
    int rc;
    if (!strcasecmp(type, "score") || !strcasecmp(type, "score-last")) {
        double min = strtod(ValkeyModule_StringPtrLen(argv[3], &len), NULL);
        double max = strtod(ValkeyModule_StringPtrLen(argv[4], &len), NULL);
        rc = !strcasecmp(type, "score") ? ValkeyModule_ZsetFirstInScoreRange(key, min, max, 0, 0)
                                        : ValkeyModule_ZsetLastInScoreRange(key, min, max, 0, 0);
    } else if (!strcasecmp(type, "lex") || !strcasecmp(type, "lex-last")) {
        rc = !strcasecmp(type, "lex") ? ValkeyModule_ZsetFirstInLexRange(key, argv[3], argv[4])
                                      : ValkeyModule_ZsetLastInLexRange(key, argv[3], argv[4]);
    } else {
        return ValkeyModule_ReplyWithError(ctx, "ERR range type must be score, lex, score-last or lex-last");
    }
    if (rc != VALKEYMODULE_OK) return ValkeyModule_ReplyWithError(ctx, "ERR range init failed");

    const char *pattern = ValkeyModule_StringPtrLen(argv[5], &len);
    ValkeyModule_ReplyWithArray(ctx, len + 1);
    zset_walk_reply_current(ctx, key);
    for (size_t i = 0; i < len; i++) {
        int moved = (pattern[i] == 'p') ? ValkeyModule_ZsetRangePrev(key) : ValkeyModule_ZsetRangeNext(key);
        if (moved)
            zset_walk_reply_current(ctx, key);
        else
            ValkeyModule_ReplyWithSimpleString(ctx, "END");
    }
    ValkeyModule_ZsetRangeStop(key);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "zset", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.rem", zset_rem, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.add", zset_add, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.incrby", zset_incrby, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.rangebylex", zset_rangebylex, "readonly",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.revrangebylex", zset_revrangebylex, "readonly",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.members", zset_members, "readonly",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.walk", zset_walk, "readonly",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
