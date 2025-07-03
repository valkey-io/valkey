#include "valkeymodule.h"

int hash_extern(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);

    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);

    size_t buf_len;
    const char *buf = ValkeyModule_StringPtrLen(argv[3], &buf_len);

    return ValkeyModule_HashExternalize(key, argv[2], buf, buf_len);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "hash.extern", 1, VALKEYMODULE_APIVER_1) ==
        VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "hash.extern", hash_extern, "write",
                                  1, 1, 1) == VALKEYMODULE_OK) {
        return VALKEYMODULE_OK;
    }
    return VALKEYMODULE_ERR;
}
