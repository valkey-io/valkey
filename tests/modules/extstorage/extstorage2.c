#include "valkeymodule.h"

/*
 * This module implements a very simple external storage.
 * It's purpose is only to test the valkey module API to implement external
 * storages.
 */

 const char *storage_name = "hellostorage2";

 int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx,
                        ValkeyModuleString **argv,
                        int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, storage_name, 1, VALKEYMODULE_APIVER_1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModule_RegisterExternalStorage(ctx, storage_name);

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    if (ValkeyModule_UnregisterExternalStorage(ctx, storage_name) != VALKEYMODULE_OK) {
        ValkeyModule_Log(ctx, "error", "Failed to unregister storage");
        return VALKEYMODULE_ERR;
    }

    return VALKEYMODULE_OK;
}
