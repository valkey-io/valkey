
#include "valkeymodule.h"
#include <strings.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

#define UNUSED(V) ((void) V)

/* A sample movable keys command that returns a list of all
 * arguments that follow a KEY argument, i.e.
 */
int getkeys_command(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    int i;
    int count = 0;

    /* Handle getkeys-api introspection */
    if (ValkeyModule_IsKeysPositionRequest(ctx)) {
        for (i = 0; i < argc; i++) {
            size_t len;
            const char *str = ValkeyModule_StringPtrLen(argv[i], &len);

            if (len == 3 && !strncasecmp(str, "key", 3) && i + 1 < argc)
                ValkeyModule_KeyAtPos(ctx, i + 1);
        }

        return VALKEYMODULE_OK;
    }

    /* Handle real command invocation */
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    for (i = 0; i < argc; i++) {
        size_t len;
        const char *str = ValkeyModule_StringPtrLen(argv[i], &len);

        if (len == 3 && !strncasecmp(str, "key", 3) && i + 1 < argc) {
            ValkeyModule_ReplyWithString(ctx, argv[i+1]);
            count++;
        }
    }
    ValkeyModule_ReplySetArrayLength(ctx, count);

    return VALKEYMODULE_OK;
}

int getkeys_command_with_flags(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    int i;
    int count = 0;

    /* Handle getkeys-api introspection */
    if (ValkeyModule_IsKeysPositionRequest(ctx)) {
        for (i = 0; i < argc; i++) {
            size_t len;
            const char *str = ValkeyModule_StringPtrLen(argv[i], &len);

            if (len == 3 && !strncasecmp(str, "key", 3) && i + 1 < argc)
                ValkeyModule_KeyAtPosWithFlags(ctx, i + 1, VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS);
        }

        return VALKEYMODULE_OK;
    }

    /* Handle real command invocation */
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    for (i = 0; i < argc; i++) {
        size_t len;
        const char *str = ValkeyModule_StringPtrLen(argv[i], &len);

        if (len == 3 && !strncasecmp(str, "key", 3) && i + 1 < argc) {
            ValkeyModule_ReplyWithString(ctx, argv[i+1]);
            count++;
        }
    }
    ValkeyModule_ReplySetArrayLength(ctx, count);

    return VALKEYMODULE_OK;
}

int getkeys_fixed(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    int i;

    ValkeyModule_ReplyWithArray(ctx, argc - 1);
    for (i = 1; i < argc; i++) {
        ValkeyModule_ReplyWithString(ctx, argv[i]);
    }
    return VALKEYMODULE_OK;
}

/* Introspect a command using RM_GetCommandKeys() and returns the list
 * of keys. Essentially this is COMMAND GETKEYS implemented in a module.
 * INTROSPECT <with-flags> <cmd> <args>
 */
int getkeys_introspect(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    long long with_flags = 0;

    if (argc < 4) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    if (ValkeyModule_StringToLongLong(argv[1],&with_flags) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid integer");

    int num_keys, *keyflags = NULL;
    int *keyidx = ValkeyModule_GetCommandKeysWithFlags(ctx, &argv[2], argc - 2, &num_keys, with_flags ? &keyflags : NULL);

    if (!keyidx) {
        if (!errno)
            ValkeyModule_ReplyWithEmptyArray(ctx);
        else {
            char err[100];
            switch (errno) {
                case ENOENT:
                    ValkeyModule_ReplyWithError(ctx, "ERR ENOENT");
                    break;
                case EINVAL:
                    ValkeyModule_ReplyWithError(ctx, "ERR EINVAL");
                    break;
                default:
                    snprintf(err, sizeof(err) - 1, "ERR errno=%d", errno);
                    ValkeyModule_ReplyWithError(ctx, err);
                    break;
            }
        }
    } else {
        int i;

        ValkeyModule_ReplyWithArray(ctx, num_keys);
        for (i = 0; i < num_keys; i++) {
            if (!with_flags) {
                ValkeyModule_ReplyWithString(ctx, argv[2 + keyidx[i]]);
                continue;
            }
            ValkeyModule_ReplyWithArray(ctx, 2);
            ValkeyModule_ReplyWithString(ctx, argv[2 + keyidx[i]]);
            char* sflags = "";
            if (keyflags[i] & VALKEYMODULE_CMD_KEY_RO)
                sflags = "RO";
            else if (keyflags[i] & VALKEYMODULE_CMD_KEY_RW)
                sflags = "RW";
            else if (keyflags[i] & VALKEYMODULE_CMD_KEY_OW)
                sflags = "OW";
            else if (keyflags[i] & VALKEYMODULE_CMD_KEY_RM)
                sflags = "RM";
            ValkeyModule_ReplyWithCString(ctx, sflags);
        }

        ValkeyModule_Free(keyidx);
        ValkeyModule_Free(keyflags);
    }

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (ValkeyModule_Init(ctx,"getkeys",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"getkeys.command", getkeys_command,"getkeys-api",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"getkeys.command_with_flags", getkeys_command_with_flags,"getkeys-api",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"getkeys.fixed", getkeys_fixed,"",2,4,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"getkeys.introspect", getkeys_introspect,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
