/* A module with no data types, so it can be unloaded, whose global defrag
 * callback keeps its pass open with a non-zero cursor until released. */

#include "valkeymodule.h"

static long long global_calls = 0;
static int global_hold = 1;

static void defragGlobal(ValkeyModuleDefragCtx *ctx) {
    global_calls++;
    ValkeyModule_DefragCursorSet(ctx, global_hold ? 1 : 0);
}

/* DEFRAGUNLOAD.CALLS */
static int callsCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithLongLong(ctx, global_calls);
}

/* DEFRAGUNLOAD.RELEASE */
static int releaseCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    global_hold = 0;
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "defragunload", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "defragunload.calls", callsCommand, "readonly", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "defragunload.release", releaseCommand, "readonly", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModule_RegisterDefragFunc(ctx, defragGlobal);
    return VALKEYMODULE_OK;
}
