#include "valkeymodule.h"

#include "sds.h"
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>

/*
 * This module implements a combined external storage and filter.
 * It follows the structure defined in src/external_data.c.
 */

const char *module_name = "helloextdata1";
static char server_id[256] = "localhost:6379"; /* Default server ID, will be updated from target */

#define MAX_DB 16
ValkeyModuleDict *storage_mem_pool[MAX_DB];  // Memory pool for storage
ValkeyModuleDict *filter_mem_pool[MAX_DB];  // Memory pool for filter

/* Failure simulation configuration */
static long long set_failure_percent = 0;  // 0 = no failures, >0 = p out of 100 sets fail
static int set_operation_counter = 0;  // Counter for set operations

/* Loading state and command queue */
static int is_loading = 0;  // Flag to indicate if we're currently loading from backup
typedef struct QueuedCommand {
    int dbid;
    ValkeyModuleString *key;
    ValkeyModuleString *value;
    struct QueuedCommand *next;
} QueuedCommand;
static QueuedCommand *command_queue_head = NULL;
static QueuedCommand *command_queue_tail = NULL;

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
    
    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    
    /* If we're currently loading, queue this command for later processing */
    if (is_loading) {
        QueuedCommand *cmd = ValkeyModule_Alloc(sizeof(QueuedCommand));
        cmd->dbid = dbid;
        cmd->key = ValkeyModule_CreateStringFromString(NULL, key);
        cmd->value = ValkeyModule_CreateStringFromString(NULL, value);
        cmd->next = NULL;
        
        if (command_queue_tail == NULL) {
            command_queue_head = command_queue_tail = cmd;
        } else {
            command_queue_tail->next = cmd;
            command_queue_tail = cmd;
        }
        
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return 1;
    }
    
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
    
    size_t dict_size = storage_mem_pool[dbid] ? ValkeyModule_DictSize(storage_mem_pool[dbid]) : 0;
    ValkeyModule_Log(module_ctx, "debug", "storageGetFunction: dbid=%d, key=%s, dict_size=%zu",
                     dbid, ValkeyModule_StringPtrLen(key, NULL), dict_size);

    void *value = ValkeyModule_DictGet(storage_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    if (!value) {
        ValkeyModule_Log(module_ctx, "debug", "storageGetFunction: key not found in storage");
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return 0;
    }

    ValkeyModule_Log(module_ctx, "debug", "storageGetFunction: key found, returning value");
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
    
    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    
    /* If we're currently loading, commands are already queued in storage, just return success */
    if (is_loading) {
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return 1;
    }
    
    ValkeyModuleExternalFilterState state = waitExternalFilterReady(filter_ctx);
    ValkeyModule_Assert(state == VMEF_STATE_READONLY || state == VMEF_STATE_READY);
    if (state == VMEF_STATE_READONLY) {
        ValkeyModule_ReplyWithError(module_ctx, "ERR External filter readonly");
        return 0;
    }

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
    
    size_t dict_size = filter_mem_pool[dbid] ? ValkeyModule_DictSize(filter_mem_pool[dbid]) : 0;
    ValkeyModule_Log(module_ctx, "debug", "filterGetFunction: dbid=%d, key=%s, dict_size=%zu",
                     dbid, ValkeyModule_StringPtrLen(key, NULL), dict_size);

    size_t length;
    ValkeyModule_StringPtrLen(key, &length);
    if (length == 0) {
        ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
        return 0;
    }

    void *value = ValkeyModule_DictGet(filter_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    int found = (value != NULL);
    ValkeyModule_Log(module_ctx, "debug", "filterGetFunction: key %s in filter", found ? "found" : "not found");
    ValkeyModule_ReplyWithSimpleString(module_ctx, "OK");
    return found;
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
    
    ValkeyModule_Log(module_ctx, "notice", "storageFlushFunction called for dbid=%d", dbid);
    
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
    
    ValkeyModule_Log(module_ctx, "notice", "filterFlushFunction called for dbid=%d", dbid);
    
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
    ValkeyModule_AutoMemory(module_ctx);
    
    /* Extract server ID from target parameter if provided, otherwise use local server ID */
    if (target) {
        size_t target_len;
        const char *target_str = ValkeyModule_StringPtrLen(target, &target_len);
        snprintf(server_id, sizeof(server_id), "%.*s", (int)target_len, target_str);
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: using target server_id=%s", server_id);
        ValkeyModule_Log(module_ctx, "warning", "DEBUG: storageDumpFunction - target provided, server_id set to '%s'", server_id);
    } else {
        /* Use local server ID */
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: using local server_id=%s", server_id);
        ValkeyModule_Log(module_ctx, "warning", "DEBUG: storageDumpFunction - no target, using local server_id='%s'", server_id);
    }
    
    /* Create a unique backup ID based on timestamp */
    char backup_id_str[256];
    snprintf(backup_id_str, sizeof(backup_id_str), "backup_%d_%lld", dbid, (long long)time(NULL));
    
    /* Create directory for this server instance */
    char server_dir[1024];
    snprintf(server_dir, sizeof(server_dir), "/tmp/external_data/%s", server_id);
    mkdir("/tmp/external_data", 0755);
    mkdir(server_dir, 0755);
    ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: creating backup in directory=%s", server_dir);
    
    /* Dump storage data to disk in server-specific directory */
    char storage_filename[1536];
    snprintf(storage_filename, sizeof(storage_filename), "%s/%s_storage_%s.dat", server_dir, module_name, backup_id_str);
    
    FILE *storage_file = fopen(storage_filename, "w");
    if (!storage_file) {
        ValkeyModule_Log(module_ctx, "warning", "Failed to create storage dump file: %s", storage_filename);
        return EXTERNAL_ERROR;
    }
    
    /* Iterate through all databases and dump their storage data */
    for (int db = 0; db < MAX_DB; db++) {
        if (storage_mem_pool[db] == NULL) continue;
        
        ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(storage_mem_pool[db], "^", NULL, 0);
        ValkeyModuleString *key;
        while ((key = ValkeyModule_DictNext(NULL, iter, NULL)) != NULL) {
            ValkeyModuleString *value = ValkeyModule_DictGet(storage_mem_pool[db], key, NULL);
            if (value != NULL) {
                size_t key_len, val_len;
                const char *key_str = ValkeyModule_StringPtrLen(key, &key_len);
                const char *val_str = ValkeyModule_StringPtrLen(value, &val_len);
                
                /* Format: db_id key_len key_data val_len val_data */
                fprintf(storage_file, "%d %zu %.*s %zu %.*s\n",
                        db, key_len, (int)key_len, key_str, val_len, (int)val_len, val_str);
            }
            ValkeyModule_FreeString(NULL, key);
        }
        ValkeyModule_DictIteratorStop(iter);
    }
    fclose(storage_file);
    
    /* Dump filter data to disk in server-specific directory */
    char filter_filename[1536];
    snprintf(filter_filename, sizeof(filter_filename), "%s/%s_filter_%s.dat", server_dir, module_name, backup_id_str);
    
    FILE *filter_file = fopen(filter_filename, "w");
    if (!filter_file) {
        ValkeyModule_Log(module_ctx, "warning", "Failed to create filter dump file: %s", filter_filename);
        unlink(storage_filename);
        return EXTERNAL_ERROR;
    }
    
    /* Iterate through all databases and dump their filter data */
    for (int db = 0; db < MAX_DB; db++) {
        if (filter_mem_pool[db] == NULL) continue;
        
        ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(filter_mem_pool[db], "^", NULL, 0);
        ValkeyModuleString *key;
        while ((key = ValkeyModule_DictNext(NULL, iter, NULL)) != NULL) {
            size_t key_len;
            const char *key_str = ValkeyModule_StringPtrLen(key, &key_len);
            
            /* Format: db_id key_len key_data */
            fprintf(filter_file, "%d %zu %.*s\n", db, key_len, (int)key_len, key_str);
            ValkeyModule_FreeString(NULL, key);
        }
        ValkeyModule_DictIteratorStop(iter);
    }
    fclose(filter_file);
    
    ValkeyModule_Log(module_ctx, "notice", "Dumped external data to %s and %s",
                     storage_filename, filter_filename);
    
    if (backup_id != NULL) {
        *backup_id = ValkeyModule_CreateString(module_ctx, backup_id_str, strlen(backup_id_str));
    }
    
    return EXTERNAL_SUCCESS;
}


static int storageLoadFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalStorageCtx *storage_ctx,
                               int dbid,
                               ValkeyModuleString *backup_id,
                               ValkeyModuleString *source) {
    VALKEYMODULE_NOT_USED(storage_ctx);
    VALKEYMODULE_NOT_USED(dbid);
    ValkeyModule_AutoMemory(module_ctx);
    
    /* Set loading flag to queue incoming commands */
    is_loading = 1;
    
    /* Extract source server ID (host:port) */
    char source_server_id[256];
    if (source != NULL) {
        size_t source_len;
        const char *source_str = ValkeyModule_StringPtrLen(source, &source_len);
        snprintf(source_server_id, sizeof(source_server_id), "%.*s", (int)source_len, source_str);
        ValkeyModule_Log(module_ctx, "notice", "Loading from source: %s", source_server_id);
        ValkeyModule_Log(module_ctx, "warning", "DEBUG: source_str='%s', source_len=%zu, server_id='%s'",
                         source_str, source_len, server_id);
    } else {
        /* Use local server ID */
        snprintf(source_server_id, sizeof(source_server_id), "%s", server_id);
        ValkeyModule_Log(module_ctx, "notice", "Loading from local instance: %s", source_server_id);
        ValkeyModule_Log(module_ctx, "warning", "DEBUG: using local server_id='%s'", server_id);
    }
    
    char backup_id_buf[256];
    size_t backup_id_len;
    const char *backup_id_str;
    
    if (backup_id == NULL) {
        /* Find the most recent backup file using glob pattern */
        ValkeyModule_Log(module_ctx, "notice", "No backup_id provided, using most recent backup");
        
        /* Use server-specific directory */
        char source_dir[1024];
        
        /* Use localhost as the hostname to match what the primary used */
        const char *final_hostname = "localhost";
        
        /* Extract port from source_server_id to use the primary's port */
        int primary_port = 6379; /* Default primary port */
        if (strstr(source_server_id, ":") != NULL) {
            /* Extract port part if source_server_id contains host:port format */
            char port_str[16];
            const char *port_start = strchr(source_server_id, ':') + 1;
            strncpy(port_str, port_start, sizeof(port_str) - 1);
            port_str[sizeof(port_str) - 1] = '\0';
            primary_port = atoi(port_str);
        }
        
        snprintf(source_dir, sizeof(source_dir), "/tmp/external_data/%s:%d",
                 final_hostname, primary_port);
        ValkeyModule_Log(module_ctx, "warning", "DEBUG: Looking for backups in directory: %s (source_server_id=%s, primary_port=%d)",
                 source_dir, source_server_id, primary_port);
        ValkeyModule_Log(module_ctx, "warning", "DEBUG: storageLoadFunction - constructed source_dir='%s' from source_server_id='%s'", source_dir, source_server_id);
        
        /* Use a simple heuristic: check the last 10 seconds for backup files */
        time_t current_time = time(NULL);
        int found_backup = 0;
        
        ValkeyModule_Log(module_ctx, "warning", "DEBUG: Searching for backup files in last 10 seconds, current_time=%ld", current_time);
        
        for (int i = 0; i < 10 && !found_backup; i++) {
            time_t t = current_time - i;
            snprintf(backup_id_buf, sizeof(backup_id_buf), "backup_%d_%lld", -1, (long long)t);
            
            char test_filename[1536];
            snprintf(test_filename, sizeof(test_filename), "%s/%s_storage_%s.dat", source_dir, module_name, backup_id_buf);
            
            ValkeyModule_Log(module_ctx, "warning", "DEBUG: Checking backup file: %s (timestamp: %ld)", test_filename, t);
            
            if (access(test_filename, F_OK) == 0) {
                found_backup = 1;
                backup_id_str = backup_id_buf;
                backup_id_len = strlen(backup_id_buf);
                ValkeyModule_Log(module_ctx, "notice", "Using most recent backup: %s", backup_id_str);
                ValkeyModule_Log(module_ctx, "warning", "DEBUG: Found backup file: %s", test_filename);
                break;
            }
        }
        
        if (!found_backup) {
            ValkeyModule_Log(module_ctx, "warning", "No backup files found in last 10 seconds in %s", source_dir);
            /* List all files in the directory for debugging */
            DIR *dir = opendir(source_dir);
            if (dir) {
                struct dirent *entry;
                ValkeyModule_Log(module_ctx, "warning", "DEBUG: Files in directory %s:", source_dir);
                while ((entry = readdir(dir)) != NULL) {
                    if (strstr(entry->d_name, ".dat") != NULL) {
                        ValkeyModule_Log(module_ctx, "warning", "DEBUG:   %s", entry->d_name);
                    }
                }
                closedir(dir);
            }
            return EXTERNAL_ERROR;
        }
    } else {
        backup_id_str = ValkeyModule_StringPtrLen(backup_id, &backup_id_len);
    }
    
    /* Load storage data from disk using source server directory */
    char source_dir[1024];
    
    /* Use localhost as the hostname to match what the primary used */
    const char *final_hostname = "localhost";
    
    /* Extract port from source_server_id to use the primary's port */
    int primary_port = 6379; /* Default primary port */
    if (strstr(source_server_id, ":") != NULL) {
        /* Extract port part if source_server_id contains host:port format */
        char port_str[16];
        const char *port_start = strchr(source_server_id, ':') + 1;
        strncpy(port_str, port_start, sizeof(port_str) - 1);
        port_str[sizeof(port_str) - 1] = '\0';
        primary_port = atoi(port_str);
    }
    
    snprintf(source_dir, sizeof(source_dir), "/tmp/external_data/%s:%d",
             final_hostname, primary_port);
    ValkeyModule_Log(module_ctx, "warning", "DEBUG: Loading storage from directory: %s", source_dir);
    
    char storage_filename[1536];
    snprintf(storage_filename, sizeof(storage_filename), "%s/%s_storage_%.*s.dat",
             source_dir, module_name, (int)backup_id_len, backup_id_str);
    
    ValkeyModule_Log(module_ctx, "notice", "Loading storage from: %s", storage_filename);
    
    FILE *storage_file = fopen(storage_filename, "r");
    if (!storage_file) {
        ValkeyModule_Log(module_ctx, "warning", "Failed to open storage dump file: %s", storage_filename);
        is_loading = 0;
        return EXTERNAL_ERROR;
    }
    
    /* Verify filter file exists before clearing data */
    char filter_filename[1536];
    snprintf(filter_filename, sizeof(filter_filename), "%s/%s_filter_%.*s.dat",
             source_dir, module_name, (int)backup_id_len, backup_id_str);
    
    FILE *filter_file = fopen(filter_filename, "r");
    if (!filter_file) {
        ValkeyModule_Log(module_ctx, "warning", "Failed to open filter dump file: %s", filter_filename);
        fclose(storage_file);
        is_loading = 0;
        return EXTERNAL_ERROR;
    }
    
    /* Both files exist, now we can safely clear existing data */
    for (int i = 0; i < MAX_DB; i++) {
        /* Clear storage */
        if (storage_mem_pool[i] != NULL) {
            ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(storage_mem_pool[i], "^", NULL, 0);
            ValkeyModuleString *key;
            while ((key = ValkeyModule_DictNext(NULL, iter, NULL)) != NULL) {
                ValkeyModuleString *value = ValkeyModule_DictGet(storage_mem_pool[i], key, NULL);
                ValkeyModule_FreeString(NULL, key);
                if (value != NULL) {
                    ValkeyModule_FreeString(NULL, value);
                }
            }
            ValkeyModule_DictIteratorStop(iter);
            ValkeyModule_FreeDict(NULL, storage_mem_pool[i]);
        }
        storage_mem_pool[i] = ValkeyModule_CreateDict(NULL);
        
        /* Clear filter */
        if (filter_mem_pool[i] != NULL) {
            ValkeyModule_FreeDict(NULL, filter_mem_pool[i]);
        }
        filter_mem_pool[i] = ValkeyModule_CreateDict(NULL);
    }
    
    /* Read and restore storage data */
    int db;
    size_t key_len, val_len;
    char line[8192];
    int loaded_count = 0;
    while (fgets(line, sizeof(line), storage_file)) {
        if (sscanf(line, "%d %zu", &db, &key_len) != 2) continue;
        if (db < 0 || db >= MAX_DB) continue;
        
        /* Parse key */
        char *key_start = line;
        for (int i = 0; i < 2; i++) {
            key_start = strchr(key_start, ' ');
            if (!key_start) break;
            key_start++;
        }
        if (!key_start) continue;
        
        char *val_len_start = key_start + key_len + 1;
        if (sscanf(val_len_start, "%zu", &val_len) != 1) continue;
        
        /* Parse value */
        char *val_start = val_len_start;
        val_start = strchr(val_start, ' ');
        if (!val_start) continue;
        val_start++;
        
        /* Remove trailing newline from value */
        if (val_start[val_len] == '\n') {
            val_start[val_len] = '\0';
        }
        
        /* Create strings and store in memory pool */
        ValkeyModuleString *key_str = ValkeyModule_CreateString(NULL, key_start, key_len);
        ValkeyModuleString *val_str = ValkeyModule_CreateString(NULL, val_start, val_len);
        
        ValkeyModule_DictReplace(storage_mem_pool[db], key_str, val_str);
        ValkeyModule_RetainString(NULL, val_str);
        ValkeyModule_FreeString(NULL, key_str);
        loaded_count++;
        ValkeyModule_Log(module_ctx, "debug", "Loaded storage key: db=%d, key_len=%zu, val_len=%zu", db, key_len, val_len);
    }
    fclose(storage_file);
    ValkeyModule_Log(module_ctx, "notice", "Loaded %d storage entries from %s", loaded_count, storage_filename);
    
    /* Read and restore filter data */
    while (fgets(line, sizeof(line), filter_file)) {
        if (sscanf(line, "%d %zu", &db, &key_len) != 2) continue;
        if (db < 0 || db >= MAX_DB) continue;
        
        /* Parse key */
        char *key_start = line;
        for (int i = 0; i < 2; i++) {
            key_start = strchr(key_start, ' ');
            if (!key_start) break;
            key_start++;
        }
        if (!key_start) continue;
        
        /* Remove trailing newline from key */
        if (key_start[key_len] == '\n') {
            key_start[key_len] = '\0';
        }
        
        /* Create string and store in memory pool */
        ValkeyModuleString *key_str = ValkeyModule_CreateString(NULL, key_start, key_len);
        ValkeyModule_DictReplace(filter_mem_pool[db], key_str, "");
        ValkeyModule_FreeString(NULL, key_str);
    }
    fclose(filter_file);
    
    ValkeyModule_Log(module_ctx, "notice", "Loaded external data from %s and %s",
                     storage_filename, filter_filename);
    
    /* Clear loading flag */
    is_loading = 0;
    
    /* Process queued commands */
    QueuedCommand *cmd = command_queue_head;
    int processed_count = 0;
    while (cmd != NULL) {
        QueuedCommand *next = cmd->next;
        
        /* Process the command - add to storage and filter */
        ValkeyModuleString *prev_value = ValkeyModule_DictGet(storage_mem_pool[cmd->dbid], cmd->key, NULL);
        ValkeyModule_DictReplace(storage_mem_pool[cmd->dbid], cmd->key, cmd->value);
        ValkeyModule_RetainString(NULL, cmd->value);
        if (prev_value != NULL) {
            ValkeyModule_FreeString(NULL, prev_value);
        }
        
        /* Add to filter */
        ValkeyModule_DictReplace(filter_mem_pool[cmd->dbid], cmd->key, "");
        
        /* Free the queued command */
        ValkeyModule_FreeString(NULL, cmd->key);
        ValkeyModule_FreeString(NULL, cmd->value);
        ValkeyModule_Free(cmd);
        
        processed_count++;
        cmd = next;
    }
    
    /* Clear the queue */
    command_queue_head = command_queue_tail = NULL;
    
    if (processed_count > 0) {
        ValkeyModule_Log(module_ctx, "notice", "Processed %d queued commands after loading", processed_count);
    }
    
    return EXTERNAL_SUCCESS;
}

/* Helper function to find databases from backup files - reused by get_state */
static int findDatabasesFromBackup(ValkeyModuleCtx *module_ctx, ValkeyModuleString *source, int **db_numbers, size_t *num_dbs) {
    /* Look for the NEWEST backup from the last 300 seconds (5 minutes) */
    time_t current_time = time(NULL);
    int found_backup = 0;
    time_t best_backup_time = 0;
    
    /* Determine which directory to look in based on source parameter */
    char server_dir[1024];
    if (source != NULL) {
        /* We're on a replica - use the primary's directory */
        size_t source_len;
        const char *source_str = ValkeyModule_StringPtrLen(source, &source_len);
        
        /* Extract hostname and port from source (format: "host:port") */
        const char *final_hostname = "localhost";
        int primary_port = 6379;
        if (strstr(source_str, ":") != NULL) {
            /* Find the position of the colon to split host and port */
            const char *colon_pos = strchr(source_str, ':');
            if (colon_pos != NULL) {
                /* Use the hostname part as-is (could be IP or hostname) */
                final_hostname = source_str;
                /* Extract port part */
                char port_str[16];
                const char *port_start = colon_pos + 1;
                strncpy(port_str, port_start, sizeof(port_str) - 1);
                port_str[sizeof(port_str) - 1] = '\0';
                primary_port = atoi(port_str);
            }
        }
        
        snprintf(server_dir, sizeof(server_dir), "/tmp/external_data/%s",
                 final_hostname);
        ValkeyModule_Log(module_ctx, "notice", "Looking for backups in primary's directory: %s", server_dir);
    } else {
        /* We're on primary - use local directory */
        snprintf(server_dir, sizeof(server_dir), "/tmp/external_data/%s", server_id);
        ValkeyModule_Log(module_ctx, "notice", "Looking for backups in local directory: %s", server_dir);
    }
    
    ValkeyModule_Log(module_ctx, "warning", "DEBUG: Searching for backup files in last 300 seconds, server_dir=%s", server_dir);
    
    for (int i = 0; i < 300; i++) {
        time_t t = current_time - i;
        char backup_id_str[256];
        snprintf(backup_id_str, sizeof(backup_id_str), "backup_%d_%lld", -1, (long long)t);
        
        char test_filename[1536];
        snprintf(test_filename, sizeof(test_filename),
                "%s/%s_storage_%s.dat", server_dir, module_name, backup_id_str);
        
        ValkeyModule_Log(module_ctx, "warning", "DEBUG: Checking backup file: %s (timestamp: %lld)", test_filename, (long long)t);
        
        if (access(test_filename, F_OK) == 0) {
            /* Found a backup - since we're iterating from newest to oldest,
             * the first one we find is the newest */
            found_backup = 1;
            best_backup_time = t;
            ValkeyModule_Log(module_ctx, "notice", "Found recent backup at timestamp %lld", (long long)t);
            ValkeyModule_Log(module_ctx, "warning", "DEBUG: Found backup file: %s", test_filename);
            break; /* Stop at the first (newest) backup found */
        }
    }
    
    if (!found_backup) {
        /* List all files in the directory for debugging */
        DIR *dir = opendir(server_dir);
        if (dir) {
            struct dirent *entry;
            ValkeyModule_Log(module_ctx, "warning", "DEBUG: Files in directory %s:", server_dir);
            while ((entry = readdir(dir)) != NULL) {
                if (strstr(entry->d_name, ".dat") != NULL) {
                    ValkeyModule_Log(module_ctx, "warning", "DEBUG:   %s", entry->d_name);
                }
            }
            closedir(dir);
        }
    }
    
    if (!found_backup) {
        ValkeyModule_Log(module_ctx, "warning", "No backup files found in last 300 seconds in %s", server_dir);
        return EXTERNAL_ERROR;
    }
    
    /* Parse the backup file to determine which databases it contains */
    char backup_id_str[256];
    snprintf(backup_id_str, sizeof(backup_id_str), "backup_%d_%lld", -1, (long long)best_backup_time);
    
    char storage_filename[1536];
    snprintf(storage_filename, sizeof(storage_filename), "%s/%s_storage_%s.dat", server_dir, module_name, backup_id_str);
    
    FILE *storage_file = fopen(storage_filename, "r");
    if (!storage_file) {
        ValkeyModule_Log(module_ctx, "warning", "Failed to open backup file to read database list: %s", storage_filename);
        return EXTERNAL_ERROR;
    }
    
    /* Scan the file to find which databases are present */
    int db_set[MAX_DB] = {0};  /* Track which databases we've seen */
    int db_count = 0;
    char line[8192];
    while (fgets(line, sizeof(line), storage_file)) {
        int db;
        if (sscanf(line, "%d ", &db) == 1) {
            if (db >= 0 && db < MAX_DB && !db_set[db]) {
                db_set[db] = 1;
                db_count++;
            }
        }
    }
    fclose(storage_file);
    
    if (db_count == 0) {
        ValkeyModule_Log(module_ctx, "warning", "No databases found in backup file");
        return EXTERNAL_ERROR;
    }
    
    /* Allocate and fill the db_numbers array using ValkeyModule_Alloc
     * The core will free it using the module's Free function */
    *db_numbers = ValkeyModule_Alloc(sizeof(int) * db_count);
    if (*db_numbers == NULL) {
        ValkeyModule_Log(module_ctx, "warning", "Failed to allocate memory for db_numbers");
        return EXTERNAL_ERROR;
    }
    int idx = 0;
    for (int db = 0; db < MAX_DB; db++) {
        if (db_set[db]) {
            (*db_numbers)[idx++] = db;
        }
    }
    *num_dbs = db_count;
    
    ValkeyModule_Log(module_ctx, "notice", "Found %d database(s) in backup", db_count);
    return EXTERNAL_SUCCESS;
}

static int filterGetStateFunction(ValkeyModuleCtx *module_ctx,
                                  ValkeyModuleExternalFilterCtx *filter_ctx,
                                  ValkeyModuleString *source,
                                  int **db_numbers,
                                  size_t *num_dbs) {
    VALKEYMODULE_NOT_USED(filter_ctx);
    ValkeyModule_AutoMemory(module_ctx);
    
    /* Reuse the helper function to find databases from backup */
    return findDatabasesFromBackup(module_ctx, source, db_numbers, num_dbs);
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

    /* Get the actual server port and update server_id */
    ValkeyModuleServerInfoData *info = ValkeyModule_GetServerInfo(ctx, "server");
    if (info) {
        long long port = ValkeyModule_ServerInfoGetFieldSigned(info, "tcp_port", NULL);
        if (port > 0) {
            snprintf(server_id, sizeof(server_id), "localhost:%lld", port);
            ValkeyModule_Log(ctx, "notice", "Module %s initialized with server_id=%s", module_name, server_id);
        }
        ValkeyModule_FreeServerInfo(ctx, info);
    }

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
        .keys_count = filterKeysCountFunction,
        .get_state = filterGetStateFunction
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
