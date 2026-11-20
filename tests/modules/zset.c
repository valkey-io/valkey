#include "valkeymodule.h"
#include <math.h>
#include <errno.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

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
 * 'p' = ZsetRangePrev, 'e' = ZsetRangeEndReached). Replies with the current
 * element after the seek and after each step, END for a step that did not
 * move, or the flag value for 'e'.
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
        if (pattern[i] == 'e') {
            ValkeyModule_ReplyWithLongLong(ctx, ValkeyModule_ZsetRangeEndReached(key));
            continue;
        }
        int moved = (pattern[i] == 'p') ? ValkeyModule_ZsetRangePrev(key) : ValkeyModule_ZsetRangeNext(key);
        if (moved)
            zset_walk_reply_current(ctx, key);
        else
            ValkeyModule_ReplyWithSimpleString(ctx, "END");
    }
    ValkeyModule_ZsetRangeStop(key);
    return VALKEYMODULE_OK;
}

/* ZSET.CLOSE_AFTER_CALL key src
 *
 * Opens 'key' for writing, overwrites it through ValkeyModule_Call with
 * ZUNIONSTORE key 1 src (so the value type can change while the handle is
 * open), then closes the handle. Replies with the key type seen before and
 * after the Call.
 */
int zset_close_after_call(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    int before = ValkeyModule_KeyType(key);
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "ZUNIONSTORE", "sls", argv[1], 1LL, argv[2]);
    if (reply) ValkeyModule_FreeCallReply(reply);
    int after = ValkeyModule_KeyType(key);
    ValkeyModule_CloseKey(key);
    ValkeyModule_ReplyWithArray(ctx, 2);
    ValkeyModule_ReplyWithLongLong(ctx, before);
    ValkeyModule_ReplyWithLongLong(ctx, after);
    return VALKEYMODULE_OK;
}

/* ZSET.WALK_AFTER_CALL key src
 *
 * Like ZSET.CLOSE_AFTER_CALL, but after the Call it walks the sorted set
 * through the handle that was opened on the string, then closes the handle.
 * Replies with the members walked. */
int zset_walk_after_call(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "ZUNIONSTORE", "sls", argv[1], 1LL, argv[2]);
    if (reply) ValkeyModule_FreeCallReply(reply);
    long count = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    if (ValkeyModule_ZsetFirstInScoreRange(key, VALKEYMODULE_NEGATIVE_INFINITE,
                                           VALKEYMODULE_POSITIVE_INFINITE, 0, 0) == VALKEYMODULE_OK) {
        while (!ValkeyModule_ZsetRangeEndReached(key)) {
            ValkeyModule_ReplyWithString(ctx, ValkeyModule_ZsetRangeCurrentElement(key, NULL));
            count++;
            ValkeyModule_ZsetRangeNext(key);
        }
    }
    ValkeyModule_ReplySetArrayLength(ctx, count);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ZSET.ITER_ADD key score member
 *
 * Starts a full score-range scan on key, adds (score, member) through the open
 * key handle (which may convert a listpack to a btree), then replies with all
 * elements yielded from the current iterator position to the end of the range.
 */
int zset_iter_add(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    size_t len;
    double score = strtod(ValkeyModule_StringPtrLen(argv[2], &len), NULL);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    if (ValkeyModule_ZsetFirstInScoreRange(key, VALKEYMODULE_NEGATIVE_INFINITE,
                                           VALKEYMODULE_POSITIVE_INFINITE, 0, 0) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR range init failed");

    ValkeyModule_ZsetAdd(key, score, argv[3], NULL);

    long count = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    do {
        zset_walk_reply_current(ctx, key);
        count++;
    } while (ValkeyModule_ZsetRangeNext(key));
    ValkeyModule_ReplySetArrayLength(ctx, count);
    ValkeyModule_ZsetRangeStop(key);
    return VALKEYMODULE_OK;
}

/* ZSET.ITER_REM key min max
 *
 * Walks the inclusive score range [min, max] forward, deleting each visited
 * element through the open key handle before advancing with ZsetRangeNext.
 * Replies with the deleted elements in iteration order.
 */
int zset_iter_rem(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    size_t len;
    double min = strtod(ValkeyModule_StringPtrLen(argv[2], &len), NULL);
    double max = strtod(ValkeyModule_StringPtrLen(argv[3], &len), NULL);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    if (ValkeyModule_ZsetFirstInScoreRange(key, min, max, 0, 0) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR range init failed");

    long count = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    while (!ValkeyModule_ZsetRangeEndReached(key)) {
        ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(key, NULL);
        if (ele)
            ValkeyModule_ReplyWithString(ctx, ele);
        else
            ValkeyModule_ReplyWithNull(ctx);
        count++;
        if (ele) ValkeyModule_ZsetRem(key, ele, NULL);
        ValkeyModule_ZsetRangeNext(key);
    }
    ValkeyModule_ReplySetArrayLength(ctx, count);
    ValkeyModule_ZsetRangeStop(key);
    return VALKEYMODULE_OK;
}

/* ZSET.ITER_CALL_REM key start end
 *
 * Seeks the last element of key, executes ZREMRANGEBYRANK key start end via
 * ValkeyModule_Call (bypassing the key handle), and replies with
 * [current, end_reached_before_step, prev_or_END, end_reached_after_step].
 */
int zset_iter_call_rem(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    long long start, end;
    if (ValkeyModule_StringToLongLong(argv[2], &start) != VALKEYMODULE_OK ||
        ValkeyModule_StringToLongLong(argv[3], &end) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid rank");

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    if (ValkeyModule_ZsetLastInScoreRange(key, VALKEYMODULE_NEGATIVE_INFINITE,
                                          VALKEYMODULE_POSITIVE_INFINITE, 0, 0) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR range init failed");

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "ZREMRANGEBYRANK", "sll", argv[1], start, end);
    if (!rep || ValkeyModule_CallReplyType(rep) == VALKEYMODULE_REPLY_ERROR)
        return ValkeyModule_ReplyWithError(ctx, "ERR call failed");

    ValkeyModule_ReplyWithArray(ctx, 4);
    zset_walk_reply_current(ctx, key);
    ValkeyModule_ReplyWithLongLong(ctx, ValkeyModule_ZsetRangeEndReached(key));
    if (ValkeyModule_ZsetRangePrev(key))
        zset_walk_reply_current(ctx, key);
    else
        ValkeyModule_ReplyWithSimpleString(ctx, "END");
    ValkeyModule_ReplyWithLongLong(ctx, ValkeyModule_ZsetRangeEndReached(key));
    ValkeyModule_ZsetRangeStop(key);
    return VALKEYMODULE_OK;
}

/* ZSET.ITER_INCR key delta min max dir
 *
 * Walks the inclusive score range [min, max] forward (dir "fwd") or in
 * reverse (dir "rev"). At every step it replies with the current member and
 * adds delta to that member's score through the open key handle. The walk is
 * capped at 100 steps so a walk that keeps revisiting members ends. */
int zset_iter_incr(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 6) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    double delta, min, max;
    if (ValkeyModule_StringToDouble(argv[2], &delta) != VALKEYMODULE_OK ||
        ValkeyModule_StringToDouble(argv[3], &min) != VALKEYMODULE_OK ||
        ValkeyModule_StringToDouble(argv[4], &max) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid number");
    size_t len;
    int reverse = strcmp(ValkeyModule_StringPtrLen(argv[5], &len), "rev") == 0;

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    int rc = reverse ? ValkeyModule_ZsetLastInScoreRange(key, min, max, 0, 0)
                     : ValkeyModule_ZsetFirstInScoreRange(key, min, max, 0, 0);
    if (rc != VALKEYMODULE_OK) return ValkeyModule_ReplyWithError(ctx, "ERR range init failed");

    long count = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    while (!ValkeyModule_ZsetRangeEndReached(key) && count < 100) {
        ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(key, NULL);
        if (ele == NULL) break;
        ValkeyModule_ReplyWithString(ctx, ele);
        ValkeyModule_ZsetIncrby(key, delta, ele, NULL, NULL);
        count++;
        if (reverse)
            ValkeyModule_ZsetRangePrev(key);
        else
            ValkeyModule_ZsetRangeNext(key);
    }
    ValkeyModule_ReplySetArrayLength(ctx, count);
    ValkeyModule_ZsetRangeStop(key);
    return VALKEYMODULE_OK;
}

/* ZSET.ITER_MUT key kind min max dir step op [arg ...]
 *
 * Walks a score range (kind "score", min and max are doubles) or a lex range
 * (kind "lex", min and max as for ZRANGEBYLEX) forward (dir "fwd") or in
 * reverse (dir "rev"). Replies with the members visited. Right after visiting
 * the member at position step (0-based) it changes the sorted set once:
 *   add member score   ValkeyModule_ZsetAdd through the key handle
 *   rem member         ValkeyModule_ZsetRem through the key handle
 *   call cmd [arg ...] ValkeyModule_Call, bypassing the key handle
 * The walk is capped at 100 steps. */
int zset_iter_mut(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 8) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    size_t len;
    int lex = strcmp(ValkeyModule_StringPtrLen(argv[2], &len), "lex") == 0;
    int reverse = strcmp(ValkeyModule_StringPtrLen(argv[5], &len), "rev") == 0;
    long long step;
    if (ValkeyModule_StringToLongLong(argv[6], &step) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid step");
    const char *op = ValkeyModule_StringPtrLen(argv[7], &len);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    int rc;
    if (lex) {
        rc = reverse ? ValkeyModule_ZsetLastInLexRange(key, argv[3], argv[4])
                     : ValkeyModule_ZsetFirstInLexRange(key, argv[3], argv[4]);
    } else {
        double min, max;
        if (ValkeyModule_StringToDouble(argv[3], &min) != VALKEYMODULE_OK ||
            ValkeyModule_StringToDouble(argv[4], &max) != VALKEYMODULE_OK)
            return ValkeyModule_ReplyWithError(ctx, "ERR invalid number");
        rc = reverse ? ValkeyModule_ZsetLastInScoreRange(key, min, max, 0, 0)
                     : ValkeyModule_ZsetFirstInScoreRange(key, min, max, 0, 0);
    }
    if (rc != VALKEYMODULE_OK) return ValkeyModule_ReplyWithError(ctx, "ERR range init failed");

    long count = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    while (!ValkeyModule_ZsetRangeEndReached(key) && count < 100) {
        ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(key, NULL);
        if (ele == NULL) break;
        ValkeyModule_ReplyWithString(ctx, ele);
        if (count == step) {
            if (strcmp(op, "add") == 0 && argc == 10) {
                double score;
                if (ValkeyModule_StringToDouble(argv[9], &score) != VALKEYMODULE_OK) break;
                ValkeyModule_ZsetAdd(key, score, argv[8], NULL);
            } else if (strcmp(op, "rem") == 0 && argc == 9) {
                ValkeyModule_ZsetRem(key, argv[8], NULL);
            } else if (strcmp(op, "call") == 0 && argc >= 9) {
                ValkeyModuleCallReply *rep =
                    ValkeyModule_Call(ctx, ValkeyModule_StringPtrLen(argv[8], &len), "v", argv + 9, (size_t)(argc - 9));
                if (rep) ValkeyModule_FreeCallReply(rep);
            }
        }
        count++;
        if (reverse)
            ValkeyModule_ZsetRangePrev(key);
        else
            ValkeyModule_ZsetRangeNext(key);
    }
    ValkeyModule_ReplySetArrayLength(ctx, count);
    ValkeyModule_ZsetRangeStop(key);
    return VALKEYMODULE_OK;
}

/* ZSET.CUR_AFTER_INCR key member delta
 *
 * Starts a full score-range walk on key, adds delta to member's score through
 * the key handle, then replies with what ValkeyModule_ZsetRangeCurrentElement
 * returns: the member and its score, or the string NULL. */
int zset_cur_after_incr(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    double delta;
    if (ValkeyModule_StringToDouble(argv[3], &delta) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid delta");
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    if (ValkeyModule_ZsetFirstInScoreRange(key, VALKEYMODULE_NEGATIVE_INFINITE,
                                           VALKEYMODULE_POSITIVE_INFINITE, 0, 0) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR range init failed");
    ValkeyModule_ZsetIncrby(key, delta, argv[2], NULL, NULL);
    double score;
    ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(key, &score);
    if (ele == NULL) {
        ValkeyModule_ReplyWithSimpleString(ctx, "NULL");
    } else {
        ValkeyModule_ReplyWithArray(ctx, 2);
        ValkeyModule_ReplyWithString(ctx, ele);
        ValkeyModule_ReplyWithDouble(ctx, score);
    }
    ValkeyModule_ZsetRangeStop(key);
    return VALKEYMODULE_OK;
}

/* ZSET.ITER_NOTIFY key cmd [arg ...]
 *
 * Starts a full forward score-range walk on key and steps to its first
 * member. Then runs cmd through ValkeyModule_Call. Every keyspace event
 * fired while this command runs makes the module's notification callback
 * step the same walk once and record the member it reaches, if any. After
 * the call the command walks to the end. Replies with three arrays: the
 * first member, the members reached from the callback, and the members
 * reached after the call. */
static ValkeyModuleKey *notify_key = NULL;
static ValkeyModuleString *notify_seen[16];
static int notify_seen_count = 0;

static int zset_notify_step(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);
    if (notify_key == NULL || notify_seen_count == 16) return VALKEYMODULE_OK;
    ValkeyModule_ZsetRangeNext(notify_key);
    ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(notify_key, NULL);
    if (ele) notify_seen[notify_seen_count++] = ele;
    return VALKEYMODULE_OK;
}

int zset_iter_notify(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    size_t len;
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    if (ValkeyModule_ZsetFirstInScoreRange(key, VALKEYMODULE_NEGATIVE_INFINITE,
                                           VALKEYMODULE_POSITIVE_INFINITE, 0, 0) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR range init failed");
    ValkeyModule_ReplyWithArray(ctx, 3);
    ValkeyModule_ReplyWithArray(ctx, 1);
    ValkeyModule_ReplyWithString(ctx, ValkeyModule_ZsetRangeCurrentElement(key, NULL));

    notify_key = key;
    notify_seen_count = 0;
    ValkeyModuleCallReply *rep =
        ValkeyModule_Call(ctx, ValkeyModule_StringPtrLen(argv[2], &len), "v", argv + 3, (size_t)(argc - 3));
    if (rep) ValkeyModule_FreeCallReply(rep);
    notify_key = NULL;
    ValkeyModule_ReplyWithArray(ctx, notify_seen_count);
    for (int i = 0; i < notify_seen_count; i++) ValkeyModule_ReplyWithString(ctx, notify_seen[i]);

    long count = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    while (ValkeyModule_ZsetRangeNext(key) && count < 100) {
        ValkeyModule_ReplyWithString(ctx, ValkeyModule_ZsetRangeCurrentElement(key, NULL));
        count++;
    }
    ValkeyModule_ReplySetArrayLength(ctx, count);
    ValkeyModule_ZsetRangeStop(key);
    return VALKEYMODULE_OK;
}

/* ZSET.WALK_LEAK key
 *
 * Starts a walk on key and returns without closing the key handle, as a
 * module with a bug would. The server must stop the walk when the command's
 * context is freed. The handle is given to a timer that never fires, so the
 * leak checker still sees it as reachable when the server exits; a module
 * with a pending timer stays loaded. */
static void zset_walk_leak_timer(ValkeyModuleCtx *ctx, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(data);
}

int zset_walk_leak(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    if (ValkeyModule_ZsetFirstInScoreRange(key, VALKEYMODULE_NEGATIVE_INFINITE,
                                           VALKEYMODULE_POSITIVE_INFINITE, 0, 0) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR range init failed");
    ValkeyModule_CreateTimer(ctx, 3600 * 1000, zset_walk_leak_timer, key);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
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

    if (ValkeyModule_CreateCommand(ctx, "zset.close_after_call", zset_close_after_call, "write",
                                  1, 2, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.walk_after_call", zset_walk_after_call, "write",
                                  1, 2, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.iter_add", zset_iter_add, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.iter_rem", zset_iter_rem, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.iter_call_rem", zset_iter_call_rem, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.iter_incr", zset_iter_incr, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.iter_notify", zset_iter_notify, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_ALL, zset_notify_step) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.iter_mut", zset_iter_mut, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.cur_after_incr", zset_cur_after_incr, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "zset.walk_leak", zset_walk_leak, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
