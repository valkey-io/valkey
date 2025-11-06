#define _XOPEN_SOURCE 700
#include "valkeymodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define UNUSED(x) (void)(x)

/* Reply callback for blocking command BLOCK.DEBUG */
int HelloBlock_Reply(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    int *myint = ValkeyModule_GetBlockedClientPrivateData(ctx);
    return ValkeyModule_ReplyWithLongLong(ctx,*myint);
}

/* Timeout callback for blocking command BLOCK.DEBUG */
int HelloBlock_Timeout(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModuleBlockedClient *bc = ValkeyModule_GetBlockedClientHandle(ctx);
    ValkeyModule_BlockedClientMeasureTimeEnd(bc);
    return ValkeyModule_ReplyWithSimpleString(ctx,"Request timedout");
}

/* Private data freeing callback for BLOCK.DEBUG command. */
void HelloBlock_FreeData(ValkeyModuleCtx *ctx, void *privdata) {
    UNUSED(ctx);
    ValkeyModule_Free(privdata);
}

/* Private data freeing callback for BLOCK.BLOCK command. */
void HelloBlock_FreeStringData(ValkeyModuleCtx *ctx, void *privdata) {
    ValkeyModule_FreeString(ctx, (ValkeyModuleString*)privdata);
}

/* The thread entry point that actually executes the blocking part
 * of the command BLOCK.DEBUG. */
void *BlockDebug_ThreadMain(void *arg) {
    void **targ = arg;
    ValkeyModuleBlockedClient *bc = targ[0];
    long long delay = (unsigned long)targ[1];
    long long enable_time_track = (unsigned long)targ[2];
    if (enable_time_track)
        ValkeyModule_BlockedClientMeasureTimeStart(bc);
    ValkeyModule_Free(targ);

    struct timespec ts;
    ts.tv_sec = delay / 1000;
    ts.tv_nsec = (delay % 1000) * 1000000;
    nanosleep(&ts, NULL);
    int *r = ValkeyModule_Alloc(sizeof(int));
    *r = rand();
    if (enable_time_track)
        ValkeyModule_BlockedClientMeasureTimeEnd(bc);
    ValkeyModule_UnblockClient(bc,r);
    return NULL;
}

/* The thread entry point that actually executes the blocking part
 * of the command BLOCK.DOUBLE_DEBUG. */
void *DoubleBlock_ThreadMain(void *arg) {
    void **targ = arg;
    ValkeyModuleBlockedClient *bc = targ[0];
    long long delay = (unsigned long)targ[1];
    ValkeyModule_BlockedClientMeasureTimeStart(bc);
    ValkeyModule_Free(targ);
    struct timespec ts;
    ts.tv_sec = delay / 1000;
    ts.tv_nsec = (delay % 1000) * 1000000;
    nanosleep(&ts, NULL);
    int *r = ValkeyModule_Alloc(sizeof(int));
    *r = rand();
    ValkeyModule_BlockedClientMeasureTimeEnd(bc);
    /* call again ValkeyModule_BlockedClientMeasureTimeStart() and
     * ValkeyModule_BlockedClientMeasureTimeEnd and ensure that the
     * total execution time is 2x the delay. */
    ValkeyModule_BlockedClientMeasureTimeStart(bc);
    nanosleep(&ts, NULL);
    ValkeyModule_BlockedClientMeasureTimeEnd(bc);

    ValkeyModule_UnblockClient(bc,r);
    return NULL;
}

void HelloBlock_Disconnected(ValkeyModuleCtx *ctx, ValkeyModuleBlockedClient *bc) {
    ValkeyModule_Log(ctx,"warning","Blocked client %p disconnected!",
        (void*)bc);
}

/* BLOCK.DEBUG <delay_ms> <timeout_ms> -- Block for <count> milliseconds, then reply with
 * a random number. Timeout is the command timeout, so that you can test
 * what happens when the delay is greater than the timeout. */
int HelloBlock_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    long long delay;
    long long timeout;

    if (ValkeyModule_StringToLongLong(argv[1],&delay) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid count");
    }

    if (ValkeyModule_StringToLongLong(argv[2],&timeout) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid count");
    }

    pthread_t tid;
    ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx,HelloBlock_Reply,HelloBlock_Timeout,HelloBlock_FreeData,timeout);

    /* Here we set a disconnection handler, however since this module will
     * block in sleep() in a thread, there is not much we can do in the
     * callback, so this is just to show you the API. */
    ValkeyModule_SetDisconnectCallback(bc,HelloBlock_Disconnected);

    /* Now that we setup a blocking client, we need to pass the control
     * to the thread. However we need to pass arguments to the thread:
     * the delay and a reference to the blocked client handle. */
    void **targ = ValkeyModule_Alloc(sizeof(void*)*3);
    targ[0] = bc;
    targ[1] = (void*)(unsigned long) delay;
    // pass 1 as flag to enable time tracking
    targ[2] = (void*)(unsigned long) 1;

    if (pthread_create(&tid,NULL,BlockDebug_ThreadMain,targ) != 0) {
        ValkeyModule_AbortBlock(bc);
        return ValkeyModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return VALKEYMODULE_OK;
}

/* BLOCK.DEBUG_NOTRACKING <delay_ms> <timeout_ms> -- Block for <count> milliseconds, then reply with
 * a random number. Timeout is the command timeout, so that you can test
 * what happens when the delay is greater than the timeout.
 * this command does not track background time so the background time should no appear in stats*/
int HelloBlockNoTracking_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    long long delay;
    long long timeout;

    if (ValkeyModule_StringToLongLong(argv[1],&delay) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid count");
    }

    if (ValkeyModule_StringToLongLong(argv[2],&timeout) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid count");
    }

    pthread_t tid;
    ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx,HelloBlock_Reply,HelloBlock_Timeout,HelloBlock_FreeData,timeout);

    /* Here we set a disconnection handler, however since this module will
     * block in sleep() in a thread, there is not much we can do in the
     * callback, so this is just to show you the API. */
    ValkeyModule_SetDisconnectCallback(bc,HelloBlock_Disconnected);

    /* Now that we setup a blocking client, we need to pass the control
     * to the thread. However we need to pass arguments to the thread:
     * the delay and a reference to the blocked client handle. */
    void **targ = ValkeyModule_Alloc(sizeof(void*)*3);
    targ[0] = bc;
    targ[1] = (void*)(unsigned long) delay;
    // pass 0 as flag to enable time tracking
    targ[2] = (void*)(unsigned long) 0;

    if (pthread_create(&tid,NULL,BlockDebug_ThreadMain,targ) != 0) {
        ValkeyModule_AbortBlock(bc);
        return ValkeyModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return VALKEYMODULE_OK;
}

/* BLOCK.DOUBLE_DEBUG <delay_ms> -- Block for 2 x <count> milliseconds,
 * then reply with a random number.
 * This command is used to test multiple calls to ValkeyModule_BlockedClientMeasureTimeStart()
 * and ValkeyModule_BlockedClientMeasureTimeEnd() within the same execution. */
int HelloDoubleBlock_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    long long delay;

    if (ValkeyModule_StringToLongLong(argv[1],&delay) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid count");
    }

    pthread_t tid;
    ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx,HelloBlock_Reply,HelloBlock_Timeout,HelloBlock_FreeData,0);

    /* Now that we setup a blocking client, we need to pass the control
     * to the thread. However we need to pass arguments to the thread:
     * the delay and a reference to the blocked client handle. */
    void **targ = ValkeyModule_Alloc(sizeof(void*)*2);
    targ[0] = bc;
    targ[1] = (void*)(unsigned long) delay;

    if (pthread_create(&tid,NULL,DoubleBlock_ThreadMain,targ) != 0) {
        ValkeyModule_AbortBlock(bc);
        return ValkeyModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return VALKEYMODULE_OK;
}

ValkeyModuleBlockedClient *blocked_client = NULL;

/* BLOCK.BLOCK [TIMEOUT] -- Blocks the current client until released
 * or TIMEOUT seconds. If TIMEOUT is zero, no timeout function is
 * registered.
 */
int Block_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_IsBlockedReplyRequest(ctx)) {
        ValkeyModuleString *r = ValkeyModule_GetBlockedClientPrivateData(ctx);
        return ValkeyModule_ReplyWithString(ctx, r);
    } else if (ValkeyModule_IsBlockedTimeoutRequest(ctx)) {
        ValkeyModule_UnblockClient(blocked_client, NULL); /* Must be called to avoid leaks. */
        blocked_client = NULL;
        return ValkeyModule_ReplyWithSimpleString(ctx, "Timed out");
    }

    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    long long timeout;

    if (ValkeyModule_StringToLongLong(argv[1], &timeout) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid timeout");
    }
    if (blocked_client) {
        return ValkeyModule_ReplyWithError(ctx, "ERR another client already blocked");
    }

    /* Block client. We use this function as both a reply and optional timeout
     * callback and differentiate the different code flows above.
     */
    blocked_client = ValkeyModule_BlockClient(ctx, Block_RedisCommand,
            timeout > 0 ? Block_RedisCommand : NULL, HelloBlock_FreeStringData, timeout);
    return VALKEYMODULE_OK;
}

/* BLOCK.IS_BLOCKED -- Returns 1 if we have a blocked client, or 0 otherwise.
 */
int IsBlocked_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_ReplyWithLongLong(ctx, blocked_client ? 1 : 0);
    return VALKEYMODULE_OK;
}

/* BLOCK.RELEASE [reply] -- Releases the blocked client and produce the specified reply.
 */
int Release_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    if (!blocked_client) {
        return ValkeyModule_ReplyWithError(ctx, "ERR No blocked client");
    }

    ValkeyModuleString *replystr = argv[1];
    ValkeyModule_RetainString(ctx, replystr);
    ValkeyModule_UnblockClient(blocked_client, replystr);
    blocked_client = NULL;

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if (ValkeyModule_Init(ctx,"block",1,VALKEYMODULE_APIVER_1)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"block.debug",
        HelloBlock_RedisCommand,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"block.double_debug",
        HelloDoubleBlock_RedisCommand,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"block.debug_no_track",
        HelloBlockNoTracking_RedisCommand,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "block.block",
        Block_RedisCommand, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"block.is_blocked",
        IsBlocked_RedisCommand,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"block.release",
        Release_RedisCommand,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
