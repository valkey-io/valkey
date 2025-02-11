#include "valkeymodule.h"

/*
 * This module implements a very simple external filter.
 * It's purpose is only to test the valkey module API to implement external
 * filters.
 */

 const char *filter_name = "hellofilter1";

 int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx,
                        ValkeyModuleString **argv,
                        int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, filter_name, 1, VALKEYMODULE_APIVER_1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModule_RegisterExternalFilter(ctx, filter_name);

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    if (ValkeyModule_UnregisterExternalFilter(ctx, filter_name) != VALKEYMODULE_OK) {
        ValkeyModule_Log(ctx, "error", "Failed to unregister filter");
        return VALKEYMODULE_ERR;
    }

    return VALKEYMODULE_OK;
}
