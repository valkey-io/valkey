/* Test module: a module that declares atomic slot migration support and
 * exposes ValkeyModule_ResetDataset through a command without the "write"
 * flag, so the command is not postponed by a write pause. */
#include "valkeymodule.h"

static int ResetDs_Flushall(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                            int argc) {
  VALKEYMODULE_NOT_USED(argv);
  VALKEYMODULE_NOT_USED(argc);
  ValkeyModule_ResetDataset(1, 0);
  return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
  VALKEYMODULE_NOT_USED(argv);
  VALKEYMODULE_NOT_USED(argc);
  if (ValkeyModule_Init(ctx, "resetds", 1, VALKEYMODULE_APIVER_1) ==
      VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  ValkeyModule_SetModuleOptions(
      ctx, VALKEYMODULE_OPTIONS_HANDLE_ATOMIC_SLOT_MIGRATION);
  if (ValkeyModule_CreateCommand(ctx, "resetds.flushall", ResetDs_Flushall, "",
                                 0, 0, 0) == VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  return VALKEYMODULE_OK;
}
