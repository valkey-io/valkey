/* Commands that don't declare their keys, used by the reply-blocking tests to
 * check that keys are tracked by what is modified or looked up rather than by
 * argv keyspecs. */

#include "valkeymodule.h"

/* RBW.CALLSET key value: SET through VM_Call. */
static int rbw_callset(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                       int argc) {
  if (argc != 3)
    return ValkeyModule_WrongArity(ctx);
  ValkeyModuleCallReply *reply =
      ValkeyModule_Call(ctx, "SET", "!ss", argv[1], argv[2]);
  if (reply == NULL)
    return ValkeyModule_ReplyWithError(ctx, "ERR call failed");
  ValkeyModule_FreeCallReply(reply);
  return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* RBW.LOCALSET key value: SET through VM_Call without "!", so the write is
 * propagated nowhere (not to the AOF, not to replicas). */
static int rbw_localset(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
  if (argc != 3)
    return ValkeyModule_WrongArity(ctx);
  ValkeyModuleCallReply *reply =
      ValkeyModule_Call(ctx, "SET", "ss", argv[1], argv[2]);
  if (reply == NULL)
    return ValkeyModule_ReplyWithError(ctx, "ERR call failed");
  ValkeyModule_FreeCallReply(reply);
  return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* RBW.KEYSET key value: SET through the low-level key API. */
static int rbw_keyset(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                      int argc) {
  if (argc != 3)
    return ValkeyModule_WrongArity(ctx);
  ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
  ValkeyModule_StringSet(key, argv[2]);
  ValkeyModule_CloseKey(key);
  ValkeyModule_ReplicateVerbatim(ctx);
  return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static void rbw_timer_callback(ValkeyModuleCtx *ctx, void *data) {
  ValkeyModuleString **args = data;
  ValkeyModuleCallReply *reply =
      ValkeyModule_Call(ctx, "SET", "!ss", args[0], args[1]);
  if (reply != NULL)
    ValkeyModule_FreeCallReply(reply);
  ValkeyModule_FreeString(ctx, args[0]);
  ValkeyModule_FreeString(ctx, args[1]);
  ValkeyModule_Free(args);
}

/* RBW.TIMERSET key value: SET through VM_Call from a timer, outside any
 * command. */
static int rbw_timerset(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
  if (argc != 3)
    return ValkeyModule_WrongArity(ctx);
  ValkeyModuleString **args = ValkeyModule_Alloc(sizeof(*args) * 2);
  args[0] = ValkeyModule_CreateStringFromString(NULL, argv[1]);
  args[1] = ValkeyModule_CreateStringFromString(NULL, argv[2]);
  ValkeyModule_CreateTimer(ctx, 1, rbw_timer_callback, args);
  return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* RBW.KEYGET key: GET through the low-level key API. */
static int rbw_keyget(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                      int argc) {
  if (argc != 2)
    return ValkeyModule_WrongArity(ctx);
  ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
  if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_STRING) {
    ValkeyModule_CloseKey(key);
    return ValkeyModule_ReplyWithNull(ctx);
  }
  size_t len;
  const char *val = ValkeyModule_StringDMA(key, &len, VALKEYMODULE_READ);
  ValkeyModule_ReplyWithStringBuffer(ctx, val, len);
  ValkeyModule_CloseKey(key);
  return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
  VALKEYMODULE_NOT_USED(argv);
  VALKEYMODULE_NOT_USED(argc);
  if (ValkeyModule_Init(ctx, "rbw", 1, VALKEYMODULE_APIVER_1) ==
      VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  /* firstkey/lastkey/step are 0: these commands declare no keys. */
  if (ValkeyModule_CreateCommand(ctx, "rbw.callset", rbw_callset, "write", 0, 0,
                                 0) == VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  if (ValkeyModule_CreateCommand(ctx, "rbw.keyset", rbw_keyset, "write", 0, 0,
                                 0) == VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  if (ValkeyModule_CreateCommand(ctx, "rbw.localset", rbw_localset, "write", 0,
                                 0, 0) == VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  if (ValkeyModule_CreateCommand(ctx, "rbw.timerset", rbw_timerset, "", 0, 0,
                                 0) == VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  if (ValkeyModule_CreateCommand(ctx, "rbw.keyget", rbw_keyget, "readonly", 0,
                                 0, 0) == VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;
  return VALKEYMODULE_OK;
}
