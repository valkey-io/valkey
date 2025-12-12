#include "valkeymodule.h"

#include "sds.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * This module implements a combined external storage and filter.
 * It follows the structure defined in src/external_data.c.
 */

const char *module_name = "helloextdata1";

#define MAX_DB 16
ValkeyModuleDict *storage_mem_pool[MAX_DB];  // Memory pool for storage
ValkeyModuleDict *filter_mem_pool[MAX_DB];  // Memory pool for filter

/* Failure simulation configuration */
static long long set_failure_percent = 0;  // 0 = no failures, >0 = p out of 100 sets fail
static int set_operation_counter = 0;  // Counter for set operations

/* Configuration callbacks for set_failure_percent */
static long long getFailurePercentConfig(const char *name, void *privdata) {
    VALKEYMODULE_NOT_USED(name);
    VALKEYMODULE_NOT_USED(privdata);
    return set_failure_percent;
}

static int setFailurePercentConfig(const char *name, long long new_val, void *privdata, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(name);
    VALKEYMODULE_NOT_USED(privdata);
    if (new_val < 0 || new_val > 100) {
        *err = ValkeyModule_CreateString(NULL, "set_failure_percent must be between 0 and 100", 47);
        return VALKEYMODULE_ERR;
    }
    set_failure_percent = new_val;
    /* Reset counter when changing failure rate */
    set_operation_counter = 0;
    return VALKEYMODULE_OK;
}

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

    /* Simulate failures if configured */
    if (set_failure_percent > 0) {
        set_operation_counter++;
        /* Fail every (100/set_failure_percent)th operation starting from the 1st */
        int fail_interval = 100 / set_failure_percent;
        if (fail_interval > 0 && (set_operation_counter % fail_interval) == 1) {
            ValkeyModule_ReplyWithError(module_ctx, "ERR Simulated storage failure");
            return 0;
        }
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
    // Retain the value to keep it alive in our storage
    ValkeyModule_RetainString(NULL, value);
    
    if (previous_value != NULL) {
        // Free the previous value if it exists
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
        return EXTERNAL_ERROR;
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
        return EXTERNAL_NOT_FOUND;
    }

    ValkeyModule_Log(module_ctx, "debug", "storageDelFunction: found value, attempting delete");
    if (ValkeyModule_DictDel(storage_mem_pool[dbid], (ValkeyModuleString *)key, NULL) != VALKEYMODULE_OK) {
        ValkeyModule_Log(module_ctx, "debug", "storageDelFunction: delete failed");
        ValkeyModule_ReplyWithErrorFormat(module_ctx, "ERR Failed to del key %s",
                                          ValkeyModule_StringPtrLen(key, NULL));
        return EXTERNAL_ERROR;
    }

    ValkeyModule_Log(module_ctx, "debug", "storageDelFunction: delete successful");
    if (found != NULL) {
        *found = value;
    } else {
        ValkeyModule_FreeString(NULL, value);
    }

    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return EXTERNAL_SUCCESS;
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
        return EXTERNAL_ERROR;
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
        return EXTERNAL_NOT_FOUND;
    }

    ValkeyModule_Log(module_ctx, "debug", "filterDelFunction: found value, attempting delete");
    if (ValkeyModule_DictDel(filter_mem_pool[dbid], (ValkeyModuleString *)key, NULL) != VALKEYMODULE_OK) {
        ValkeyModule_Log(module_ctx, "debug", "filterDelFunction: delete failed");
        ValkeyModule_ReplyWithErrorFormat(module_ctx, "ERR Failed to del key %s",
                                          ValkeyModule_StringPtrLen(key, NULL));
        return EXTERNAL_ERROR;
    }
    
    ValkeyModule_Log(module_ctx, "debug", "filterDelFunction: delete successful");
    if (found != NULL) {
        *found = ValkeyModule_CreateStringFromLongLong(NULL, 1);
    }
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return EXTERNAL_SUCCESS;
}

static int storageSetReadonlyFunction(ValkeyModuleCtx *, ValkeyModuleExternalStorageCtx *) {
    return EXTERNAL_SUCCESS;
}

static int storageDropReadonlyFunction(ValkeyModuleCtx *, ValkeyModuleExternalStorageCtx *) {
    return EXTERNAL_SUCCESS;
}

static int storageFlushFunction(ValkeyModuleCtx *module_ctx,
                                ValkeyModuleExternalStorageCtx *storage_ctx,
                                int dbid) {
    (void)storage_ctx; /* Unused */
    (void)module_ctx; /* Log only, no automatic memory management needed */
    
    if (dbid < 0 || dbid >= MAX_DB) {
        return EXTERNAL_ERROR;
    }
    
    if (storage_mem_pool[dbid] != NULL) {
        /* First, iterate through all keys and free the retained strings
         * This prevents memory leaks from ValkeyModule_RetainString calls */
        ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(storage_mem_pool[dbid], "^", NULL, 0);
        ValkeyModuleString *key;
        while ((key = ValkeyModule_DictNext(NULL, iter, NULL)) != NULL) {
            /* Get the value before freeing the key */
            ValkeyModuleString *value = ValkeyModule_DictGet(storage_mem_pool[dbid], key, NULL);
            
            /* Free the key string created by DictNext */
            ValkeyModule_FreeString(NULL, key);
            
            if (value != NULL) {
                ValkeyModule_FreeString(NULL, value);
            }
        }
        ValkeyModule_DictIteratorStop(iter);
        
        /* Now free the entire dictionary */
        ValkeyModule_FreeDict(NULL, storage_mem_pool[dbid]);
    }
    storage_mem_pool[dbid] = ValkeyModule_CreateDict(NULL);
    
    return EXTERNAL_SUCCESS;
}

static int filterSetReadonlyFunction(ValkeyModuleCtx *, ValkeyModuleExternalFilterCtx *) {
    return EXTERNAL_SUCCESS;
}

static int filterDropReadonlyFunction(ValkeyModuleCtx *, ValkeyModuleExternalFilterCtx *) {
    return EXTERNAL_SUCCESS;
}

static int filterFlushFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalFilterCtx *filter_ctx,
                               int dbid) {
    (void)filter_ctx; /* Unused */
    (void)module_ctx; /* Log only, no automatic memory management needed */
    
    if (dbid < 0 || dbid >= MAX_DB) {
        return EXTERNAL_ERROR;
    }
    
    if (filter_mem_pool[dbid] != NULL) {
        /* For filter, we don't retain strings with ValkeyModule_RetainString like storage does.
         * We just use ValkeyModule_DictReplace directly, so we can free the dictionary directly
         * without iterating through individual strings to avoid double-free issues. */
        ValkeyModule_FreeDict(NULL, filter_mem_pool[dbid]);
    }
    filter_mem_pool[dbid] = ValkeyModule_CreateDict(NULL);
    
    return EXTERNAL_SUCCESS;
}

static int storageSwapFunction(ValkeyModuleCtx *module_ctx,
                              ValkeyModuleExternalStorageCtx *storage_ctx,
                              int dbid1,
                              int dbid2) {
    (void)storage_ctx; /* Unused */
    (void)module_ctx; /* Log only, no automatic memory management needed */
    
    if (dbid1 < 0 || dbid1 >= MAX_DB || dbid2 < 0 || dbid2 >= MAX_DB) {
        return EXTERNAL_ERROR;
    }
    
    /* Swap the storage memory pools between the two databases */
    ValkeyModuleDict *temp = storage_mem_pool[dbid1];
    storage_mem_pool[dbid1] = storage_mem_pool[dbid2];
    storage_mem_pool[dbid2] = temp;
    
    return EXTERNAL_SUCCESS;
}

static int filterSwapFunction(ValkeyModuleCtx *module_ctx,
                             ValkeyModuleExternalFilterCtx *filter_ctx,
                             int dbid1,
                             int dbid2) {
    (void)filter_ctx; /* Unused */
    (void)module_ctx; /* Log only, no automatic memory management needed */
    
    if (dbid1 < 0 || dbid1 >= MAX_DB || dbid2 < 0 || dbid2 >= MAX_DB) {
        return EXTERNAL_ERROR;
    }
    
    /* Swap the filter memory pools between the two databases */
    ValkeyModuleDict *temp = filter_mem_pool[dbid1];
    filter_mem_pool[dbid1] = filter_mem_pool[dbid2];
    filter_mem_pool[dbid2] = temp;
    
    return EXTERNAL_SUCCESS;
}

static int storageDumpFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalStorageCtx *storage_ctx,
                               int dbid,
                               int slot,
                               long long timestamp,
                               ValkeyModuleString *target,
                               ValkeyModuleString **backup_id) {
    VALKEYMODULE_NOT_USED(storage_ctx);
    VALKEYMODULE_NOT_USED(slot);
    VALKEYMODULE_NOT_USED(timestamp);
    VALKEYMODULE_NOT_USED(target);
    ValkeyModule_AutoMemory(module_ctx);
    
    if (dbid < 0 || dbid >= MAX_DB) {
        return EXTERNAL_ERROR;
    }
    
    /* For this simple implementation, we just return a dummy backup_id */
    if (backup_id != NULL) {
        *backup_id = ValkeyModule_CreateStringPrintf(module_ctx, "backup_%d_%lld", dbid, (long long)time(NULL));
    }
    
    return EXTERNAL_SUCCESS;
}

static int storageLoadFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalStorageCtx *storage_ctx,
                               int dbid,
                               ValkeyModuleString *backup_id) {
    VALKEYMODULE_NOT_USED(storage_ctx);
    VALKEYMODULE_NOT_USED(backup_id);
    ValkeyModule_AutoMemory(module_ctx);
    
    if (dbid < 0 || dbid >= MAX_DB) {
        return EXTERNAL_ERROR;
    }
    
    /* For this simple implementation, we just return success */
    return EXTERNAL_SUCCESS;
}

static int filterKeysCountFunction(ValkeyModuleCtx *module_ctx,
                                   ValkeyModuleExternalFilterCtx *filter_ctx,
                                   int dbid,
                                   unsigned long long *count) {
    VALKEYMODULE_NOT_USED(filter_ctx);
    ValkeyModule_AutoMemory(module_ctx);
    
    if (dbid < 0 || dbid >= MAX_DB) {
        return EXTERNAL_ERROR;
    }
    
    if (filter_mem_pool[dbid] == NULL) {
        *count = 0;
        return EXTERNAL_SUCCESS;
    }
    
    *count = ValkeyModule_DictSize(filter_mem_pool[dbid]);
    return EXTERNAL_SUCCESS;
}

/* Module initialization */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_Init(ctx, module_name, 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Register runtime configuration for set_failure_percent */
    if (ValkeyModule_RegisterNumericConfig(ctx, "set_failure_percent", 0,
                                           VALKEYMODULE_CONFIG_DEFAULT,
                                           0, 100,
                                           getFailurePercentConfig,
                                           setFailurePercentConfig,
                                           NULL, NULL) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    /* Parse module arguments for initial set_failure_percent value */
    if (argc > 0) {
        for (int i = 0; i < argc; i++) {
            const char *arg = ValkeyModule_StringPtrLen(argv[i], NULL);
            if (strncmp(arg, "set_failure_percent=", 20) == 0) {
                int initial_value = atoi(arg + 20);
                if (initial_value < 0 || initial_value > 100) {
                    ValkeyModule_Log(ctx, "warning",
                        "Invalid set_failure_percent value: %d, must be 0-100. Using 0.",
                        initial_value);
                    initial_value = 0;
                }
                set_failure_percent = initial_value;
                ValkeyModule_Log(ctx, "notice",
                    "Module %s loaded with set_failure_percent=%d",
                    module_name, (int)set_failure_percent);
            }
        }
    }

    /* Initialize storage methods */
    ValkeyModuleExternalStorageMethods storage_methods = {
        .version = VALKEYMODULE_EXTERNAL_STORAGE_ABI_VERSION,
        .set = storageSetFunction,
        .get = storageGetFunction,
        .del = storageDelFunction,
        .set_readonly = storageSetReadonlyFunction,
        .drop_readonly = storageDropReadonlyFunction,
        .iterate = storageIterateFunction,
        .flush = storageFlushFunction,
        .swap = storageSwapFunction,
        .dump = storageDumpFunction,
        .load = storageLoadFunction
    };

    /* Initialize filter methods */
    ValkeyModuleExternalFilterMethods filter_methods = {
        .version = VALKEYMODULE_EXTERNAL_FILTER_ABI_VERSION,
        .set = filterSetFunction,
        .get = filterGetFunction,
        .del = filterDelFunction,
        .set_readonly = filterSetReadonlyFunction,
        .drop_readonly = filterDropReadonlyFunction,
        .flush = filterFlushFunction,
        .swap = filterSwapFunction,
        .keys_count = filterKeysCountFunction
    };

    /* Create memory pools */
    for (int i = 0; i < MAX_DB; i++) {
        storage_mem_pool[i] = ValkeyModule_CreateDict(NULL);
        filter_mem_pool[i] = ValkeyModule_CreateDict(NULL);
    }

    /* Load configurations */
    if (ValkeyModule_LoadConfigs(ctx) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
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
