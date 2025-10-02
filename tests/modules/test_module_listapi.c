/* ListAPI -- An example of module list API
 *
 * This module implements a volatile queue on top of the list exported by the
 * modules API.
 *
 * -----------------------------------------------------------------------------
 */

#include "valkeymodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ValkeyModuleList *Queue;

/* LISTAPI.ADD <value>
 *
 * Adds a value onto the queue. */
int cmd_ADD(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_ListAddToTail(Queue, argv[1]);
    /* We need to keep a reference to the value stored at the key, otherwise
     * it would be freed when this callback returns. */
    ValkeyModule_RetainString(NULL, argv[1]);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* LISTAPI.SIZE
 *
 * Return the queue length. */
int cmd_SIZE(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    long long size = ValkeyModule_ListLength(Queue);
    return ValkeyModule_ReplyWithLongLong(ctx, size);
}

/* LISTAPI.RANGE <count>
 *
 * Return a list of values in the queue in order. No more than 'count' items
 * are emitted. */
int cmd_RANGE(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    long long count;
    if (ValkeyModule_StringToLongLong(argv[1], &count) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid count");
    }

    ValkeyModuleListIter *iter = ValkeyModule_ListGetIter(Queue, VALKEYMODULE_LIST_HEAD);

    ValkeyModuleString *value;
    long long replylen = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    while ((value = ValkeyModule_ListIterNext(iter)) != NULL) {
        if (replylen >= count) break;
        size_t val_str_len;
        const char *val_str = ValkeyModule_StringPtrLen(value, &val_str_len);
        ValkeyModule_ReplyWithStringBuffer(ctx, val_str, val_str_len);
        replylen++;
    }
    ValkeyModule_ReplySetArrayLength(ctx, replylen);

    ValkeyModule_ListReleaseIter(iter);
    return VALKEYMODULE_OK;
}

static void delete_queue(ValkeyModuleCtx *ctx) {
    ValkeyModuleString *value;
    ValkeyModuleListIter *iter = ValkeyModule_ListGetIter(Queue, VALKEYMODULE_LIST_HEAD);
    while ((value = ValkeyModule_ListIterNext(iter)) != NULL) {
        ValkeyModule_FreeString(NULL, value);
    }
    ValkeyModule_ListReleaseIter(iter);

    ValkeyModule_ListFree(ctx, Queue);
}

/* LISTAPI.RESET
 *
 * Resets the queue. Removes all values from the queue. */
int cmd_RESET(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);

    delete_queue(ctx);

    Queue = ValkeyModule_ListCreate(NULL);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "listapi", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "listapi.add", cmd_ADD, "write deny-oom", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "listapi.size", cmd_SIZE, "readonly", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "listapi.range", cmd_RANGE, "readonly", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "listapi.reset", cmd_RESET, "write deny-oom", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Create our global list. Here we'll set our keys and values. */
    Queue = ValkeyModule_ListCreate(NULL);

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    delete_queue(ctx);
    return VALKEYMODULE_OK;
}
