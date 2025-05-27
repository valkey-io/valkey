#include "valkeymodule.h"

#include "sds.h"
#include <string.h>
#include <unistd.h>

/*
 * This module implements a very simple external filter.
 * It's purpose is only to test the valkey module API to implement external
 * filters.
 */

const char *filter_name = "hellofilter2";

#define MAX_DB 16
ValkeyModuleDict *mem_pool[MAX_DB];

static ValkeyModuleExternalFilterState
waitExternalFilterReady(ValkeyModuleExternalFilterCtx *filter_ctx) {
  uint32_t elapsed_milliseconds = 0;
  unsigned int seconds = ValkeyModule_GetExternalFilterTimeout(filter_ctx);
  ValkeyModuleExternalFilterState state = VMEF_STATE_READY;
  while (1) {
    state = ValkeyModule_GetExternalFilterState(filter_ctx);
    if (state != VMEF_STATE_READONLY) {
      break;
    }

    if (elapsed_milliseconds >= (seconds * 1000)) {
      break;
    }

    usleep(1000);
    elapsed_milliseconds++;
  }

  return state;
}

static int setFunction(ValkeyModuleCtx *module_ctx,
                       ValkeyModuleExternalFilterCtx *filter_ctx,
                       ValkeyModuleKeyOptCtx *key_ctx,
                       ValkeyModuleString *value) {
  ValkeyModule_AutoMemory(module_ctx);
  VALKEYMODULE_NOT_USED(filter_ctx);
  VALKEYMODULE_NOT_USED(key_ctx);

  ValkeyModuleExternalFilterState state = waitExternalFilterReady(filter_ctx);
  ValkeyModule_Assert(state == VMEF_STATE_READONLY ||
                      state == VMEF_STATE_READY);
  if (state == VMEF_STATE_READONLY) {
    ValkeyModule_ReplyWithError(module_ctx, "ERR External filter readonly");
    return 0;
  }

  int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
  const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
  ValkeyModuleString *previous_value =
      ValkeyModule_DictGet(mem_pool[dbid], (ValkeyModuleString *)key, NULL);
  if (previous_value != NULL &&
      ValkeyModule_StringCompare(previous_value, value) == 0) {
    // nothing to do, already set
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return 1;
  }

  ValkeyModule_DictReplace(mem_pool[dbid], (ValkeyModuleString *)key, value);

  const ValkeyModuleString *resulting_value =
      ValkeyModule_DictGet(mem_pool[dbid], (ValkeyModuleString *)key, NULL);
  if (ValkeyModule_StringCompare(resulting_value, value) != 0) {
    ValkeyModule_ReplyWithErrorFormat(module_ctx,
                                      "ERR Failed to set key %s and value %s",
                                      ValkeyModule_StringPtrLen(key, NULL),
                                      ValkeyModule_StringPtrLen(value, NULL));
    return 0;
  }

  ValkeyModule_RetainString(module_ctx, value);
  if (previous_value != NULL) {
    ValkeyModule_FreeString(module_ctx, previous_value);
  }
  ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
  return 1;
}

static int getFunction(ValkeyModuleCtx *module_ctx,
                       ValkeyModuleExternalFilterCtx *filter_ctx,
                       ValkeyModuleKeyOptCtx *key_ctx, void **found) {
  ValkeyModule_AutoMemory(module_ctx);
  ValkeyModule_Assert(module_ctx != NULL && filter_ctx != NULL &&
                      key_ctx != NULL);
  VALKEYMODULE_NOT_USED(filter_ctx);
  VALKEYMODULE_NOT_USED(key_ctx);

  int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
  const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);

  size_t length;
  ValkeyModule_StringPtrLen(key, &length);

  if (length == 0) {
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return 0;
  }

  void *value =
      ValkeyModule_DictGet(mem_pool[dbid], (ValkeyModuleString *)key, NULL);
  if (!value) {
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return 0;
  }
  *found = value;

  ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
  return 1;
}

static int delFunction(ValkeyModuleCtx *module_ctx,
                       ValkeyModuleExternalFilterCtx *filter_ctx,
                       ValkeyModuleKeyOptCtx *key_ctx,
                       ValkeyModuleString **found) {
  ValkeyModule_AutoMemory(module_ctx);
  VALKEYMODULE_NOT_USED(filter_ctx);
  VALKEYMODULE_NOT_USED(key_ctx);

  ValkeyModuleExternalFilterState state = waitExternalFilterReady(filter_ctx);
  ValkeyModule_Assert(state == VMEF_STATE_READONLY ||
                      state == VMEF_STATE_READY);
  if (state == VMEF_STATE_READONLY) {
    ValkeyModule_ReplyWithError(module_ctx,
                                "ERR External filter blocked readonly");
    return 0;
  }

  int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
  const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);

  ValkeyModuleString *value =
      ValkeyModule_DictGet(mem_pool[dbid], (ValkeyModuleString *)key, NULL);
  if (!value) {
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return 1;
  }

  if (ValkeyModule_DictDel(mem_pool[dbid], (ValkeyModuleString *)key, NULL) !=
      VALKEYMODULE_OK) {
    ValkeyModule_ReplyWithErrorFormat(module_ctx, "ERR Failed to del key %s",
                                      ValkeyModule_StringPtrLen(key, NULL));
    return 0;
  }
  ValkeyModule_FreeString(module_ctx, value);
  *found = ValkeyModule_CreateStringFromLongLong(NULL, 1);

  ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
  return 1;
}

static void setReadonlyFunction(ValkeyModuleCtx *module_ctx,
                                ValkeyModuleExternalFilterCtx *filter_ctx) {
  VALKEYMODULE_NOT_USED(filter_ctx);

  ValkeyModule_SetExternalFilterState(filter_ctx, VMEF_STATE_READONLY);

  ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
  return;
}

static void dropReadonlyFunction(ValkeyModuleCtx *module_ctx,
                                 ValkeyModuleExternalFilterCtx *filter_ctx) {
  VALKEYMODULE_NOT_USED(filter_ctx);

  ValkeyModule_SetExternalFilterState(filter_ctx, VMEF_STATE_READY);

  ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
  return;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
  VALKEYMODULE_NOT_USED(argv);
  VALKEYMODULE_NOT_USED(argc);

  if (ValkeyModule_Init(ctx, filter_name, 1, VALKEYMODULE_APIVER_1) ==
      VALKEYMODULE_ERR)
    return VALKEYMODULE_ERR;

  ValkeyModuleExternalFilterMethods methods = {
      .version = VALKEYMODULE_EXTERNAL_STORAGE_ABI_VERSION,
      .set = setFunction,
      .get = getFunction,
      .del = delFunction,
      .set_readonly = setReadonlyFunction,
      .drop_readonly = dropReadonlyFunction,
  };

  for (int i = 0; i < MAX_DB; i++) {
    mem_pool[i] = ValkeyModule_CreateDict(NULL);
  }

  return ValkeyModule_RegisterExternalFilter(ctx, filter_name, &methods);
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
  int res = ValkeyModule_UnregisterExternalFilter(ctx, filter_name);
  if (res == VALKEYMODULE_ERR) {
    return res;
  }

  for (int i = 0; i < MAX_DB; i++) {
    ValkeyModule_FreeDict(NULL, mem_pool[i]);
  }
  return res;
}
