#include "valkeymodule.h"
#include <pthread.h>
#include <assert.h>

#define UNUSED(V) ((void) V)

ValkeyModuleUser *user = NULL;

int call_without_user(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 2) {
        return ValkeyModule_WrongArity(ctx);
    }

    const char *cmd = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, cmd, "Ev", argv + 2, argc - 2);
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }
    return VALKEYMODULE_OK;
}

int call_with_user_flag(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 3) {
        return ValkeyModule_WrongArity(ctx);
    }

    ValkeyModule_SetContextUser(ctx, user);

    /* Append Ev to the provided flags. */
    ValkeyModuleString *flags = ValkeyModule_CreateStringFromString(ctx, argv[1]);
    ValkeyModule_StringAppendBuffer(ctx, flags, "Ev", 2);

    const char* flg = ValkeyModule_StringPtrLen(flags, NULL);
    const char* cmd = ValkeyModule_StringPtrLen(argv[2], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, flg, argv + 3, argc - 3);
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }
    ValkeyModule_FreeString(ctx, flags);

    return VALKEYMODULE_OK;
}

int add_to_acl(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        return ValkeyModule_WrongArity(ctx);
    }

    size_t acl_len;
    const char *acl = ValkeyModule_StringPtrLen(argv[1], &acl_len);

    ValkeyModuleString *error;
    int ret = ValkeyModule_SetModuleUserACLString(ctx, user, acl, &error);
    if (ret) {
        size_t len;
        const char * e = ValkeyModule_StringPtrLen(error, &len);
        ValkeyModule_ReplyWithError(ctx, e);
        return VALKEYMODULE_OK;
    }

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");

    return VALKEYMODULE_OK;
}

int get_acl(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);

    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }

    ValkeyModule_Assert(user != NULL);

    ValkeyModuleString *acl = ValkeyModule_GetModuleUserACLString(user);

    ValkeyModule_ReplyWithString(ctx, acl);

    ValkeyModule_FreeString(NULL, acl);

    return VALKEYMODULE_OK;
}

int reset_user(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);

    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }

    if (user != NULL) {
        ValkeyModule_FreeModuleUser(user);
    }

    user = ValkeyModule_CreateModuleUser("module_user");

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");

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

    // Set user
    ValkeyModule_SetContextUser(ctx, user);

    // Call the command
    size_t format_len;
    ValkeyModuleString *format_valkey_str = ValkeyModule_CreateString(NULL, "v", 1);
    const char *format = ValkeyModule_StringPtrLen(bg->argv[1], &format_len);
    ValkeyModule_StringAppendBuffer(NULL, format_valkey_str, format, format_len);
    ValkeyModule_StringAppendBuffer(NULL, format_valkey_str, "E", 1);
    format = ValkeyModule_StringPtrLen(format_valkey_str, NULL);
    const char *cmd = ValkeyModule_StringPtrLen(bg->argv[2], NULL);
    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, cmd, format, bg->argv + 3, bg->argc - 3);
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

int call_with_user_bg(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
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

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx,"usercall",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"usercall.call_without_user", call_without_user,"write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"usercall.call_with_user_flag", call_with_user_flag,"write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "usercall.call_with_user_bg", call_with_user_bg, "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "usercall.add_to_acl", add_to_acl, "write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"usercall.reset_user", reset_user,"write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"usercall.get_acl", get_acl,"write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
