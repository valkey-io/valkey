
#include "valkeymodule.h"

static void timer_callback(ValkeyModuleCtx *ctx, void *data)
{
    ValkeyModuleString *keyname = data;
    ValkeyModuleCallReply *reply;

    reply = ValkeyModule_Call(ctx, "INCR", "s", keyname);
    if (reply != NULL)
        ValkeyModule_FreeCallReply(reply);
    ValkeyModule_FreeString(ctx, keyname);
}

int test_createtimer(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 3) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    long long period;
    if (ValkeyModule_StringToLongLong(argv[1], &period) == VALKEYMODULE_ERR) {
        ValkeyModule_ReplyWithError(ctx, "Invalid time specified.");
        return VALKEYMODULE_OK;
    }

    ValkeyModuleString *keyname = argv[2];
    ValkeyModule_RetainString(ctx, keyname);

    ValkeyModuleTimerID id = ValkeyModule_CreateTimer(ctx, period, timer_callback, keyname);
    ValkeyModule_ReplyWithLongLong(ctx, id);

    return VALKEYMODULE_OK;
}

int test_gettimer(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    long long id;
    if (ValkeyModule_StringToLongLong(argv[1], &id) == VALKEYMODULE_ERR) {
        ValkeyModule_ReplyWithError(ctx, "Invalid id specified.");
        return VALKEYMODULE_OK;
    }

    uint64_t remaining;
    ValkeyModuleString *keyname;
    if (ValkeyModule_GetTimerInfo(ctx, id, &remaining, (void **)&keyname) == VALKEYMODULE_ERR) {
        ValkeyModule_ReplyWithNull(ctx);
    } else {
        ValkeyModule_ReplyWithArray(ctx, 2);
        ValkeyModule_ReplyWithString(ctx, keyname);
        ValkeyModule_ReplyWithLongLong(ctx, remaining);
    }

    return VALKEYMODULE_OK;
}

int test_stoptimer(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    long long id;
    if (ValkeyModule_StringToLongLong(argv[1], &id) == VALKEYMODULE_ERR) {
        ValkeyModule_ReplyWithError(ctx, "Invalid id specified.");
        return VALKEYMODULE_OK;
    }

    int ret = 0;
    ValkeyModuleString *keyname;
    if (ValkeyModule_StopTimer(ctx, id, (void **) &keyname) == VALKEYMODULE_OK) {
        ValkeyModule_FreeString(ctx, keyname);
        ret = 1;
    }

    ValkeyModule_ReplyWithLongLong(ctx, ret);
    return VALKEYMODULE_OK;
}


int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx,"timer",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"test.createtimer", test_createtimer,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.gettimer", test_gettimer,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.stoptimer", test_stoptimer,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
