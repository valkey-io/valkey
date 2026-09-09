#include "valkeymodule.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

int cmd_publish_classic_multi(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc < 3)
        return ValkeyModule_WrongArity(ctx);
    ValkeyModule_ReplyWithArray(ctx, argc-2);
    for (int i = 2; i < argc; i++) {
        int receivers = ValkeyModule_PublishMessage(ctx, argv[1], argv[i]);
        ValkeyModule_ReplyWithLongLong(ctx, receivers);
    }
    return VALKEYMODULE_OK;
}

int cmd_publish_classic(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 3)
        return ValkeyModule_WrongArity(ctx);
    
    int receivers = ValkeyModule_PublishMessage(ctx, argv[1], argv[2]);
    ValkeyModule_ReplyWithLongLong(ctx, receivers);
    return VALKEYMODULE_OK;
}

int cmd_publish_shard(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 3)
        return ValkeyModule_WrongArity(ctx);
    
    int receivers = ValkeyModule_PublishMessageShard(ctx, argv[1], argv[2]);
    ValkeyModule_ReplyWithLongLong(ctx, receivers);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    
    if (ValkeyModule_Init(ctx,"publish",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"publish.classic",cmd_publish_classic,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"publish.classic_multi",cmd_publish_classic_multi,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"publish.shard",cmd_publish_shard,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
        
    return VALKEYMODULE_OK;
}
