/* define macros for having usleep */
#define _BSD_SOURCE
#define _DEFAULT_SOURCE
#include <unistd.h>

#include "valkeymodule.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <strings.h>

#define UNUSED(V) ((void) V)

/* used to test processing events during slow bg operation */
static volatile int g_slow_bg_operation = 0;
static volatile int g_is_in_slow_bg_operation = 0;

void *sub_worker(void *arg) {
    // Get module context
    ValkeyModuleCtx *ctx = (ValkeyModuleCtx *)arg;

    // Try acquiring GIL
    int res = ValkeyModule_ThreadSafeContextTryLock(ctx);

    // GIL is already taken by the calling thread expecting to fail.
    assert(res != VALKEYMODULE_OK);

    return NULL;
}

void *worker(void *arg) {
    // Retrieve blocked client
    ValkeyModuleBlockedClient *bc = (ValkeyModuleBlockedClient *)arg;

    // Get module context
    ValkeyModuleCtx *ctx = ValkeyModule_GetThreadSafeContext(bc);

    // Acquire GIL
    ValkeyModule_ThreadSafeContextLock(ctx);

    // Create another thread which will try to acquire the GIL
    pthread_t tid;
    int res = pthread_create(&tid, NULL, sub_worker, ctx);
    assert(res == 0);

    // Wait for thread
    pthread_join(tid, NULL);

    // Release GIL
    ValkeyModule_ThreadSafeContextUnlock(ctx);

    // Reply to client
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");

    // Unblock client
    ValkeyModule_UnblockClient(bc, NULL);

    // Free the module context
    ValkeyModule_FreeThreadSafeContext(ctx);

    return NULL;
}

int acquire_gil(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);

    int flags = ValkeyModule_GetContextFlags(ctx);
    int allFlags = ValkeyModule_GetContextFlagsAll();
    if ((allFlags & VALKEYMODULE_CTX_FLAGS_MULTI) &&
        (flags & VALKEYMODULE_CTX_FLAGS_MULTI)) {
        ValkeyModule_ReplyWithSimpleString(ctx, "Blocked client is not supported inside multi");
        return VALKEYMODULE_OK;
    }

    if ((allFlags & VALKEYMODULE_CTX_FLAGS_DENY_BLOCKING) &&
        (flags & VALKEYMODULE_CTX_FLAGS_DENY_BLOCKING)) {
        ValkeyModule_ReplyWithSimpleString(ctx, "Blocked client is not allowed");
        return VALKEYMODULE_OK;
    }

    /* This command handler tries to acquire the GIL twice
     * once in the worker thread using "ValkeyModule_ThreadSafeContextLock"
     * second in the sub-worker thread
     * using "ValkeyModule_ThreadSafeContextTryLock"
     * as the GIL is already locked. */
    ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, 0);

    pthread_t tid;
    int res = pthread_create(&tid, NULL, worker, bc);
    assert(res == 0);

    return VALKEYMODULE_OK;
}

typedef struct {
    ValkeyModuleString **argv;
    int argc;
    ValkeyModuleBlockedClient *bc;
} bg_call_data;

void *bg_call_worker(void *arg) {
    bg_call_data *bg = arg;
    ValkeyModuleBlockedClient *bc = bg->bc;

    // Get module context
    ValkeyModuleCtx *ctx = ValkeyModule_GetThreadSafeContext(bg->bc);

    // Acquire GIL
    ValkeyModule_ThreadSafeContextLock(ctx);

    // Test slow operation yielding
    if (g_slow_bg_operation) {
        g_is_in_slow_bg_operation = 1;
        while (g_slow_bg_operation) {
            ValkeyModule_Yield(ctx, VALKEYMODULE_YIELD_FLAG_CLIENTS, "Slow module operation");
            usleep(1000);
        }
        g_is_in_slow_bg_operation = 0;
    }

    // Call the command
    const char *module_cmd = ValkeyModule_StringPtrLen(bg->argv[0], NULL);
    int cmd_pos = 1;
    ValkeyModuleString *format_valkey_str = ValkeyModule_CreateString(NULL, "v", 1);
    if (!strcasecmp(module_cmd, "do_bg_rm_call_format")) {
        cmd_pos = 2;
        size_t format_len;
        const char *format = ValkeyModule_StringPtrLen(bg->argv[1], &format_len);
        ValkeyModule_StringAppendBuffer(NULL, format_valkey_str, format, format_len);
        ValkeyModule_StringAppendBuffer(NULL, format_valkey_str, "E", 1);
    }
    const char *format = ValkeyModule_StringPtrLen(format_valkey_str, NULL);
    const char *cmd = ValkeyModule_StringPtrLen(bg->argv[cmd_pos], NULL);
    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, cmd, format, bg->argv + cmd_pos + 1, bg->argc - cmd_pos - 1);
    ValkeyModule_FreeString(NULL, format_valkey_str);

    /* Free the arguments within GIL to prevent simultaneous freeing in main thread. */
    for (int i=0; i<bg->argc; i++)
        ValkeyModule_FreeString(ctx, bg->argv[i]);
    ValkeyModule_Free(bg->argv);
    ValkeyModule_Free(bg);

    // Release GIL
    ValkeyModule_ThreadSafeContextUnlock(ctx);

    // Reply to client
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    // Unblock client
    ValkeyModule_UnblockClient(bc, NULL);

    // Free the module context
    ValkeyModule_FreeThreadSafeContext(ctx);

    return NULL;
}

int do_bg_rm_call(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);

    /* Make sure we're not trying to block a client when we shouldn't */
    int flags = ValkeyModule_GetContextFlags(ctx);
    int allFlags = ValkeyModule_GetContextFlagsAll();
    if ((allFlags & VALKEYMODULE_CTX_FLAGS_MULTI) &&
        (flags & VALKEYMODULE_CTX_FLAGS_MULTI)) {
        ValkeyModule_ReplyWithSimpleString(ctx, "Blocked client is not supported inside multi");
        return VALKEYMODULE_OK;
    }
    if ((allFlags & VALKEYMODULE_CTX_FLAGS_DENY_BLOCKING) &&
        (flags & VALKEYMODULE_CTX_FLAGS_DENY_BLOCKING)) {
        ValkeyModule_ReplyWithSimpleString(ctx, "Blocked client is not allowed");
        return VALKEYMODULE_OK;
    }

    /* Make a copy of the arguments and pass them to the thread. */
    bg_call_data *bg = ValkeyModule_Alloc(sizeof(bg_call_data));
    bg->argv = ValkeyModule_Alloc(sizeof(ValkeyModuleString*)*argc);
    bg->argc = argc;
    for (int i=0; i<argc; i++)
        bg->argv[i] = ValkeyModule_HoldString(ctx, argv[i]);

    /* Block the client */
    bg->bc = ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, 0);

    /* Start a thread to handle the request */
    pthread_t tid;
    int res = pthread_create(&tid, NULL, bg_call_worker, bg);
    assert(res == 0);

    return VALKEYMODULE_OK;
}

int do_rm_call(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return ValkeyModule_WrongArity(ctx);
    }

    const char* cmd = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, "Ev", argv + 2, argc - 2);
    if(!rep){
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

static void rm_call_async_send_reply(ValkeyModuleCtx *ctx, ValkeyModuleCallReply *reply) {
    ValkeyModule_ReplyWithCallReply(ctx, reply);
    ValkeyModule_FreeCallReply(reply);
}

/* Called when the command that was blocked on 'RM_Call' gets unblocked
 * and send the reply to the blocked client. */
static void rm_call_async_on_unblocked(ValkeyModuleCtx *ctx, ValkeyModuleCallReply *reply, void *private_data) {
    UNUSED(ctx);
    ValkeyModuleBlockedClient *bc = private_data;
    ValkeyModuleCtx *bctx = ValkeyModule_GetThreadSafeContext(bc);
    rm_call_async_send_reply(bctx, reply);
    ValkeyModule_FreeThreadSafeContext(bctx);
    ValkeyModule_UnblockClient(bc, ValkeyModule_BlockClientGetPrivateData(bc));
}

int do_rm_call_async_fire_and_forget(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return ValkeyModule_WrongArity(ctx);
    }
    const char* cmd = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, "!KEv", argv + 2, argc - 2);

    if(ValkeyModule_CallReplyType(rep) != VALKEYMODULE_REPLY_PROMISE) {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
    } else {
        ValkeyModule_ReplyWithSimpleString(ctx, "Blocked");
    }
    ValkeyModule_FreeCallReply(rep);

    return VALKEYMODULE_OK;
}

static void do_rm_call_async_free_pd(ValkeyModuleCtx * ctx, void *pd) {
    UNUSED(ctx);
    ValkeyModule_FreeCallReply(pd);
}

static void do_rm_call_async_disconnect(ValkeyModuleCtx *ctx, struct ValkeyModuleBlockedClient *bc) {
    UNUSED(ctx);
    ValkeyModuleCallReply* rep = ValkeyModule_BlockClientGetPrivateData(bc);
    ValkeyModule_CallReplyPromiseAbort(rep, NULL);
    ValkeyModule_FreeCallReply(rep);
    ValkeyModule_AbortBlock(bc);
}

/*
 * Callback for do_rm_call_async / do_rm_call_async_script_mode
 * Gets the command to invoke as the first argument to the command and runs it,
 * passing the rest of the arguments to the command invocation.
 * If the command got blocked, blocks the client and unblock it when the command gets unblocked,
 * this allows check the K (allow blocking) argument to RM_Call.
 */
int do_rm_call_async(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return ValkeyModule_WrongArity(ctx);
    }

    size_t format_len = 0;
    char format[6] = {0};

    if (!(ValkeyModule_GetContextFlags(ctx) & VALKEYMODULE_CTX_FLAGS_DENY_BLOCKING)) {
        /* We are allowed to block the client so we can allow RM_Call to also block us */
        format[format_len++] = 'K';
    }

    const char* invoked_cmd = ValkeyModule_StringPtrLen(argv[0], NULL);
    if (strcasecmp(invoked_cmd, "do_rm_call_async_script_mode") == 0) {
        format[format_len++] = 'S';
    }

    format[format_len++] = 'E';
    format[format_len++] = 'v';
    if (strcasecmp(invoked_cmd, "do_rm_call_async_no_replicate") != 0) {
        /* Notice, without the '!' flag we will have inconsistency between master and replica.
         * This is used only to check '!' flag correctness on blocked commands. */
        format[format_len++] = '!';
    }

    const char* cmd = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, format, argv + 2, argc - 2);

    if(ValkeyModule_CallReplyType(rep) != VALKEYMODULE_REPLY_PROMISE) {
        rm_call_async_send_reply(ctx, rep);
    } else {
        ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx, NULL, NULL, do_rm_call_async_free_pd, 0);
        ValkeyModule_SetDisconnectCallback(bc, do_rm_call_async_disconnect);
        ValkeyModule_BlockClientSetPrivateData(bc, rep);
        ValkeyModule_CallReplyPromiseSetUnblockHandler(rep, rm_call_async_on_unblocked, bc);
    }

    return VALKEYMODULE_OK;
}

typedef struct ThreadedAsyncRMCallCtx{
    ValkeyModuleBlockedClient *bc;
    ValkeyModuleCallReply *reply;
} ThreadedAsyncRMCallCtx;

void *send_async_reply(void *arg) {
    ThreadedAsyncRMCallCtx *ta_rm_call_ctx = arg;
    rm_call_async_on_unblocked(NULL, ta_rm_call_ctx->reply, ta_rm_call_ctx->bc);
    ValkeyModule_Free(ta_rm_call_ctx);
    return NULL;
}

/* Called when the command that was blocked on 'RM_Call' gets unblocked
 * and schedule a thread to send the reply to the blocked client. */
static void rm_call_async_reply_on_thread(ValkeyModuleCtx *ctx, ValkeyModuleCallReply *reply, void *private_data) {
    UNUSED(ctx);
    ThreadedAsyncRMCallCtx *ta_rm_call_ctx = ValkeyModule_Alloc(sizeof(*ta_rm_call_ctx));
    ta_rm_call_ctx->bc = private_data;
    ta_rm_call_ctx->reply = reply;
    pthread_t tid;
    int res = pthread_create(&tid, NULL, send_async_reply, ta_rm_call_ctx);
    assert(res == 0);
}

/*
 * Callback for do_rm_call_async_on_thread.
 * Gets the command to invoke as the first argument to the command and runs it,
 * passing the rest of the arguments to the command invocation.
 * If the command got blocked, blocks the client and unblock on a background thread.
 * this allows check the K (allow blocking) argument to RM_Call, and make sure that the reply
 * that passes to unblock handler is owned by the handler and are not attached to any
 * context that might be freed after the callback ends.
 */
int do_rm_call_async_on_thread(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return ValkeyModule_WrongArity(ctx);
    }

    const char* cmd = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, "KEv", argv + 2, argc - 2);

    if(ValkeyModule_CallReplyType(rep) != VALKEYMODULE_REPLY_PROMISE) {
        rm_call_async_send_reply(ctx, rep);
    } else {
        ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, 0);
        ValkeyModule_CallReplyPromiseSetUnblockHandler(rep, rm_call_async_reply_on_thread, bc);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

/* Private data for wait_and_do_rm_call_async that holds information about:
 * 1. the block client, to unblock when done.
 * 2. the arguments, contains the command to run using RM_Call */
typedef struct WaitAndDoRMCallCtx {
    ValkeyModuleBlockedClient *bc;
    ValkeyModuleString **argv;
    int argc;
} WaitAndDoRMCallCtx;

/*
 * This callback will be called when the 'wait' command invoke on 'wait_and_do_rm_call_async' will finish.
 * This callback will continue the execution flow just like 'do_rm_call_async' command.
 */
static void wait_and_do_rm_call_async_on_unblocked(ValkeyModuleCtx *ctx, ValkeyModuleCallReply *reply, void *private_data) {
    WaitAndDoRMCallCtx *wctx = private_data;
    if (ValkeyModule_CallReplyType(reply) != VALKEYMODULE_REPLY_INTEGER) {
        goto done;
    }

    if (ValkeyModule_CallReplyInteger(reply) != 1) {
        goto done;
    }

    ValkeyModule_FreeCallReply(reply);
    reply = NULL;

    const char* cmd = ValkeyModule_StringPtrLen(wctx->argv[0], NULL);
    reply = ValkeyModule_Call(ctx, cmd, "!EKv", wctx->argv + 1, wctx->argc - 1);

done:
    if(ValkeyModule_CallReplyType(reply) != VALKEYMODULE_REPLY_PROMISE) {
        ValkeyModuleCtx *bctx = ValkeyModule_GetThreadSafeContext(wctx->bc);
        rm_call_async_send_reply(bctx, reply);
        ValkeyModule_FreeThreadSafeContext(bctx);
        ValkeyModule_UnblockClient(wctx->bc, NULL);
    } else {
        ValkeyModule_CallReplyPromiseSetUnblockHandler(reply, rm_call_async_on_unblocked, wctx->bc);
        ValkeyModule_FreeCallReply(reply);
    }
    for (int i = 0 ; i < wctx->argc ; ++i) {
        ValkeyModule_FreeString(NULL, wctx->argv[i]);
    }
    ValkeyModule_Free(wctx->argv);
    ValkeyModule_Free(wctx);
}

/*
 * Callback for wait_and_do_rm_call
 * Gets the command to invoke as the first argument, runs 'wait'
 * command (using the K flag to RM_Call). Once the wait finished, runs the
 * command that was given (just like 'do_rm_call_async').
 */
int wait_and_do_rm_call_async(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return ValkeyModule_WrongArity(ctx);
    }

    int flags = ValkeyModule_GetContextFlags(ctx);
    if (flags & VALKEYMODULE_CTX_FLAGS_DENY_BLOCKING) {
        return ValkeyModule_ReplyWithError(ctx, "Err can not run wait, blocking is not allowed.");
    }

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "wait", "!EKcc", "1", "0");
    if(ValkeyModule_CallReplyType(rep) != VALKEYMODULE_REPLY_PROMISE) {
        rm_call_async_send_reply(ctx, rep);
    } else {
        ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, 0);
        WaitAndDoRMCallCtx *wctx = ValkeyModule_Alloc(sizeof(*wctx));
        *wctx = (WaitAndDoRMCallCtx){
                .bc = bc,
                .argv = ValkeyModule_Alloc((argc - 1) * sizeof(ValkeyModuleString*)),
                .argc = argc - 1,
        };

        for (int i = 1 ; i < argc ; ++i) {
            wctx->argv[i - 1] = ValkeyModule_HoldString(NULL, argv[i]);
        }
        ValkeyModule_CallReplyPromiseSetUnblockHandler(rep, wait_and_do_rm_call_async_on_unblocked, wctx);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

static void blpop_and_set_multiple_keys_on_unblocked(ValkeyModuleCtx *ctx, ValkeyModuleCallReply *reply, void *private_data) {
    /* ignore the reply */
    ValkeyModule_FreeCallReply(reply);
    WaitAndDoRMCallCtx *wctx = private_data;
    for (int i = 0 ; i < wctx->argc ; i += 2) {
        ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "set", "!ss", wctx->argv[i], wctx->argv[i + 1]);
        ValkeyModule_FreeCallReply(rep);
    }

    ValkeyModuleCtx *bctx = ValkeyModule_GetThreadSafeContext(wctx->bc);
    ValkeyModule_ReplyWithSimpleString(bctx, "OK");
    ValkeyModule_FreeThreadSafeContext(bctx);
    ValkeyModule_UnblockClient(wctx->bc, NULL);

    for (int i = 0 ; i < wctx->argc ; ++i) {
        ValkeyModule_FreeString(NULL, wctx->argv[i]);
    }
    ValkeyModule_Free(wctx->argv);
    ValkeyModule_Free(wctx);

}

/*
 * Performs a blpop command on a given list and when unblocked set multiple string keys.
 * This command allows checking that the unblock callback is performed as a unit
 * and its effect are replicated to the replica and AOF wrapped with multi exec.
 */
int blpop_and_set_multiple_keys(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2 || argc % 2 != 0){
        return ValkeyModule_WrongArity(ctx);
    }

    int flags = ValkeyModule_GetContextFlags(ctx);
    if (flags & VALKEYMODULE_CTX_FLAGS_DENY_BLOCKING) {
        return ValkeyModule_ReplyWithError(ctx, "Err can not run wait, blocking is not allowed.");
    }

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "blpop", "!EKsc", argv[1], "0");
    if(ValkeyModule_CallReplyType(rep) != VALKEYMODULE_REPLY_PROMISE) {
        rm_call_async_send_reply(ctx, rep);
    } else {
        ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, 0);
        WaitAndDoRMCallCtx *wctx = ValkeyModule_Alloc(sizeof(*wctx));
        *wctx = (WaitAndDoRMCallCtx){
                .bc = bc,
                .argv = ValkeyModule_Alloc((argc - 2) * sizeof(ValkeyModuleString*)),
                .argc = argc - 2,
        };

        for (int i = 0 ; i < argc - 2 ; ++i) {
            wctx->argv[i] = ValkeyModule_HoldString(NULL, argv[i + 2]);
        }
        ValkeyModule_CallReplyPromiseSetUnblockHandler(rep, blpop_and_set_multiple_keys_on_unblocked, wctx);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

/* simulate a blocked client replying to a thread safe context without creating a thread */
int do_fake_bg_true(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    ValkeyModuleCtx *bctx = ValkeyModule_GetThreadSafeContext(bc);

    ValkeyModule_ReplyWithBool(bctx, 1);

    ValkeyModule_FreeThreadSafeContext(bctx);
    ValkeyModule_UnblockClient(bc, NULL);

    return VALKEYMODULE_OK;
}


/* this flag is used to work with busy commands, that might take a while
 * and ability to stop the busy work with a different command*/
static volatile int abort_flag = 0;

int slow_fg_command(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    long long block_time = 0;
    if (ValkeyModule_StringToLongLong(argv[1], &block_time) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "Invalid integer value");
        return VALKEYMODULE_OK;
    }

    uint64_t start_time = ValkeyModule_MonotonicMicroseconds();
    /* when not blocking indefinitely, we don't process client commands in this test. */
    int yield_flags = block_time? VALKEYMODULE_YIELD_FLAG_NONE: VALKEYMODULE_YIELD_FLAG_CLIENTS;
    while (!abort_flag) {
        ValkeyModule_Yield(ctx, yield_flags, "Slow module operation");
        usleep(1000);
        if (block_time && ValkeyModule_MonotonicMicroseconds() - start_time > (uint64_t)block_time)
            break;
    }

    abort_flag = 0;
    ValkeyModule_ReplyWithLongLong(ctx, 1);
    return VALKEYMODULE_OK;
}

int stop_slow_fg_command(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    abort_flag = 1;
    ValkeyModule_ReplyWithLongLong(ctx, 1);
    return VALKEYMODULE_OK;
}

/* used to enable or disable slow operation in do_bg_rm_call */
static int set_slow_bg_operation(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    long long ll;
    if (ValkeyModule_StringToLongLong(argv[1], &ll) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "Invalid integer value");
        return VALKEYMODULE_OK;
    }
    g_slow_bg_operation = ll;
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/* used to test if we reached the slow operation in do_bg_rm_call */
static int is_in_slow_bg_operation(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    if (argc != 1) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    ValkeyModule_ReplyWithLongLong(ctx, g_is_in_slow_bg_operation);
    return VALKEYMODULE_OK;
}

static void timer_callback(ValkeyModuleCtx *ctx, void *data)
{
    UNUSED(ctx);

    ValkeyModuleBlockedClient *bc = data;

    // Get module context
    ValkeyModuleCtx *reply_ctx = ValkeyModule_GetThreadSafeContext(bc);

    // Reply to client
    ValkeyModule_ReplyWithSimpleString(reply_ctx, "OK");

    // Unblock client
    ValkeyModule_UnblockClient(bc, NULL);

    // Free the module context
    ValkeyModule_FreeThreadSafeContext(reply_ctx);
}

/* unblock_by_timer <period_ms> <timeout_ms>
 * period_ms is the period of the timer.
 * timeout_ms is the blocking timeout. */
int unblock_by_timer(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 3)
        return ValkeyModule_WrongArity(ctx);

    long long period;
    long long timeout;
    if (ValkeyModule_StringToLongLong(argv[1],&period) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid period");
    if (ValkeyModule_StringToLongLong(argv[2],&timeout) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid timeout");
    }

    ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, timeout);
    ValkeyModule_CreateTimer(ctx, period, timer_callback, bc);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "blockedclient", 1, VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "acquire_gil", acquire_gil, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "do_rm_call", do_rm_call,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "do_rm_call_async", do_rm_call_async,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "do_rm_call_async_on_thread", do_rm_call_async_on_thread,
                                      "write", 0, 0, 0) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "do_rm_call_async_script_mode", do_rm_call_async,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "do_rm_call_async_no_replicate", do_rm_call_async,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "do_rm_call_fire_and_forget", do_rm_call_async_fire_and_forget,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "wait_and_do_rm_call", wait_and_do_rm_call_async,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "blpop_and_set_multiple_keys", blpop_and_set_multiple_keys,
                                      "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "do_bg_rm_call", do_bg_rm_call, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "do_bg_rm_call_format", do_bg_rm_call, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "do_fake_bg_true", do_fake_bg_true, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "slow_fg_command", slow_fg_command,"", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "stop_slow_fg_command", stop_slow_fg_command,"allow-busy", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "set_slow_bg_operation", set_slow_bg_operation, "allow-busy", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "is_in_slow_bg_operation", is_in_slow_bg_operation, "allow-busy", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "unblock_by_timer", unblock_by_timer, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
