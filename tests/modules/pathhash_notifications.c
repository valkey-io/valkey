/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "valkeymodule.h"
#include <string.h>

static int recreate;
static long long path_events;
static long long del_events;
static long long exists_at_notification;

static void freeKey(void *data) { ValkeyModule_FreeString(NULL, data); }

static void recreatePath(ValkeyModuleCtx *ctx, void *data) {
  ValkeyModuleCallReply *reply = ValkeyModule_Call(
      ctx, "PHSET", "!sccccc", data, "p", "FIELDS", "1", "f", "restored");
  ValkeyModule_FreeCallReply(reply);
}

static int notification(ValkeyModuleCtx *ctx, int type, const char *event,
                        ValkeyModuleString *key) {
  VALKEYMODULE_NOT_USED(type);
  if (strncmp(ValkeyModule_StringPtrLen(key, NULL), "ph-notify", 9))
    return VALKEYMODULE_OK;
  if (!strcmp(event, "phdel") || !strcmp(event, "phdelprefix")) {
    path_events++;
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "EXISTS", "s", key);
    exists_at_notification = ValkeyModule_CallReplyInteger(reply);
    ValkeyModule_FreeCallReply(reply);
  } else if (!strcmp(event, "del")) {
    del_events++;
    if (recreate) {
      /* Writes from notifications use the same post-execution jobs as other
       * types. */
      ValkeyModuleString *copy = ValkeyModule_HoldString(NULL, key);
      if (ValkeyModule_AddPostNotificationJob(ctx, recreatePath, copy,
                                              freeKey) != VALKEYMODULE_OK)
        freeKey(copy);
    }
  }
  return VALKEYMODULE_OK;
}

static int configure(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                     int argc) {
  if (argc != 2)
    return ValkeyModule_WrongArity(ctx);
  const char *mode = ValkeyModule_StringPtrLen(argv[1], NULL);
  if (strcmp(mode, "observe") && strcmp(mode, "recreate"))
    return ValkeyModule_ReplyWithError(ctx, "ERR unknown mode");
  recreate = !strcmp(mode, "recreate");
  path_events = del_events = 0;
  exists_at_notification = -1;
  return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int stats(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
  VALKEYMODULE_NOT_USED(argv);
  if (argc != 1)
    return ValkeyModule_WrongArity(ctx);
  ValkeyModule_ReplyWithArray(ctx, 3);
  ValkeyModule_ReplyWithLongLong(ctx, path_events);
  ValkeyModule_ReplyWithLongLong(ctx, del_events);
  ValkeyModule_ReplyWithLongLong(ctx, exists_at_notification);
  return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
  VALKEYMODULE_NOT_USED(argv);
  VALKEYMODULE_NOT_USED(argc);
  if (ValkeyModule_Init(ctx, "pathhash_notifications", 1,
                        VALKEYMODULE_APIVER_1) != VALKEYMODULE_OK)
    return VALKEYMODULE_ERR;
  if (ValkeyModule_SubscribeToKeyspaceEvents(
          ctx, VALKEYMODULE_NOTIFY_PATH_HASH | VALKEYMODULE_NOTIFY_GENERIC,
          notification) != VALKEYMODULE_OK)
    return VALKEYMODULE_ERR;
  if (ValkeyModule_CreateCommand(ctx, "pathhash.notify.configure", configure,
                                 "", 0, 0, 0) != VALKEYMODULE_OK)
    return VALKEYMODULE_ERR;
  return ValkeyModule_CreateCommand(ctx, "pathhash.notify.stats", stats,
                                    "readonly", 0, 0, 0);
}
