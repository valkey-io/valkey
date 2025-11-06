#include "valkeymodule.h"
#include <strings.h>

/* Second module configs module, for testing.
 * Need to make sure that multiple modules with configs don't interfere with each other */
int bool_config;

int getBoolConfigCommand(const char *name, void *privdata) {
    VALKEYMODULE_NOT_USED(privdata);
    if (!strcasecmp(name, "test")) {
        return bool_config;
    }
    return 0;
}

int setBoolConfigCommand(const char *name, int new, void *privdata, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(privdata);
    VALKEYMODULE_NOT_USED(err);
    if (!strcasecmp(name, "test")) {
        bool_config = new;
        return VALKEYMODULE_OK;
    }
    return VALKEYMODULE_ERR;
}

/* No arguments are expected */ 
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "configs", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_RegisterBoolConfig(ctx, "test", 1, VALKEYMODULE_CONFIG_DEFAULT, getBoolConfigCommand, setBoolConfigCommand, NULL, &argc) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    if (ValkeyModule_LoadConfigs(ctx) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}