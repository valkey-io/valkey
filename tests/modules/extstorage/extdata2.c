#include "valkeymodule.h"

#include "sds.h"
#include <string.h>
#include <unistd.h>

/*
 * This module implements a combined external storage and filter.
 * It follows the structure defined in src/external_data.c.
 */

const char *module_name = "helloextdata2";

#define MAX_DB 16
ValkeyModuleDict *storage_mem_pool[MAX_DB];  // Memory pool for storage
ValkeyModuleDict *filter_mem_pool[MAX_DB];  // Memory pool for filter

/* Common helper functions */
static ValkeyModuleExternalStorageState
waitExternalStorageReady(ValkeyModuleExternalStorageCtx *storage_ctx) {
    ValkeyModuleExternalStorageState state = ValkeyModule_GetExternalStorageState(storage_ctx);
    if (state != VMES_STATE_READONLY) {
        return state;
    }
    
    // If in readonly state, return immediately to avoid blocking
    // The caller should handle this appropriately
    return state;
}

static ValkeyModuleExternalFilterState
waitExternalFilterReady(ValkeyModuleExternalFilterCtx *filter_ctx) {
    ValkeyModuleExternalFilterState state = ValkeyModule_GetExternalFilterState(filter_ctx);
    if (state != VMEF_STATE_READONLY) {
        return state;
    }
    
    // If in readonly state, return immediately to avoid blocking
    // The caller should handle this appropriately
    return state;
}

/* Storage methods */
static int storageSetFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalStorageCtx *storage_ctx,
                               ValkeyModuleKeyOptCtx *key_ctx,
                               ValkeyModuleString *value) {
    ValkeyModule_AutoMemory(module_ctx);
    ValkeyModuleExternalStorageState state = waitExternalStorageReady(storage_ctx);
    ValkeyModule_Assert(state == VMES_STATE_READONLY || state == VMES_STATE_READY);
    if (state == VMES_STATE_READONLY) {
        ValkeyModule_ReplyWithError(module_ctx, "ERR External storage readonly");
        return 0;
    }

    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    ValkeyModuleString *previous_value = 
        ValkeyModule_DictGet(storage_mem_pool[dbid], (ValkeyModuleString *)key, NULL);

    if (previous_value != NULL &&
        ValkeyModule_StringCompare(previous_value, value) == 0) {
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return 1;
    }

    ValkeyModule_DictReplace(storage_mem_pool[dbid], (ValkeyModuleString *)key, value);
    ValkeyModule_RetainString(NULL, value);
    
    if (previous_value != NULL) {
        ValkeyModule_FreeString(NULL, previous_value);
    }
    
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return 1;
}

static int storageGetFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalStorageCtx *storage_ctx,
                               ValkeyModuleKeyOptCtx *key_ctx, 
                               void **found) {
    ValkeyModule_AutoMemory(module_ctx);
    ValkeyModule_Assert(module_ctx != NULL && storage_ctx != NULL &&
                        key_ctx != NULL);
    VALKEYMODULE_NOT_USED(storage_ctx);
    VALKEYMODULE_NOT_USED(key_ctx);

    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);

    void *value = ValkeyModule_DictGet(storage_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    if (!value) {
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return 0;
    }

    if (found != NULL) {
        *found = value;
    }

    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return 1;
}

static int storageDelFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalStorageCtx *storage_ctx,
                               ValkeyModuleKeyOptCtx *key_ctx,
                               ValkeyModuleString **found) {
    ValkeyModule_AutoMemory(module_ctx);
    ValkeyModuleExternalStorageState state = waitExternalStorageReady(storage_ctx);
    ValkeyModule_Assert(state == VMES_STATE_READONLY || state == VMES_STATE_READY);
    if (state == VMES_STATE_READONLY) {
        ValkeyModule_ReplyWithError(module_ctx, "ERR External storage readonly");
        return EXTERNAL_DEL_ERROR;
    }

    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    ValkeyModule_Log(module_ctx, "debug", "storageDelFunction: dbid=%d, key=%s",
                     dbid, ValkeyModule_StringPtrLen(key, NULL));
    
    ValkeyModuleString *value =
        ValkeyModule_DictGet(storage_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    
    if (!value) {
        ValkeyModule_Log(module_ctx, "debug", "storageDelFunction: key not found in storage");
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return EXTERNAL_DEL_NOT_FOUND;
    }

    ValkeyModule_Log(module_ctx, "debug", "storageDelFunction: found value, attempting delete");
    if (ValkeyModule_DictDel(storage_mem_pool[dbid], (ValkeyModuleString *)key, NULL) != VALKEYMODULE_OK) {
        ValkeyModule_Log(module_ctx, "debug", "storageDelFunction: delete failed");
        ValkeyModule_ReplyWithErrorFormat(module_ctx, "ERR Failed to del key %s",
                                          ValkeyModule_StringPtrLen(key, NULL));
        return EXTERNAL_DEL_ERROR;
    }

    ValkeyModule_Log(module_ctx, "debug", "storageDelFunction: delete successful");
    if (found != NULL) {
        *found = value;
    } else {
        ValkeyModule_FreeString(NULL, value);
    }

    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return EXTERNAL_DEL_SUCCESS;
}

static void storageSetReadonlyFunction(ValkeyModuleCtx *module_ctx,
                                        ValkeyModuleExternalStorageCtx *storage_ctx) {
    ValkeyModule_SetExternalStorageState(storage_ctx, VMES_STATE_READONLY);
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
}

static void storageDropReadonlyFunction(ValkeyModuleCtx *module_ctx,
                                         ValkeyModuleExternalStorageCtx *storage_ctx) {
    ValkeyModule_SetExternalStorageState(storage_ctx, VMES_STATE_READY);
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
}

static int storageIterateFunction(ValkeyModuleCtx *, int dbid,
                                   ValkeyModuleString *, long long *,
                                   ValkeyModuleString **next,
                                   ValkeyModuleDictIter **iter) {
    if (!*iter) {
        *iter = ValkeyModule_DictIteratorStartC(storage_mem_pool[dbid], "^", NULL, 0);
    }

    void *key = ValkeyModule_DictNext(NULL, *iter, NULL);
    if (!key) {
        ValkeyModule_DictIteratorStop(*iter);
        *iter = NULL;
    }

    *next = key;
    return (key != NULL);
}

/* Filter methods */
static int filterSetFunction(ValkeyModuleCtx *module_ctx,
                              ValkeyModuleExternalFilterCtx *filter_ctx,
                              ValkeyModuleKeyOptCtx *key_ctx) {
    ValkeyModule_AutoMemory(module_ctx);
    ValkeyModuleExternalFilterState state = waitExternalFilterReady(filter_ctx);
    ValkeyModule_Assert(state == VMEF_STATE_READONLY || state == VMEF_STATE_READY);
    if (state == VMEF_STATE_READONLY) {
        ValkeyModule_ReplyWithError(module_ctx, "ERR External filter readonly");
        return 0;
    }

    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    ValkeyModuleString *previous_value = 
        ValkeyModule_DictGet(filter_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    
    if (previous_value != NULL) {
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return 1;
    }

    if (ValkeyModule_DictReplace(filter_mem_pool[dbid], (ValkeyModuleString *)key, "") == VALKEYMODULE_ERR) {
        ValkeyModule_ReplyWithErrorFormat(module_ctx, "ERR Failed to set key %s",
                                          ValkeyModule_StringPtrLen(key, NULL));
        return 0;
    }

    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return 1;
}

static int filterGetFunction(ValkeyModuleCtx *module_ctx,
                              ValkeyModuleExternalFilterCtx *,
                              ValkeyModuleKeyOptCtx *key_ctx) {
    ValkeyModule_AutoMemory(module_ctx);
    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);

    size_t length;
    ValkeyModule_StringPtrLen(key, &length);
    if (length == 0) {
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return 0;
    }

    void *value = ValkeyModule_DictGet(filter_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return (value != NULL);
}

static int filterDelFunction(ValkeyModuleCtx *module_ctx,
                              ValkeyModuleExternalFilterCtx *filter_ctx,
                              ValkeyModuleKeyOptCtx *key_ctx,
                              ValkeyModuleString **found) {
    ValkeyModule_AutoMemory(module_ctx);
    ValkeyModuleExternalFilterState state = waitExternalFilterReady(filter_ctx);
    ValkeyModule_Assert(state == VMEF_STATE_READONLY || state == VMEF_STATE_READY);
    if (state == VMEF_STATE_READONLY) {
        ValkeyModule_ReplyWithError(module_ctx, "ERR External filter readonly");
        return EXTERNAL_DEL_ERROR;
    }

    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    ValkeyModule_Log(module_ctx, "debug", "filterDelFunction: dbid=%d, key=%s",
                     dbid, ValkeyModule_StringPtrLen(key, NULL));
    
    ValkeyModuleString *value =
        ValkeyModule_DictGet(filter_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    
    if (!value) {
        ValkeyModule_Log(module_ctx, "debug", "filterDelFunction: key not found in filter");
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return EXTERNAL_DEL_NOT_FOUND;
    }

    ValkeyModule_Log(module_ctx, "debug", "filterDelFunction: found value, attempting delete");
    if (ValkeyModule_DictDel(filter_mem_pool[dbid], (ValkeyModuleString *)key, NULL) != VALKEYMODULE_OK) {
        ValkeyModule_Log(module_ctx, "debug", "filterDelFunction: delete failed");
        ValkeyModule_ReplyWithErrorFormat(module_ctx, "ERR Failed to del key %s",
                                          ValkeyModule_StringPtrLen(key, NULL));
        return EXTERNAL_DEL_ERROR;
    }
    
    ValkeyModule_Log(module_ctx, "debug", "filterDelFunction: delete successful");
    if (found != NULL) {
        *found = ValkeyModule_CreateStringFromLongLong(NULL, 1);
    }
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return EXTERNAL_DEL_SUCCESS;
}

static void filterSetReadonlyFunction(ValkeyModuleCtx *module_ctx,
                                       ValkeyModuleExternalFilterCtx *filter_ctx) {
    ValkeyModule_SetExternalFilterState(filter_ctx, VMEF_STATE_READONLY);
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
}

static void filterDropReadonlyFunction(ValkeyModuleCtx *module_ctx,
                                        ValkeyModuleExternalFilterCtx *filter_ctx) {
    ValkeyModule_SetExternalFilterState(filter_ctx, VMEF_STATE_READY);
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
}

/* Module initialization */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, module_name, 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Initialize storage methods */
    ValkeyModuleExternalStorageMethods storage_methods = {
        .version = VALKEYMODULE_EXTERNAL_STORAGE_ABI_VERSION,
        .set = storageSetFunction,
        .get = storageGetFunction,
        .del = storageDelFunction,
        .set_readonly = storageSetReadonlyFunction,
        .drop_readonly = storageDropReadonlyFunction,
        .iterate = storageIterateFunction
    };

    /* Initialize filter methods */
    ValkeyModuleExternalFilterMethods filter_methods = {
        .version = VALKEYMODULE_EXTERNAL_FILTER_ABI_VERSION,
        .set = filterSetFunction,
        .get = filterGetFunction,
        .del = filterDelFunction,
        .set_readonly = filterSetReadonlyFunction,
        .drop_readonly = filterDropReadonlyFunction
    };

    /* Create memory pools */
    for (int i = 0; i < MAX_DB; i++) {
        storage_mem_pool[i] = ValkeyModule_CreateDict(NULL);
        filter_mem_pool[i] = ValkeyModule_CreateDict(NULL);
    }

    /* Register both storage and filter */
    if (ValkeyModule_RegisterExternalDataModule(ctx, module_name, &storage_methods, &filter_methods) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    /* Unregister both storage and filter */
    if (ValkeyModule_UnregisterExternalDataModule(ctx, module_name) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Free memory pools */
    for (int i = 0; i < MAX_DB; i++) {
        ValkeyModule_FreeDict(NULL, storage_mem_pool[i]);
        ValkeyModule_FreeDict(NULL, filter_mem_pool[i]);
    }

    return VALKEYMODULE_OK;
}