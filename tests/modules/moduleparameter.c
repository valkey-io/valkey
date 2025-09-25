#include "valkeymodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

int test_module_update_parameter(ValkeyModuleCtx *ctx,
                                 ValkeyModuleString **argv, int argc) {

  ValkeyModule_UpdateRuntimeArgs(ctx, argv, argc);
  return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "moduleparameter", 1, VALKEYMODULE_APIVER_1) ==
        VALKEYMODULE_ERR)
      return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "testmoduleparameter.update.parameter",
                                   test_module_update_parameter, "fast", 0, 0,
                                   0) == VALKEYMODULE_ERR)
      return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
