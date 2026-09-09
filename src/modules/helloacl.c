/* ACL API example - An example for performing custom synchronous and
 * asynchronous password authentication.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright 2019 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "../valkeymodule.h"
#include <pthread.h>
#include <unistd.h>

// A simple global user
static ValkeyModuleUser *global;
static uint64_t global_auth_client_id = 0;

/* HELLOACL.REVOKE
 * Synchronously revoke access from a user. */
int RevokeCommand_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (global_auth_client_id) {
        ValkeyModule_DeauthenticateAndCloseClient(ctx, global_auth_client_id);
        return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        return ValkeyModule_ReplyWithError(ctx, "Global user currently not used");
    }
}

/* HELLOACL.RESET
 * Synchronously delete and re-create a module user. */
int ResetCommand_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    ValkeyModule_FreeModuleUser(global);
    global = ValkeyModule_CreateModuleUser("global");
    ValkeyModule_SetModuleUserACL(global, "allcommands");
    ValkeyModule_SetModuleUserACL(global, "allkeys");
    ValkeyModule_SetModuleUserACL(global, "on");

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* Callback handler for user changes, use this to notify a module of
 * changes to users authenticated by the module */
void HelloACL_UserChanged(uint64_t client_id, void *privdata) {
    VALKEYMODULE_NOT_USED(privdata);
    VALKEYMODULE_NOT_USED(client_id);
    global_auth_client_id = 0;
}

/* HELLOACL.AUTHGLOBAL
 * Synchronously assigns a module user to the current context. */
int AuthGlobalCommand_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (global_auth_client_id) {
        return ValkeyModule_ReplyWithError(ctx, "Global user currently used");
    }

    ValkeyModule_AuthenticateClientWithUser(ctx, global, HelloACL_UserChanged, NULL, &global_auth_client_id);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

#define TIMEOUT_TIME 1000

/* Reply callback for auth command HELLOACL.AUTHASYNC */
int HelloACL_Reply(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    size_t length;

    ValkeyModuleString *user_string = ValkeyModule_GetBlockedClientPrivateData(ctx);
    const char *name = ValkeyModule_StringPtrLen(user_string, &length);

    if (ValkeyModule_AuthenticateClientWithACLUser(ctx, name, length, NULL, NULL, NULL) == VALKEYMODULE_ERR) {
        return ValkeyModule_ReplyWithError(ctx, "Invalid Username or password");
    }
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* Timeout callback for auth command HELLOACL.AUTHASYNC */
int HelloACL_Timeout(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithSimpleString(ctx, "Request timedout");
}

/* Private data frees data for HELLOACL.AUTHASYNC command. */
void HelloACL_FreeData(ValkeyModuleCtx *ctx, void *privdata) {
    VALKEYMODULE_NOT_USED(ctx);
    ValkeyModule_FreeString(NULL, privdata);
}

/* Background authentication can happen here. */
void *HelloACL_ThreadMain(void *args) {
    void **targs = args;
    ValkeyModuleBlockedClient *bc = targs[0];
    ValkeyModuleString *user = targs[1];
    ValkeyModule_Free(targs);

    ValkeyModule_UnblockClient(bc, user);
    return NULL;
}

/* HELLOACL.AUTHASYNC
 * Asynchronously assigns an ACL user to the current context. */
int AuthAsyncCommand_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    pthread_t tid;
    ValkeyModuleBlockedClient *bc =
        ValkeyModule_BlockClient(ctx, HelloACL_Reply, HelloACL_Timeout, HelloACL_FreeData, TIMEOUT_TIME);


    void **targs = ValkeyModule_Alloc(sizeof(void *) * 2);
    targs[0] = bc;
    targs[1] = ValkeyModule_CreateStringFromString(NULL, argv[1]);

    if (pthread_create(&tid, NULL, HelloACL_ThreadMain, targs) != 0) {
        ValkeyModule_AbortBlock(bc);
        return ValkeyModule_ReplyWithError(ctx, "-ERR Can't start thread");
    }

    return VALKEYMODULE_OK;
}

/* This function must be present on each module. It is used in order to
 * register the commands into the server. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "helloacl", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "helloacl.reset", ResetCommand_ValkeyCommand, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "helloacl.revoke", RevokeCommand_ValkeyCommand, "", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "helloacl.authglobal", AuthGlobalCommand_ValkeyCommand, "no-auth", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "helloacl.authasync", AuthAsyncCommand_ValkeyCommand, "no-auth", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    global = ValkeyModule_CreateModuleUser("global");
    ValkeyModule_SetModuleUserACL(global, "allcommands");
    ValkeyModule_SetModuleUserACL(global, "allkeys");
    ValkeyModule_SetModuleUserACL(global, "on");

    global_auth_client_id = 0;

    return VALKEYMODULE_OK;
}
