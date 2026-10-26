/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "valkeymodule.h"

#include <stdio.h>
#include <string.h>

static char module_name[64];
static ValkeyModuleString *username;
static ValkeyModuleString *password;
static long long calls;
static long long fail_after;
static int mode;

enum {
    MODE_OK,
    MODE_ERROR,
    MODE_MISSING_PASSWORD,
    MODE_AUTO_MEMORY,
    MODE_SELF_UNLOAD
};

static int credentials(ValkeyModuleCtx *ctx, ValkeyModuleString **user, ValkeyModuleString **pass) {
    calls++;
    if (mode == MODE_ERROR || (fail_after && calls > fail_after))
        return VALKEYMODULE_ERR;
    if (mode == MODE_MISSING_PASSWORD)
        return VALKEYMODULE_OK;
    if (mode == MODE_SELF_UNLOAD) {
        ValkeyModuleCallReply *reply =
            ValkeyModule_Call(ctx, "MODULE", "Ecc", "UNLOAD", module_name);
        ValkeyModule_Assert(reply && ValkeyModule_CallReplyType(reply) ==
                                         VALKEYMODULE_REPLY_ERROR);
        size_t len;
        const char *err = ValkeyModule_CallReplyStringPtr(reply, &len);
        const char *expected = "ERR Error unloading module: the module is "
                               "executing a replication credentials callback";
        ValkeyModule_Assert(err && len == strlen(expected) &&
                            !memcmp(err, expected, len));
        ValkeyModule_FreeCallReply(reply);
    }
    if (mode == MODE_AUTO_MEMORY) {
        ValkeyModule_AutoMemory(ctx);
        if (username)
            *user = ValkeyModule_CreateStringFromString(ctx, username);
        if (password)
            *pass = ValkeyModule_CreateStringFromString(ctx, password);
    } else {
        *user = username;
        *pass = password;
    }
    return VALKEYMODULE_OK;
}

static int setCredentials(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3)
        return ValkeyModule_WrongArity(ctx);
    if (username)
        ValkeyModule_FreeString(NULL, username);
    if (password)
        ValkeyModule_FreeString(NULL, password);
    username = strcmp(ValkeyModule_StringPtrLen(argv[1], NULL), "-")
                   ? ValkeyModule_CreateStringFromString(NULL, argv[1])
                   : NULL;
    password = ValkeyModule_CreateStringFromString(NULL, argv[2]);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int setMode(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2)
        return ValkeyModule_WrongArity(ctx);
    const char *value = ValkeyModule_StringPtrLen(argv[1], NULL);
    fail_after = 0;
    if (!strcmp(value, "ok"))
        mode = MODE_OK;
    else if (!strcmp(value, "error"))
        mode = MODE_ERROR;
    else if (!strcmp(value, "missing-password"))
        mode = MODE_MISSING_PASSWORD;
    else if (!strcmp(value, "auto-memory"))
        mode = MODE_AUTO_MEMORY;
    else if (!strcmp(value, "self-unload"))
        mode = MODE_SELF_UNLOAD;
    else if (!strcmp(value, "error-after-first")) {
        mode = MODE_OK;
        fail_after = calls + 1;
    } else
        return ValkeyModule_ReplyWithError(ctx, "ERR unknown mode");
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int getCalls(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1)
        return ValkeyModule_WrongArity(ctx);
    return ValkeyModule_ReplyWithLongLong(ctx, calls);
}

static int registerAgain(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1)
        return ValkeyModule_WrongArity(ctx);
    return ValkeyModule_ReplyWithLongLong(
        ctx,
        ValkeyModule_RegisterReplicationCredentialsCallback(ctx, credentials));
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    const char *name = argc == 0   ? "replicationauth"
                       : argc == 1 ? "replicationauthtwo"
                                   : "failedprovider";
    strcpy(module_name, name);
    if (ValkeyModule_Init(ctx, module_name, 1, VALKEYMODULE_APIVER_1) !=
        VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_RegisterReplicationCredentialsCallback)
        ValkeyModule_Assert(ValkeyModule_RegisterReplicationCredentialsCallback(
                                ctx, NULL) == VALKEYMODULE_ERR);
    if (!ValkeyModule_RegisterReplicationCredentialsCallback ||
        ValkeyModule_RegisterReplicationCredentialsCallback(ctx, credentials) !=
            VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (argc > 1)
        return VALKEYMODULE_ERR; /* Exercise cleanup after a registered provider
                                    fails to load. */
    const char *commands[] = {"set", "mode", "calls", "register"};
    ValkeyModuleCmdFunc functions[] = {setCredentials, setMode, getCalls,
                                       registerAgain};
    for (int i = 0; i < 4; i++) {
        char command[96];
        snprintf(command, sizeof(command), "%s.%s", module_name, commands[i]);
        if (ValkeyModule_CreateCommand(
                ctx, command, functions[i],
                "admin allow-stale deny-script no-commandlog no-monitor", 0, 0,
                0) != VALKEYMODULE_OK)
            return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    VALKEYMODULE_NOT_USED(ctx);
    if (username)
        ValkeyModule_FreeString(NULL, username);
    if (password)
        ValkeyModule_FreeString(NULL, password);
    return VALKEYMODULE_OK;
}
