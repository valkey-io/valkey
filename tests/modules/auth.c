/* define macros for having usleep */
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include "valkeymodule.h"

#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define UNUSED(V) ((void) V)

// A simple global user
static ValkeyModuleUser *global = NULL;
static long long client_change_delta = 0;

void UserChangedCallback(uint64_t client_id, void *privdata) {
    VALKEYMODULE_NOT_USED(privdata);
    VALKEYMODULE_NOT_USED(client_id);
    client_change_delta++;
}

int Auth_CreateModuleUser(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (global) {
        ValkeyModule_FreeModuleUser(global);
    }

    global = ValkeyModule_CreateModuleUser("global");
    ValkeyModule_SetModuleUserACL(global, "allcommands");
    ValkeyModule_SetModuleUserACL(global, "allkeys");
    ValkeyModule_SetModuleUserACL(global, "on");

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int Auth_AuthModuleUser(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    uint64_t client_id;
    ValkeyModule_AuthenticateClientWithUser(ctx, global, UserChangedCallback, NULL, &client_id);

    return ValkeyModule_ReplyWithLongLong(ctx, (uint64_t) client_id);
}

int Auth_AuthRealUser(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    size_t length;
    uint64_t client_id;

    ValkeyModuleString *user_string = argv[1];
    const char *name = ValkeyModule_StringPtrLen(user_string, &length);

    if (ValkeyModule_AuthenticateClientWithACLUser(ctx, name, length, 
            UserChangedCallback, NULL, &client_id) == VALKEYMODULE_ERR) {
        return ValkeyModule_ReplyWithError(ctx, "Invalid user");   
    }

    return ValkeyModule_ReplyWithLongLong(ctx, (uint64_t) client_id);
}

/* This command redacts every other arguments and returns OK */
int Auth_RedactedAPI(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    for(int i = argc - 1; i > 0; i -= 2) {
        int result = ValkeyModule_RedactClientCommandArgument(ctx, i);
        ValkeyModule_Assert(result == VALKEYMODULE_OK);
    }
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK"); 
}

int Auth_ChangeCount(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    long long result = client_change_delta;
    client_change_delta = 0;
    return ValkeyModule_ReplyWithLongLong(ctx, result);
}

/* The Module functionality below validates that module authentication callbacks can be registered
 * to support both non-blocking and blocking module based authentication. */

/* Non Blocking Module Auth callback / implementation. */
int auth_cb(ValkeyModuleCtx *ctx, ValkeyModuleString *username, ValkeyModuleString *password, ValkeyModuleString **err) {
    const char *user = ValkeyModule_StringPtrLen(username, NULL);
    const char *pwd = ValkeyModule_StringPtrLen(password, NULL);
    if (!strcmp(user,"foo") && !strcmp(pwd,"allow")) {
        ValkeyModule_AuthenticateClientWithACLUser(ctx, "foo", 3, NULL, NULL, NULL);
        return VALKEYMODULE_AUTH_HANDLED;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"deny")) {
        ValkeyModuleString *log = ValkeyModule_CreateString(ctx, "Module Auth", 11);
        ValkeyModule_ACLAddLogEntryByUserName(ctx, username, log, VALKEYMODULE_ACL_LOG_AUTH);
        ValkeyModule_FreeString(ctx, log);
        const char *err_msg = "Auth denied by Misc Module.";
        *err = ValkeyModule_CreateString(ctx, err_msg, strlen(err_msg));
        return VALKEYMODULE_AUTH_HANDLED;
    }
    return VALKEYMODULE_AUTH_NOT_HANDLED;
}

int test_rm_register_auth_cb(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModule_RegisterAuthCallback(ctx, auth_cb);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/*
 * The thread entry point that actually executes the blocking part of the AUTH command.
 * This function sleeps for 0.5 seconds and then unblocks the client which will later call
 * `AuthBlock_Reply`.
 * `arg` is expected to contain the ValkeyModuleBlockedClient, username, and password.
 */
void *AuthBlock_ThreadMain(void *arg) {
    usleep(500000);
    void **targ = arg;
    ValkeyModuleBlockedClient *bc = targ[0];
    int result = 2;
    const char *user = ValkeyModule_StringPtrLen(targ[1], NULL);
    const char *pwd = ValkeyModule_StringPtrLen(targ[2], NULL);
    if (!strcmp(user,"foo") && !strcmp(pwd,"block_allow")) {
        result = 1;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"block_deny")) {
        result = 0;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"block_abort")) {
        ValkeyModule_BlockedClientMeasureTimeEnd(bc);
        ValkeyModule_AbortBlock(bc);
        goto cleanup;
    }
    /* Provide the result to the blocking reply cb. */
    void **replyarg = ValkeyModule_Alloc(sizeof(void*));
    replyarg[0] = (void *) (uintptr_t) result;
    ValkeyModule_BlockedClientMeasureTimeEnd(bc);
    ValkeyModule_UnblockClient(bc, replyarg);
cleanup:
    /* Free the username and password and thread / arg data. */
    ValkeyModule_FreeString(NULL, targ[1]);
    ValkeyModule_FreeString(NULL, targ[2]);
    ValkeyModule_Free(targ);
    return NULL;
}

/*
 * Reply callback for a blocking AUTH command. This is called when the client is unblocked.
 */
int AuthBlock_Reply(ValkeyModuleCtx *ctx, ValkeyModuleString *username, ValkeyModuleString *password, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(password);
    void **targ = ValkeyModule_GetBlockedClientPrivateData(ctx);
    int result = (uintptr_t) targ[0];
    size_t userlen = 0;
    const char *user = ValkeyModule_StringPtrLen(username, &userlen);
    /* Handle the success case by authenticating. */
    if (result == 1) {
        ValkeyModule_AuthenticateClientWithACLUser(ctx, user, userlen, NULL, NULL, NULL);
        return VALKEYMODULE_AUTH_HANDLED;
    }
    /* Handle the Error case by denying auth */
    else if (result == 0) {
        ValkeyModuleString *log = ValkeyModule_CreateString(ctx, "Module Auth", 11);
        ValkeyModule_ACLAddLogEntryByUserName(ctx, username, log, VALKEYMODULE_ACL_LOG_AUTH);
        ValkeyModule_FreeString(ctx, log);
        const char *err_msg = "Auth denied by Misc Module.";
        *err = ValkeyModule_CreateString(ctx, err_msg, strlen(err_msg));
        return VALKEYMODULE_AUTH_HANDLED;
    }
    /* "Skip" Authentication */
    return VALKEYMODULE_AUTH_NOT_HANDLED;
}

/* Private data freeing callback for Module Auth. */
void AuthBlock_FreeData(ValkeyModuleCtx *ctx, void *privdata) {
    VALKEYMODULE_NOT_USED(ctx);
    ValkeyModule_Free(privdata);
}

/* Callback triggered when the engine attempts module auth
 * Return code here is one of the following: Auth succeeded, Auth denied,
 * Auth not handled, Auth blocked.
 * The Module can have auth succeed / denied here itself, but this is an example
 * of blocking module auth.
 */
int blocking_auth_cb(ValkeyModuleCtx *ctx, ValkeyModuleString *username, ValkeyModuleString *password, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(username);
    VALKEYMODULE_NOT_USED(password);
    VALKEYMODULE_NOT_USED(err);
    /* Block the client from the Module. */
    ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClientOnAuth(ctx, AuthBlock_Reply, AuthBlock_FreeData);
    int ctx_flags = ValkeyModule_GetContextFlags(ctx);
    if (ctx_flags & VALKEYMODULE_CTX_FLAGS_MULTI || ctx_flags & VALKEYMODULE_CTX_FLAGS_LUA) {
        /* Clean up by using ValkeyModule_UnblockClient since we attempted blocking the client. */
        ValkeyModule_UnblockClient(bc, NULL);
        return VALKEYMODULE_AUTH_HANDLED;
    }
    ValkeyModule_BlockedClientMeasureTimeStart(bc);
    pthread_t tid;
    /* Allocate memory for information needed. */
    void **targ = ValkeyModule_Alloc(sizeof(void*)*3);
    targ[0] = bc;
    targ[1] = ValkeyModule_CreateStringFromString(NULL, username);
    targ[2] = ValkeyModule_CreateStringFromString(NULL, password);
    /* Create bg thread and pass the blockedclient, username and password to it. */
    if (pthread_create(&tid, NULL, AuthBlock_ThreadMain, targ) != 0) {
        ValkeyModule_AbortBlock(bc);
    }
    return VALKEYMODULE_AUTH_HANDLED;
}

int test_rm_register_blocking_auth_cb(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModule_RegisterAuthCallback(ctx, blocking_auth_cb);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/* This function must be present on each module. It is used in order to
 * register the commands into the server. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx,"testacl",1,VALKEYMODULE_APIVER_1)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"auth.authrealuser",
        Auth_AuthRealUser,"no-auth",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"auth.createmoduleuser",
        Auth_CreateModuleUser,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"auth.authmoduleuser",
        Auth_AuthModuleUser,"no-auth",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"auth.changecount",
        Auth_ChangeCount,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"auth.redact",
        Auth_RedactedAPI,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"testmoduleone.rm_register_auth_cb",
        test_rm_register_auth_cb,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"testmoduleone.rm_register_blocking_auth_cb",
        test_rm_register_blocking_auth_cb,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    UNUSED(ctx);

    if (global)
        ValkeyModule_FreeModuleUser(global);

    return VALKEYMODULE_OK;
}
