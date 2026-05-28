
#include "valkeymodule.h"

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx,"unsupported_features",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* This module does not set any options, meaning it will not opt-in to
     * features like Atomic Slot Migration and Async Loading */

    return VALKEYMODULE_OK;
}