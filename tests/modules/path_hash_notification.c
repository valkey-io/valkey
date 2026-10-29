/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "valkeymodule.h"
#include <string.h>

static int mutation_mode;

static int setMode(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
  if (argc != 2)
    return ValkeyModule_WrongArity(ctx);
  const char *mode = ValkeyModule_StringPtrLen(argv[1], NULL);
  if (!strcmp(mode, "none"))
    mutation_mode = 0;
  else if (!strcmp(mode, "del"))
    mutation_mode = 1;
  else if (!strcmp(mode, "replace"))
    mutation_mode = 2;
  else
    return ValkeyModule_ReplyWithError(ctx, "ERR invalid mode");
  return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int onPathHashEvent(ValkeyModuleCtx *ctx, int type, const char *event,
                           ValkeyModuleString *key) {
  VALKEYMODULE_NOT_USED(type);
  size_t len;
  const char *name = ValkeyModule_StringPtrLen(key, &len);
  if (!mutation_mode || len < 4 || memcmp(name, "uaf:", 4) ||
      (strcmp(event, "phdel") && strcmp(event, "phdelprefix")))
    return VALKEYMODULE_OK;

  /* Deliberately exercise a synchronous write in this regression module. */
  ValkeyModuleCallReply *reply =
      mutation_mode == 1
          ? ValkeyModule_Call(ctx, "DEL", "s", key)
          : ValkeyModule_Call(ctx, "SET", "sc", key, "replacement");
  if (reply)
    ValkeyModule_FreeCallReply(reply);
  return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
  VALKEYMODULE_NOT_USED(argv);
  VALKEYMODULE_NOT_USED(argc);
  if (ValkeyModule_Init(ctx, "path_hash_notification", 1,
                        VALKEYMODULE_APIVER_1) != VALKEYMODULE_OK)
    return VALKEYMODULE_ERR;
  if (ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_PATH_HASH,
                                             onPathHashEvent) !=
      VALKEYMODULE_OK)
    return VALKEYMODULE_ERR;
  return ValkeyModule_CreateCommand(ctx, "pathhashnotification.mode", setMode,
                                    "write", 0, 0, 0);
}
