#include "valkeymodule.h"
#include <math.h>
#include <errno.h>

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

    if (ValkeyModule_CreateCommand(ctx, "zset.close_after_call", zset_close_after_call, "write",
                                  1, 2, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
