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
#include <ctype.h>
#include <sys/time.h>

/*
 * This module implements a combined external storage and filter.
 * It follows the structure defined in src/external_data.c.
 */

const char *module_name = "helloextdata1";
static char server_id[256] = "127.0.0.1:6379"; /* Default server ID, will be updated from target */

/* Helper function to get external data path and replace localhost with 127.0.0.1 */
static char* get_external_data_path(ValkeyModuleCtx *module_ctx, const char *server_id) {
    static char path[1024];
    
    /* Create the base path */
    snprintf(path, sizeof(path), "/tmp/external_data/%s", server_id);
    
    /* Replace localhost with 127.0.0.1 if present */
    char *localhost_pos = strstr(path, "localhost");
    if (localhost_pos != NULL) {
        /* Replace "localhost" with "127.0.0.1" */
        char temp_path[1024];
        snprintf(temp_path, sizeof(temp_path), "%.*s127.0.0.1%s",
                (int)(localhost_pos - path), path, localhost_pos + 9);
        strncpy(path, temp_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    
    ValkeyModule_Log(module_ctx, "debug", "get_external_data_path: returning %s", path);
    return path;
}

#define MAX_DB 16
ValkeyModuleDict *storage_mem_pool[MAX_DB];  // Memory pool for storage
ValkeyModuleDict *filter_mem_pool[MAX_DB];  // Memory pool for filter
ValkeyModuleDict *storage_snapshot_pool[MAX_DB]; // Snapshot pool for storage
ValkeyModuleDict *filter_snapshot_pool[MAX_DB]; // Snapshot pool for filter

/* Failure simulation configuration */
static long long set_failure_percent = 0;  // 0 = no failures, >0 = p out of 100 sets fail
static int set_operation_counter = 0;  // Counter for set operations

/* Auto-dump configuration */
static int dump_every_write = 0;  // 0 = no auto-dump, 1 = dump on every write

/* Backup ID coordination for same-second dumps */
static long long last_backup_second = 0;
static int backup_sequence_counter = 0;
static char last_backup_id[256] = {0};
static int reuse_last_backup_id = 0;  /* Flag: 1 = reuse last_backup_id, 0 = generate new */

/* Loading state and command queue */
static int is_loading[MAX_DB] = {0};  // Flag to indicate if we're currently loading from backup
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
    ValkeyModule_Log(NULL, "notice", "DEBUG: setFailurePercentConfig called - set_failure_percent=%lld", new_val);
    if (new_val < 0 || new_val > 100) {
        *err = ValkeyModule_CreateString(NULL, "set_failure_percent must be between 0 and 100", 47);
        return VALKEYMODULE_ERR;
    }
    set_failure_percent = new_val;
    /* Reset counter when changing failure rate */
    set_operation_counter = 0;
    return VALKEYMODULE_OK;
}

/* Configuration callbacks for dump_every_write */
static long long getDumpEveryWriteConfig(const char *name, void *privdata) {
    VALKEYMODULE_NOT_USED(name);
    VALKEYMODULE_NOT_USED(privdata);
    return dump_every_write;
}

static int setDumpEveryWriteConfig(const char *name, long long new_val, void *privdata, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(name);
    VALKEYMODULE_NOT_USED(privdata);
    ValkeyModule_Log(NULL, "debug", "DEBUG: setDumpEveryWriteConfig called - dump_every_write=%lld",
        new_val);
    if (new_val != 0 && new_val != 1) {
        *err = ValkeyModule_CreateString(NULL, "dump_every_write must be 0 or 1", 36);
        return VALKEYMODULE_ERR;
    }
    dump_every_write = new_val;
    return VALKEYMODULE_OK;
}

/* Helper function to drain queued commands after loading completes */
static void drainCommandQueue(ValkeyModuleCtx *module_ctx, int dbid) {
    /* Check if queue is empty */
    if (command_queue_head == NULL) {
        ValkeyModule_Log(module_ctx, "debug", "Command queue is empty for dbid=%d, nothing to drain", dbid);
        return;
    }
    
    ValkeyModule_Log(module_ctx, "notice", "Draining command queue for dbid=%d", dbid);
    
    QueuedCommand *cmd = command_queue_head;
    QueuedCommand *prev = NULL;
    int drained_count = 0;
    
    while (cmd != NULL) {
        if (cmd->dbid == dbid) {
            /* Process this command now that loading is complete */
            /* For storage - add to storage pool */
            if (storage_mem_pool[dbid] != NULL) {
                ValkeyModule_DictReplace(storage_mem_pool[dbid], cmd->key, cmd->value);
                ValkeyModule_RetainString(NULL, cmd->value);
            }
            
            /* For filter - add to filter pool */
            if (filter_mem_pool[dbid] != NULL) {
                ValkeyModule_DictReplace(filter_mem_pool[dbid], cmd->key, "");
            }
            
            ValkeyModule_Log(module_ctx, "debug", "Processed queued command for key in storage and filter");
            
            /* Remove from queue */
            if (prev == NULL) {
                command_queue_head = cmd->next;
            } else {
                prev->next = cmd->next;
            }
            if (cmd == command_queue_tail) {
                command_queue_tail = prev;
            }
            
            QueuedCommand *next = cmd->next;
            ValkeyModule_FreeString(NULL, cmd->key);
            ValkeyModule_FreeString(NULL, cmd->value);
            ValkeyModule_Free(cmd);
            drained_count++;
            cmd = next;
        } else {
            prev = cmd;
            cmd = cmd->next;
        }
    }
    
    ValkeyModule_Log(module_ctx, "notice", "Drained %d queued commands for dbid=%d", drained_count, dbid);
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

/* Snapshot methods */
static void *storageSnapshotFunction(ValkeyModuleCtx *module_ctx,
                                     ValkeyModuleExternalStorageCtx *storage_ctx,
                                     int dbid) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(storage_ctx);

    if (dbid < 0 || dbid >= MAX_DB || storage_mem_pool[dbid] == NULL) {
        return NULL;
    }

    /* Create a deep copy of the dictionary */
    ValkeyModuleDict *snapshot = ValkeyModule_CreateDict(NULL);
    ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(storage_mem_pool[dbid], "^", NULL, 0);
    char *key;
    size_t key_len;
    ValkeyModuleString *value;

    while ((key = ValkeyModule_DictNextC(iter, &key_len, (void **)&value)) != NULL) {
        if (value == NULL) continue;

        ValkeyModuleString *key_str = ValkeyModule_CreateString(NULL, key, key_len);
        ValkeyModule_DictReplace(snapshot, key_str, value);
        ValkeyModule_RetainString(NULL, value); /* Retain value for the snapshot */
        ValkeyModule_FreeString(NULL, key_str);
    }
    ValkeyModule_DictIteratorStop(iter);

    storage_snapshot_pool[dbid] = snapshot;
    return snapshot;
}

static void storageFreeSnapshotFunction(ValkeyModuleCtx *module_ctx,
                                        ValkeyModuleExternalStorageCtx *storage_ctx,
                                        void *snapshot) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(storage_ctx);

    if (snapshot == NULL) return;

    ValkeyModuleDict *dict = (ValkeyModuleDict *)snapshot;
    
    /* Clear from pool */
    for (int i = 0; i < MAX_DB; i++) {
        if (storage_snapshot_pool[i] == dict) {
            storage_snapshot_pool[i] = NULL;
            break;
        }
    }

    /* Free retained values */
    ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(dict, "^", NULL, 0);
    char *key;
    ValkeyModuleString *value;
    
    while ((key = ValkeyModule_DictNextC(iter, NULL, (void **)&value)) != NULL) {
        if (value) {
            ValkeyModule_FreeString(NULL, value);
        }
    }
    ValkeyModule_DictIteratorStop(iter);

    ValkeyModule_FreeDict(NULL, dict);
}

static void *filterSnapshotFunction(ValkeyModuleCtx *module_ctx,
                                    ValkeyModuleExternalFilterCtx *filter_ctx,
                                    int dbid) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(filter_ctx);

    if (dbid < 0 || dbid >= MAX_DB || filter_mem_pool[dbid] == NULL) {
        return NULL;
    }

    /* Create a deep copy of the dictionary */
    ValkeyModuleDict *snapshot = ValkeyModule_CreateDict(NULL);
    ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(filter_mem_pool[dbid], "^", NULL, 0);
    char *key;
    size_t key_len;

    while ((key = ValkeyModule_DictNextC(iter, &key_len, NULL)) != NULL) {
        ValkeyModuleString *key_str = ValkeyModule_CreateString(NULL, key, key_len);
        ValkeyModule_DictReplace(snapshot, key_str, "");
        ValkeyModule_FreeString(NULL, key_str);
    }
    ValkeyModule_DictIteratorStop(iter);

    filter_snapshot_pool[dbid] = snapshot;
    return snapshot;
}

static void filterFreeSnapshotFunction(ValkeyModuleCtx *module_ctx,
                                       ValkeyModuleExternalFilterCtx *filter_ctx,
                                       void *snapshot) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(filter_ctx);

    if (snapshot == NULL) return;

    ValkeyModuleDict *dict = (ValkeyModuleDict *)snapshot;

    /* Clear from pool */
    for (int i = 0; i < MAX_DB; i++) {
        if (filter_snapshot_pool[i] == dict) {
            filter_snapshot_pool[i] = NULL;
            break;
        }
    }

    ValkeyModule_FreeDict(NULL, dict);
}
/* Storage methods */
static int storageDumpFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalStorageCtx *storage_ctx,
                               int dbid,
                               int slot,
                               long long timestamp,
                               ValkeyModuleString *target,
                               ValkeyModuleString **backup_id);

static int storageSetFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalStorageCtx *storage_ctx,
                               ValkeyModuleKeyOptCtx *key_ctx,
                               ValkeyModuleString *value) {
    ValkeyModule_AutoMemory(module_ctx);
    
    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    
    if (key == NULL) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: storageSetFunction called with NULL key");
        return EXTERNAL_ERROR;
    }

    ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageSetFunction called - key=%s, dbid=%d, dump_every_write=%d",
                     ValkeyModule_StringPtrLen(key, NULL), dbid, dump_every_write);
    
    /* If we're currently loading, queue this command for later processing */
    if (is_loading[dbid]) {
        ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageSetFunction - storage is loading");
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
        
        ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageSetFunction - command queued during loading");
        return EXTERNAL_SUCCESS;
    }
    
    ValkeyModuleExternalStorageState state = waitExternalStorageReady(storage_ctx);
    ValkeyModule_Assert(state == VMES_STATE_READONLY || state == VMES_STATE_READY);
    if (state == VMES_STATE_READONLY) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: External storage readonly");
        return EXTERNAL_ERROR;
    }

    ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageSetFunction - failure percent: %d", set_failure_percent);
    /* Simulate failures if configured */
    if (set_failure_percent > 0) {
        set_operation_counter++;
        /* Fail every (100/set_failure_percent)th operation starting from the 1st */
        int fail_interval = 100 / set_failure_percent;
        ValkeyModule_Log(module_ctx, "notice", "Simulating storage failure: fail_interval=%d, counter=%d, mod=%d",
            fail_interval, set_operation_counter, set_operation_counter % fail_interval);
        if (fail_interval > 0 && (set_operation_counter % fail_interval) == 1) {
            ValkeyModule_Log(module_ctx, "warning", "Simulated storage failure");
            return EXTERNAL_ERROR;
        }
    }

    ValkeyModuleString *previous_value =
        ValkeyModule_DictGet(storage_mem_pool[dbid], (ValkeyModuleString *)key, NULL);

    if (previous_value != NULL &&
        ValkeyModule_StringCompare(previous_value, value) == 0) {
        ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageSetFunction - value unchanged");
        return EXTERNAL_SUCCESS;
    }

    ValkeyModule_DictReplace(storage_mem_pool[dbid], (ValkeyModuleString *)key, value);
    // Retain the value to keep it alive in our storage
    ValkeyModule_RetainString(NULL, value);
    
    if (previous_value != NULL) {
        // Free the previous value if it exists
        ValkeyModule_FreeString(NULL, previous_value);
    }
    
    /* Auto-dump on every write if configured */
    if (dump_every_write) {
        ValkeyModule_Log(module_ctx, "notice", "Auto-dumping data after SET operation (dump_every_write=1)");
        ValkeyModule_Log(module_ctx, "debug", "AUTO_DUMP: server_id=%s, dbid=%d", server_id, dbid);
        /* Call storage dump function with appropriate parameters */
        ValkeyModuleString *backup_id = NULL;
        
        ValkeyModule_Log(module_ctx, "debug", "AUTO_DUMP: Calling storageDumpFunction with target=%s", server_id);
        ValkeyModuleString *server_id_str = ValkeyModule_CreateStringPrintf(NULL, "%s", server_id);
        storageDumpFunction(module_ctx, storage_ctx, dbid, 0, time(NULL), server_id_str, &backup_id);
        ValkeyModule_Log(module_ctx, "debug", "AUTO_DUMP: storageDumpFunction completed");
        
        if (backup_id) {
            ValkeyModule_FreeString(NULL, backup_id);
        }
    } else {
        ValkeyModule_Log(module_ctx, "debug", "AUTO_DUMP: dump_every_write is 0, skipping automatic dump");
    }
    
    ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageSetFunction - success");
    return EXTERNAL_SUCCESS;
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
    
    if (key == NULL) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: storageGetFunction called with NULL key");
        return EXTERNAL_ERROR;
    }
    
    size_t dict_size = storage_mem_pool[dbid] ? ValkeyModule_DictSize(storage_mem_pool[dbid]) : 0;
    ValkeyModule_Log(module_ctx, "debug", "storageGetFunction: dbid=%d, key=%s, dict_size=%zu",
                     dbid, ValkeyModule_StringPtrLen(key, NULL), dict_size);

    void *value = ValkeyModule_DictGet(storage_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    if (!value) {
        ValkeyModule_Log(module_ctx, "debug", "storageGetFunction: key not found in storage");
        return 0;
    }

    ValkeyModule_Log(module_ctx, "debug", "storageGetFunction: key found, returning value");
    if (found != NULL) {
        *found = value;
    }

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
        ValkeyModule_Log(module_ctx, "error", "ERROR: External storage readonly");
        return EXTERNAL_ERROR;
    }

    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    
    if (key == NULL) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: storageDelFunction called with NULL key");
        return EXTERNAL_ERROR;
    }
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
        ValkeyModule_ReplyWithErrorFormat(module_ctx, "ERR Failed to del key %s",
                                          ValkeyModule_StringPtrLen(key, NULL) );
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
static int filterDumpFunction(ValkeyModuleCtx *module_ctx,
                              ValkeyModuleExternalFilterCtx *filter_ctx,
                              int dbid,
                              int slot,
                              long long timestamp,
                              ValkeyModuleString *target,
                              ValkeyModuleString **backup_id);

static int filterSetFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalFilterCtx *filter_ctx,
                               ValkeyModuleKeyOptCtx *key_ctx) {
    ValkeyModule_AutoMemory(module_ctx);
    
    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    ValkeyModule_Log(module_ctx, "debug", "DEBUG: filterSetFunction called");
    
    if (key == NULL) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: filterSetFunction called with NULL key");
        return EXTERNAL_ERROR;
    }
    
    /* If we're currently loading, just return success - storage will queue the commands */
    if (is_loading[dbid]) {
        ValkeyModule_Log(module_ctx, "debug", "DEBUG: filterSetFunction - storage is loading, just return OK");
        return EXTERNAL_SUCCESS;
    }
    
    ValkeyModuleExternalFilterState state = waitExternalFilterReady(filter_ctx);
    ValkeyModule_Assert(state == VMEF_STATE_READONLY || state == VMEF_STATE_READY);
    if (state == VMEF_STATE_READONLY) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: External filter readonly");
        return EXTERNAL_ERROR;
    }

    ValkeyModuleString *previous_value =
        ValkeyModule_DictGet(filter_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    
    if (previous_value != NULL) {
        ValkeyModule_Log(module_ctx, "debug", "DEBUG: filterSetFunction - key already exists");
        return EXTERNAL_SUCCESS;
    }

    if (ValkeyModule_DictReplace(filter_mem_pool[dbid], (ValkeyModuleString *)key, "") == VALKEYMODULE_ERR) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: Failed to set key in filter");
        return EXTERNAL_ERROR;
    }

    ValkeyModule_Log(module_ctx, "debug", "DEBUG: filterSetFunction - set successfully: %zu",
                     ValkeyModule_DictSize(filter_mem_pool[dbid]));

    /* Auto-dump on every write if configured */
    if (dump_every_write) {
        ValkeyModule_Log(module_ctx, "notice", "Auto-dumping filter data after SET operation (dump_every_write=1)");
        ValkeyModuleString *backup_id = NULL;
        ValkeyModuleString *server_id_str = ValkeyModule_CreateStringPrintf(NULL, "%s", server_id);
        filterDumpFunction(module_ctx, filter_ctx, dbid, 0, time(NULL), server_id_str, &backup_id);
        if (backup_id) {
            ValkeyModule_FreeString(NULL, backup_id);
        }
    }

    ValkeyModule_Log(module_ctx, "debug", "DEBUG: filterSetFunction - success");
    return EXTERNAL_SUCCESS;
}

static int filterGetFunction(ValkeyModuleCtx *module_ctx,
                              ValkeyModuleExternalFilterCtx *,
                              ValkeyModuleKeyOptCtx *key_ctx) {
    ValkeyModule_AutoMemory(module_ctx);
    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    
    if (key == NULL) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: filterGetFunction called with NULL key");
        return EXTERNAL_ERROR;
    }
    size_t dict_size = filter_mem_pool[dbid] ? ValkeyModule_DictSize(filter_mem_pool[dbid]) : 0;
    ValkeyModule_Log(module_ctx, "debug", "filterGetFunction: dbid=%d, key=%s, dict_size=%zu",
                     dbid, ValkeyModule_StringPtrLen(key, NULL), dict_size);

    size_t length;
    ValkeyModule_StringPtrLen(key, &length);
    if (length == 0) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: filterGetFunction called with empty key");
        return EXTERNAL_ERROR;
    }

    void *value = ValkeyModule_DictGet(filter_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    int found = (value != NULL);
    ValkeyModule_Log(module_ctx, "debug", "filterGetFunction: key %s in filter", found ? "found" : "not found");
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
        ValkeyModule_Log(module_ctx, "error", "ERROR: External filter readonly");
        return EXTERNAL_ERROR;
    }

    int dbid = ValkeyModule_GetDbIdFromOptCtx(key_ctx);
    const ValkeyModuleString *key = ValkeyModule_GetKeyNameFromOptCtx(key_ctx);
    
    if (key == NULL) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: filterDelFunction called with NULL key");
        return EXTERNAL_ERROR;
    }
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
    
    if (storage_mem_pool[dbid] == NULL) {
        return EXTERNAL_SUCCESS;
    }

    is_loading[dbid] = 1;
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
    storage_mem_pool[dbid] = ValkeyModule_CreateDict(NULL);
    is_loading[dbid] = 0;

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

static int filterDumpFunction(ValkeyModuleCtx *module_ctx,
                              ValkeyModuleExternalFilterCtx *filter_ctx,
                              int dbid,
                              int slot,
                              long long timestamp,
                              ValkeyModuleString *target,
                              ValkeyModuleString **backup_id) {
    VALKEYMODULE_NOT_USED(filter_ctx);
    VALKEYMODULE_NOT_USED(slot);
    VALKEYMODULE_NOT_USED(timestamp);
    ValkeyModule_AutoMemory(module_ctx);

    /* Check if we have a snapshot available in the global pool */
    ValkeyModuleDict *snapshot_dict = NULL;
    if (dbid >= 0 && dbid < MAX_DB && filter_snapshot_pool[dbid]) {
        snapshot_dict = filter_snapshot_pool[dbid];
        ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: using snapshot for dump of db%d", dbid);
    }
    
    /* Extract server ID from target parameter if provided, otherwise use local server ID */
    char target_server_id[1024];
    if (target != NULL) {
        size_t target_len;
        const char *target_str = ValkeyModule_StringPtrLen(target, &target_len);
        snprintf(target_server_id, sizeof(target_server_id), "%.*s", (int)target_len, target_str);
    } else {
        /* Use local server ID */
        snprintf(target_server_id, sizeof(target_server_id), "%s", server_id);
    }
    
    /* Handle backup_id: use passed value if initialized, otherwise calculate using current time */
    char backup_id_str[256];
    if (backup_id != NULL && *backup_id != NULL) {
        /* backup_id was passed in - use it instead of creating a new one */
        size_t backup_id_len;
        const char *existing_backup_id = ValkeyModule_StringPtrLen(*backup_id, &backup_id_len);
        snprintf(backup_id_str, sizeof(backup_id_str), "%.*s", (int)backup_id_len, existing_backup_id);
        ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: using passed backup_id=%s", backup_id_str);
    } else {
        /* backup_id not initialized - check if we should reuse last backup_id */
        if (reuse_last_backup_id && last_backup_id[0] != '\0') {
            /* Reuse the backup_id from storage dump */
            strncpy(backup_id_str, last_backup_id, sizeof(backup_id_str) - 1);
            backup_id_str[sizeof(backup_id_str) - 1] = '\0';
            reuse_last_backup_id = 0;  /* Clear flag after reuse */
            ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: reusing backup_id=%s", backup_id_str);
        } else {
            /* Generate new backup_id using timestamp and sequence counter */
            struct timeval tv;
            gettimeofday(&tv, NULL);
            
            /* Check if we're still in the same second as last dump */
            if (tv.tv_sec == last_backup_second) {
                backup_sequence_counter++;
            } else {
                last_backup_second = tv.tv_sec;
                backup_sequence_counter = 0;
            }
            
            /* Format: backup_-1_<timestamp>_<sequence> */
            snprintf(backup_id_str, sizeof(backup_id_str), "backup_%d_%lld_%d",
                    -1, (long long)tv.tv_sec, backup_sequence_counter);
            
            /* Store for potential reuse by paired dump (storage/filter) */
            strncpy(last_backup_id, backup_id_str, sizeof(last_backup_id) - 1);
            last_backup_id[sizeof(last_backup_id) - 1] = '\0';
            reuse_last_backup_id = 1;  /* Set flag for next dump to reuse */
            
            ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: calculated new backup_id=%s", backup_id_str);
        }
    }
    
    /* Create directory for this server instance */
    char server_dir[1024];
    snprintf(server_dir, sizeof(server_dir), "%s", get_external_data_path(module_ctx, target_server_id));
    mkdir("/tmp/external_data", 0755);
    mkdir(server_dir, 0755);
    ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: creating backup in directory=%s", server_dir);
    
    /* Handle filter data dump */
    if (dbid >= 0) {
        if (ValkeyModule_DictSize(filter_mem_pool[dbid]) == 0) {
            return EXTERNAL_SUCCESS;  // Skip empty database
        }

        /* Dump specific database only */
        ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: dumping specific database %d", dbid);
        
        if (dbid >= MAX_DB || filter_mem_pool[dbid] == NULL) {
            ValkeyModule_Log(module_ctx, "warning", "filterDumpFunction: invalid dbid=%d or no filter data", dbid);
            return EXTERNAL_ERROR;
        }
        
        /* Create database-specific directory */
        char db_dir[1024];
        snprintf(db_dir, sizeof(db_dir), "%s/db%d", server_dir, dbid);
        mkdir(db_dir, 0755);
        
        /* Dump filter data to database-specific file */
        char filter_filename[1536];
        snprintf(filter_filename, sizeof(filter_filename), "%s/%s_filter_%s.dat", db_dir, module_name, backup_id_str);
        
        FILE *filter_file = fopen(filter_filename, "w");
        if (!filter_file) {
            ValkeyModule_Log(module_ctx, "warning", "Failed to create filter dump file: %s", filter_filename);
            return EXTERNAL_ERROR;
        }
        
        /* Iterate through the specific database for filter data */
        ValkeyModuleDict *dict_to_dump = snapshot_dict ? snapshot_dict : filter_mem_pool[dbid];
        ValkeyModuleDictIter *filter_iter = ValkeyModule_DictIteratorStartC(dict_to_dump, "^", NULL, 0);
        char *key;
        size_t key_len;
        while ((key = ValkeyModule_DictNextC(filter_iter, &key_len, NULL)) != NULL) {
            ValkeyModuleString *key_ptr = ValkeyModule_CreateString(NULL, key, key_len);
            const char *key_str = ValkeyModule_StringPtrLen(key_ptr, &key_len);
            ValkeyModule_Log(module_ctx, "debug", "filterDumpFunction - writing filter key=%.*s",
                (int) key_len, key_str);
            /* Format: db_id key_len key_data */
            fprintf(filter_file, "%d %zu %.*s\n", dbid, key_len, (int)key_len, key_str);
            ValkeyModule_FreeString(NULL, key_ptr);
        }
        ValkeyModule_DictIteratorStop(filter_iter);
        fclose(filter_file);
        
        ValkeyModule_Log(module_ctx, "notice", "Dumped filter data for database %d to %s",
                         dbid, filter_filename);
        
    } else if (dbid == -1) {
        /* Dump all databases */
        ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: dumping all databases");
        
        /* Iterate through all databases */
        for (int db = 0; db < MAX_DB; db++) {
            if (filterDumpFunction(module_ctx,
                                   filter_ctx,
                                   db,
                                   slot,
                                   timestamp,
                                   target,
                                   backup_id) == EXTERNAL_ERROR) {
                ValkeyModule_Log(module_ctx, "warning", "Failed to dump filter data for database %d", db);
                return EXTERNAL_ERROR;
            }
        }

        // Set backup_id even if all databases were empty
        if (backup_id != NULL && *backup_id == NULL) {
            *backup_id = ValkeyModule_CreateString(NULL, backup_id_str, strlen(backup_id_str));
        }

        ValkeyModule_Log(module_ctx, "notice", "Dumped filter data for all databases");
        return EXTERNAL_SUCCESS;
    } else {
        ValkeyModule_Log(module_ctx, "warning", "filterDumpFunction: invalid dbid=%d", dbid);
        return EXTERNAL_ERROR;
    }
    
    if (backup_id != NULL) {
        *backup_id = ValkeyModule_CreateString(NULL, backup_id_str, strlen(backup_id_str));
    }
    
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

    /* Check if we have a snapshot available in the global pool */
    ValkeyModuleDict *snapshot_dict = NULL;
    if (dbid >= 0 && dbid < MAX_DB && storage_snapshot_pool[dbid]) {
        snapshot_dict = storage_snapshot_pool[dbid];
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: using snapshot for dump of db%d", dbid);
    }
    
    /* Extract server ID from target parameter if provided, otherwise use local server ID */
    char target_server_id[1024];
    if (target != NULL) {
        size_t target_len;
        // Add logging before StringPtrLen call
        ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageDumpFunction - about to call StringPtrLen on target=%p", (void*)target);
        const char *target_str = ValkeyModule_StringPtrLen(target, &target_len);
        snprintf(target_server_id, sizeof(target_server_id), "%.*s", (int)target_len, target_str);
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: using target server_id=%s", server_id);
    } else {
        ValkeyModule_Log(module_ctx, "warning", "storageDumpFunction: target is NULL, using local server_id=%s", server_id);
        /* Use local server ID */
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: using local server_id=%s", server_id);
        snprintf(target_server_id, sizeof(target_server_id), "%s", server_id);
    }
    
    /* Handle backup_id: use passed value if initialized, otherwise calculate using current time */
    char backup_id_str[256];
    if (backup_id != NULL && *backup_id != NULL) {
        /* backup_id was passed in - use it instead of creating a new one */
        size_t backup_id_len;
        const char *existing_backup_id = ValkeyModule_StringPtrLen(*backup_id, &backup_id_len);
        snprintf(backup_id_str, sizeof(backup_id_str), "%.*s", (int)backup_id_len, existing_backup_id);
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: using passed backup_id=%s", backup_id_str);
    } else {
        /* backup_id not initialized - check if we should reuse last backup_id */
        if (reuse_last_backup_id && last_backup_id[0] != '\0') {
            /* Reuse the backup_id from filter dump */
            strncpy(backup_id_str, last_backup_id, sizeof(backup_id_str) - 1);
            backup_id_str[sizeof(backup_id_str) - 1] = '\0';
            reuse_last_backup_id = 0;  /* Clear flag after reuse */
            ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: reusing backup_id=%s", backup_id_str);
        } else {
            /* Generate new backup_id using timestamp and sequence counter */
            struct timeval tv;
            gettimeofday(&tv, NULL);
            
            /* Check if we're still in the same second as last dump */
            if (tv.tv_sec == last_backup_second) {
                backup_sequence_counter++;
            } else {
                last_backup_second = tv.tv_sec;
                backup_sequence_counter = 0;
            }
            
            /* Format: backup_-1_<timestamp>_<sequence> */
            snprintf(backup_id_str, sizeof(backup_id_str), "backup_%d_%lld_%d",
                    -1, (long long)tv.tv_sec, backup_sequence_counter);
            
            /* Store for potential reuse by paired dump (storage/filter) */
            strncpy(last_backup_id, backup_id_str, sizeof(last_backup_id) - 1);
            last_backup_id[sizeof(last_backup_id) - 1] = '\0';
            reuse_last_backup_id = 1;  /* Set flag for next dump to reuse */
            
            ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: calculated new backup_id=%s", backup_id_str);
        }
    }
    
    /* Create directory for this server instance */
    char server_dir[1024];
    snprintf(server_dir, sizeof(server_dir), "%s", get_external_data_path(module_ctx, target_server_id));
    mkdir("/tmp/external_data", 0755);
    mkdir(server_dir, 0755);
    ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: creating backup in directory=%s", server_dir);
    
    /* Handle storage data dump */
    if (dbid >= 0) {
        if (ValkeyModule_DictSize(storage_mem_pool[dbid]) == 0) {
            return EXTERNAL_SUCCESS;  // Skip empty database
        }

        /* Dump specific database only */
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: dumping specific database %d", dbid);
        
        if (dbid >= MAX_DB || storage_mem_pool[dbid] == NULL) {
            ValkeyModule_Log(module_ctx, "warning", "storageDumpFunction: invalid dbid=%d or no storage data", dbid);
            return EXTERNAL_ERROR;
        }
        
        /* Create database-specific directory */
        char db_dir[1024];
        snprintf(db_dir, sizeof(db_dir), "%s/db%d", server_dir, dbid);
        mkdir(db_dir, 0755);
        
        /* Dump storage data to database-specific file */
        char storage_filename[1536];
        snprintf(storage_filename, sizeof(storage_filename), "%s/%s_storage_%s.dat", db_dir, module_name, backup_id_str);
        
        FILE *storage_file = fopen(storage_filename, "w");
        if (!storage_file) {
            ValkeyModule_Log(module_ctx, "warning", "Failed to create storage dump file: %s", storage_filename);
            return EXTERNAL_ERROR;
        }
        
        /* Iterate through the specific database */
        ValkeyModuleDict *dict_to_dump = snapshot_dict ? snapshot_dict : storage_mem_pool[dbid];
        ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(dict_to_dump, "^", NULL, 0);
        char *key;
        size_t key_len, val_len;
        ValkeyModuleString *value;
        while ((key = ValkeyModule_DictNextC(iter, &key_len, (void **)&value)) != NULL) {
            if (value == NULL) {
                continue;
            }

            ValkeyModule_Log(module_ctx, "debug", "DEBUG: tyz");
            ValkeyModuleString *key_ptr = ValkeyModule_CreateString(NULL, key, key_len);
            const char *key_str = ValkeyModule_StringPtrLen(key_ptr, &key_len);

            ValkeyModule_Log(module_ctx, "debug", "DEBUG: pertyz");
            const char *val_str = ValkeyModule_StringPtrLen(value, &val_len);

            ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageDumpFunction - about to write on key=%.*s, value=%.*s",
                (int) key_len, key_str, (int) val_len, val_str);
            /* Format: db_id key_len key_data val_len val_data */
            fprintf(storage_file, "%d %zu %.*s %zu %.*s\n",
                    dbid, key_len, (int)key_len, key_str, val_len, (int)val_len, val_str);
            ValkeyModule_FreeString(NULL, key_ptr);
        }
        ValkeyModule_DictIteratorStop(iter);
        fclose(storage_file);
        
        ValkeyModule_Log(module_ctx, "notice", "Dumped storage data for database %d to %s",
                         dbid, storage_filename);
        
    } else if (dbid == -1) {
        /* Dump all databases */
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: dumping all databases");
        
        /* Iterate through all databases */
        for (int db = 0; db < MAX_DB; db++) {
            if (storageDumpFunction(module_ctx,
                                    storage_ctx,
                                    db,
                                    slot,
                                    timestamp,
                                    target,
                                    backup_id) == EXTERNAL_ERROR) {
                ValkeyModule_Log(module_ctx, "warning", "Failed to dump storage data for database %d", db);
                return EXTERNAL_ERROR;
            }
        }

        // Set backup_id even if all databases were empty
        if (backup_id != NULL && *backup_id == NULL) {
            *backup_id = ValkeyModule_CreateString(NULL, backup_id_str, strlen(backup_id_str));
        }

        ValkeyModule_Log(module_ctx, "notice", "Dumped storage data for all databases");
        return EXTERNAL_SUCCESS;
    } else {
        ValkeyModule_Log(module_ctx, "warning", "storageDumpFunction: invalid dbid=%d", dbid);
        return EXTERNAL_ERROR;
    }
    
    if (backup_id != NULL) {
        *backup_id = ValkeyModule_CreateString(NULL, backup_id_str, strlen(backup_id_str));
    }
    
    return EXTERNAL_SUCCESS;
}

static int filterLoadFunction(ValkeyModuleCtx *module_ctx,
                              ValkeyModuleExternalFilterCtx *filter_ctx,
                              int dbid,
                              ValkeyModuleString *backup_id,
                              ValkeyModuleString *source) {
    VALKEYMODULE_NOT_USED(filter_ctx);
    ValkeyModule_AutoMemory(module_ctx);
   
    if (dbid == -1) {
        int success = 0;
        for (int db = 0; db < MAX_DB; db++) {
            int res = filterLoadFunction(module_ctx,
                                         filter_ctx,
                                         db,
                                         backup_id,
                                         source);
            if (res == EXTERNAL_ERROR) return EXTERNAL_ERROR;
            if (res == EXTERNAL_SUCCESS) success = 1;
        }
        return success ? EXTERNAL_SUCCESS : EXTERNAL_ERROR;
    }

    /* Extract source server ID (host:port) */
    char source_server_id[256];
    if (source != NULL) {
        size_t source_len;
        const char *source_str = ValkeyModule_StringPtrLen(source, &source_len);
        
        /* Use the full source string as server ID (includes port if present) */
        snprintf(source_server_id, sizeof(source_server_id), "%.*s", (int)source_len, source_str);
        
        ValkeyModule_Log(module_ctx, "notice", "Loading filter from source: %s", source_server_id);
    } else {
        /* Use local server ID */
        snprintf(source_server_id, sizeof(source_server_id), "%s", server_id);
        ValkeyModule_Log(module_ctx, "notice", "Loading filter from local instance: %s", source_server_id);
    }
    
    char backup_id_buf[256];
    size_t backup_id_len;
    const char *backup_id_str;
    
    if (backup_id == NULL) {
        /* Find the most recent backup file using glob pattern */
        ValkeyModule_Log(module_ctx, "notice", "No backup_id provided, using most recent backup");
        
        /* Use server-specific directory */
        char source_dir[1024];
        snprintf(source_dir, sizeof(source_dir), "%s", get_external_data_path(module_ctx, source_server_id));
        
        /* Use a simple heuristic: check the last 15 seconds for backup files */
        time_t current_time = time(NULL);
        int found_backup = 0;
        
        for (int i = 0; i < 15 && !found_backup; i++) {
            time_t t = current_time - i;
            char prefix[256];
            snprintf(prefix, sizeof(prefix), "backup_%d_%lld_", -1, (long long)t);
            
            ValkeyModule_Log(module_ctx, "debug", "filterLoad: Searching for backups with prefix=%s in dir=%s", prefix, source_dir);
            
            DIR *dir = opendir(source_dir);
            if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir)) != NULL) {
                    ValkeyModule_Log(module_ctx, "debug", "filterLoad: Found entry=%s", entry->d_name);
                    if (strncmp(entry->d_name, "db", 2) == 0) {
                        int entry_dbid = strtol(entry->d_name + 2, NULL, 10);
                        ValkeyModule_Log(module_ctx, "debug", "filterLoad: entry_dbid=%d, target_dbid=%d", entry_dbid, dbid);
                        if (entry_dbid == dbid) {
                            /* Found a database directory, check for filter files matching prefix */
                            char db_dir[1024];
                            snprintf(db_dir, sizeof(db_dir), "%s/%s", source_dir, entry->d_name);
                        
                            ValkeyModule_Log(module_ctx, "debug", "filterLoad: Opening db_dir=%s for dbid=%d", db_dir, dbid);
                            DIR *db_subdir = opendir(db_dir);
                            if (db_subdir) {
                                struct dirent *file_entry;
                                int max_sequence = -1;
                                char best_backup_id[256] = {0};
                                
                                while ((file_entry = readdir(db_subdir)) != NULL) {
                                    /* Check if filename matches pattern: module_filter_backup_-1_<timestamp>_<seq>.dat */
                                    char expected_prefix[512];
                                    snprintf(expected_prefix, sizeof(expected_prefix), "%s_filter_%s", module_name, prefix);
                                    ValkeyModule_Log(module_ctx, "debug", "filterLoad: Checking file=%s against prefix=%s",
                                                   file_entry->d_name, expected_prefix);
                                    if (strncmp(file_entry->d_name, expected_prefix, strlen(expected_prefix)) == 0 &&
                                        strstr(file_entry->d_name, ".dat") != NULL) {
                                        /* Extract the full backup_id from filename */
                                        const char *backup_start = file_entry->d_name + strlen(module_name) + 8; /* +8 for "_filter_" */
                                        const char *dat_pos = strstr(backup_start, ".dat");
                                        if (dat_pos) {
                                            size_t backup_len = dat_pos - backup_start;
                                            char temp_backup_id[256];
                                            snprintf(temp_backup_id, sizeof(temp_backup_id), "%.*s", (int)backup_len, backup_start);
                                            
                                            /* Extract sequence number from backup_id: backup_-1_<timestamp>_<seq> */
                                            const char *last_underscore = strrchr(temp_backup_id, '_');
                                            if (last_underscore) {
                                                int seq = atoi(last_underscore + 1);
                                                ValkeyModule_Log(module_ctx, "debug", "filterLoad: Found backup %s with sequence=%d", temp_backup_id, seq);
                                                if (seq > max_sequence) {
                                                    max_sequence = seq;
                                                    strncpy(best_backup_id, temp_backup_id, sizeof(best_backup_id) - 1);
                                                    best_backup_id[sizeof(best_backup_id) - 1] = '\0';
                                                }
                                            }
                                        }
                                    }
                                }
                                closedir(db_subdir);
                                
                                if (max_sequence >= 0) {
                                    found_backup = 1;
                                    strncpy(backup_id_buf, best_backup_id, sizeof(backup_id_buf) - 1);
                                    backup_id_buf[sizeof(backup_id_buf) - 1] = '\0';
                                    backup_id_str = backup_id_buf;
                                    backup_id_len = strlen(backup_id_buf);
                                    ValkeyModule_Log(module_ctx, "notice", "Using most recent backup with highest sequence (new format): %s", backup_id_str);
                                }
                                
                                if (found_backup) break;
                            } else {
                                ValkeyModule_Log(module_ctx, "warning", "filterLoad: Failed to open db_dir=%s", db_dir);
                            }
                    }
                    }
                }
                closedir(dir);
            } else {
                ValkeyModule_Log(module_ctx, "warning", "filterLoad: Failed to open source_dir=%s", source_dir);
            }
            
            if (found_backup) break;
        }
        
        if (!found_backup) {
            ValkeyModule_Log(module_ctx, "warning", "No filter backup files found in last 10 seconds in %s", source_dir);
            return EXTERNAL_NOT_FOUND;
        }
    } else {
        backup_id_str = ValkeyModule_StringPtrLen(backup_id, &backup_id_len);
    }
    
    /* Load filter data from disk using source server directory */
    char source_dir[1024];
    snprintf(source_dir, sizeof(source_dir), "%s/db%d", get_external_data_path(module_ctx, source_server_id), dbid);
    
    char filter_filename[1536];
    snprintf(filter_filename, sizeof(filter_filename), "%s/%s_filter_%.*s.dat",
             source_dir, module_name, (int)backup_id_len, backup_id_str);
    
    ValkeyModule_Log(module_ctx, "notice", "Trying to load filter from: %s", filter_filename);
    FILE *filter_file = fopen(filter_filename, "r");
    
    /* If exact match fails, try prefix matching (for new backup_id format with microseconds) */
    if (!filter_file) {
        ValkeyModule_Log(module_ctx, "notice", "Exact match failed, trying prefix matching for: %.*s",
                        (int)backup_id_len, backup_id_str);
        
        DIR *dir = opendir(source_dir);
        if (dir) {
            struct dirent *entry;
            char best_match[1536] = {0};
            long long best_microseconds = -1;
            
            /* Create prefix pattern: backup_-1_<timestamp>_ (without microseconds) */
            char prefix[256];
            /* Find the last underscore in backup_id to extract timestamp only */
            const char *last_underscore = strrchr(backup_id_str, '_');
            size_t timestamp_len = backup_id_len;
            if (last_underscore != NULL) {
                /* Calculate length up to (and including) the timestamp part */
                timestamp_len = last_underscore - backup_id_str;
            }
            /* Create prefix with trailing underscore to match pattern */
            snprintf(prefix, sizeof(prefix), "%s_filter_%.*s_", module_name, (int)timestamp_len, backup_id_str);
            
            while ((entry = readdir(dir)) != NULL) {
                /* Check if filename starts with our prefix */
                if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0 &&
                    strstr(entry->d_name, ".dat") != NULL) {
                    
                    /* Extract microseconds from filename if present */
                    const char *underscore = strrchr(entry->d_name, '_');
                    if (underscore && underscore > entry->d_name + strlen(prefix)) {
                        long long microseconds = atoll(underscore + 1);
                        if (microseconds > best_microseconds) {
                            best_microseconds = microseconds;
                            snprintf(best_match, sizeof(best_match), "%s/%s", source_dir, entry->d_name);
                            ValkeyModule_Log(module_ctx, "debug", "Found matching file: %s (microseconds=%lld)",
                                           entry->d_name, microseconds);
                        }
                    } else {
                        /* File without microseconds - use it if no better match found */
                        if (best_microseconds < 0) {
                            snprintf(best_match, sizeof(best_match), "%s/%s", source_dir, entry->d_name);
                            ValkeyModule_Log(module_ctx, "debug", "Found matching file: %s (no microseconds)",
                                           entry->d_name);
                        }
                    }
                }
            }
            closedir(dir);
            
            if (best_match[0] != '\0') {
                ValkeyModule_Log(module_ctx, "notice", "Using prefix-matched file: %s", best_match);
                strncpy(filter_filename, best_match, sizeof(filter_filename) - 1);
                filter_filename[sizeof(filter_filename) - 1] = '\0';
                filter_file = fopen(filter_filename, "r");
            }
        }
    }
    
    if (!filter_file) {
        ValkeyModule_Log(module_ctx, "warning", "Failed to open filter dump file: %s", filter_filename);
        return EXTERNAL_NOT_FOUND;
    }
    
    /* Clear existing filter data */
    filterFlushFunction(module_ctx, filter_ctx, dbid);
    
    /* Mark that we're loading - this will cause incoming commands to be queued */
    is_loading[dbid] = 1;
    
    /* Read and restore filter data */
    int db;
    size_t key_len;
    char line[8192];
    int loaded_count = 0;
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
        loaded_count++;
        ValkeyModule_Log(module_ctx, "debug", "Loaded filter key: db=%d, key_len=%zu", db, key_len);
    }
    fclose(filter_file);
    
    /* Done loading - allow incoming commands to be processed */
    is_loading[dbid] = 0;
    
    ValkeyModule_Log(module_ctx, "notice", "Loaded %d filter entries from %s", loaded_count, filter_filename);
    
    /* Drain queued commands that arrived during loading */
    drainCommandQueue(module_ctx, dbid);
    
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
   
    if (dbid == -1) {
        int success = 0;
        for (int db = 0; db < MAX_DB; db++) {
            int res = storageLoadFunction(module_ctx,
                                      storage_ctx,
                                      db,
                                      backup_id,
                                      source);
            if (res == EXTERNAL_ERROR) return EXTERNAL_ERROR;
            if (res == EXTERNAL_SUCCESS) success = 1;
        }
        return success ? EXTERNAL_SUCCESS : EXTERNAL_ERROR;
    }

    /* Extract source server ID (host:port) */
    char source_server_id[256];
    if (source != NULL) {
        size_t source_len;
        // Add logging before StringPtrLen call
        ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageLoadFunction - about to call StringPtrLen on source=%p", (void*)source);
        const char *source_str = ValkeyModule_StringPtrLen(source, &source_len);
        
        /* Use the full source string as server ID (includes port if present) */
        snprintf(source_server_id, sizeof(source_server_id), "%.*s", (int)source_len, source_str);
        
        ValkeyModule_Log(module_ctx, "notice", "Loading storage from source: %s", source_server_id);
    } else {
        ValkeyModule_Log(module_ctx, "warning", "storageLoadFunction: source is NULL, using local server_id=%s", server_id);
        /* Use local server ID */
        snprintf(source_server_id, sizeof(source_server_id), "%s", server_id);
        ValkeyModule_Log(module_ctx, "notice", "Loading storage from local instance: %s", source_server_id);
    }
    
    char backup_id_buf[256];
    size_t backup_id_len;
    const char *backup_id_str;
    
    if (backup_id == NULL) {
        /* Find the most recent backup file using glob pattern */
        ValkeyModule_Log(module_ctx, "notice", "No backup_id provided, using most recent backup");
        
        /* Use server-specific directory */
        char source_dir[1024];
        snprintf(source_dir, sizeof(source_dir), "%s", get_external_data_path(module_ctx, source_server_id));
        
        /* Use a simple heuristic: check the last 15 seconds for backup files */
        time_t current_time = time(NULL);
        int found_backup = 0;
        
        for (int i = 0; i < 15 && !found_backup; i++) {
            time_t t = current_time - i;
            char prefix[256];
            snprintf(prefix, sizeof(prefix), "backup_%d_%lld_", -1, (long long)t);
            
            DIR *dir = opendir(source_dir);
            if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir)) != NULL) {
                    if (strncmp(entry->d_name, "db", 2) == 0 && strtol(entry->d_name + 2, NULL, 10) == dbid) {
                        /* Found a database directory, check for storage files matching prefix */
                        char db_dir[1024];
                        snprintf(db_dir, sizeof(db_dir), "%s/%s", source_dir, entry->d_name);
                        
                        DIR *db_subdir = opendir(db_dir);
                        if (db_subdir) {
                            struct dirent *file_entry;
                            int max_sequence = -1;
                            char best_backup_id[256] = {0};
                            
                            while ((file_entry = readdir(db_subdir)) != NULL) {
                                /* Check if filename matches pattern: module_storage_backup_-1_<timestamp>_<seq>.dat */
                                char expected_prefix[512];
                                snprintf(expected_prefix, sizeof(expected_prefix), "%s_storage_%s", module_name, prefix);
                                if (strncmp(file_entry->d_name, expected_prefix, strlen(expected_prefix)) == 0 &&
                                    strstr(file_entry->d_name, ".dat") != NULL) {
                                    /* Extract the full backup_id from filename */
                                    const char *backup_start = file_entry->d_name + strlen(module_name) + 9; /* +9 for "_storage_" */
                                    const char *dat_pos = strstr(backup_start, ".dat");
                                    if (dat_pos) {
                                        size_t backup_len = dat_pos - backup_start;
                                        char temp_backup_id[256];
                                        snprintf(temp_backup_id, sizeof(temp_backup_id), "%.*s", (int)backup_len, backup_start);
                                        
                                        /* Extract sequence number from backup_id: backup_-1_<timestamp>_<seq> */
                                        const char *last_underscore = strrchr(temp_backup_id, '_');
                                        if (last_underscore) {
                                            int seq = atoi(last_underscore + 1);
                                            ValkeyModule_Log(module_ctx, "debug", "storageLoad: Found backup %s with sequence=%d", temp_backup_id, seq);
                                            if (seq > max_sequence) {
                                                max_sequence = seq;
                                                strncpy(best_backup_id, temp_backup_id, sizeof(best_backup_id) - 1);
                                                best_backup_id[sizeof(best_backup_id) - 1] = '\0';
                                            }
                                        }
                                    }
                                }
                            }
                            closedir(db_subdir);
                            
                            if (max_sequence >= 0) {
                                found_backup = 1;
                                strncpy(backup_id_buf, best_backup_id, sizeof(backup_id_buf) - 1);
                                backup_id_buf[sizeof(backup_id_buf) - 1] = '\0';
                                backup_id_str = backup_id_buf;
                                backup_id_len = strlen(backup_id_buf);
                                ValkeyModule_Log(module_ctx, "notice", "Using most recent backup with highest sequence (new format): %s", backup_id_str);
                            }
                            
                            if (found_backup) break;
                        }
                    }
                }
                closedir(dir);
            }
            
            if (found_backup) break;
        }
        
        if (!found_backup) {
            ValkeyModule_Log(module_ctx, "warning", "No backup files found in last 10 seconds in %s", source_dir);
            /* List all files in the directory for debugging */
            DIR *dir = opendir(source_dir);
            if (dir) {
                struct dirent *entry;
                ValkeyModule_Log(module_ctx, "warning", "Files in directory %s:", source_dir);
                while ((entry = readdir(dir)) != NULL) {
                    if (strstr(entry->d_name, ".dat") != NULL) {
                        ValkeyModule_Log(module_ctx, "warning", "  %s", entry->d_name);
                    }
                }
                closedir(dir);
            }
            return EXTERNAL_NOT_FOUND;
        }
    } else {
        // Add logging before StringPtrLen call
        ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageLoadFunction - about to call StringPtrLen on backup_id=%p", (void*)backup_id);
        backup_id_str = ValkeyModule_StringPtrLen(backup_id, &backup_id_len);
    }
    
    /* Load storage data from disk using source server directory */
    char source_dir[1024];
    snprintf(source_dir, sizeof(source_dir), "%s/db%d", get_external_data_path(module_ctx, source_server_id), dbid);
    
    char storage_filename[1536];
    snprintf(storage_filename, sizeof(storage_filename), "%s/%s_storage_%.*s.dat",
             source_dir, module_name, (int)backup_id_len, backup_id_str);
    
    ValkeyModule_Log(module_ctx, "notice", "Trying to load storage from: %s", storage_filename);
    FILE *storage_file = fopen(storage_filename, "r");
    
    /* If exact match fails, try prefix matching (for new backup_id format with microseconds) */
    if (!storage_file) {
        ValkeyModule_Log(module_ctx, "notice", "Exact match failed, trying prefix matching for: %.*s",
                        (int)backup_id_len, backup_id_str);
        
        DIR *dir = opendir(source_dir);
        if (dir) {
            struct dirent *entry;
            char best_match[1536] = {0};
            long long best_microseconds = -1;
            
            /* Create prefix pattern: backup_-1_<timestamp>_ (without microseconds) */
            char prefix[256];
            /* Find the last underscore in backup_id to extract timestamp only */
            const char *last_underscore = strrchr(backup_id_str, '_');
            size_t timestamp_len = backup_id_len;
            if (last_underscore != NULL) {
                /* Calculate length up to (and including) the timestamp part */
                timestamp_len = last_underscore - backup_id_str;
            }
            /* Create prefix with trailing underscore to match pattern */
            snprintf(prefix, sizeof(prefix), "%s_storage_%.*s_", module_name, (int)timestamp_len, backup_id_str);
            
            while ((entry = readdir(dir)) != NULL) {
                /* Check if filename starts with our prefix */
                if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0 &&
                    strstr(entry->d_name, ".dat") != NULL) {
                    
                    /* Extract microseconds from filename if present */
                    const char *underscore = strrchr(entry->d_name, '_');
                    if (underscore && underscore > entry->d_name + strlen(prefix)) {
                        long long microseconds = atoll(underscore + 1);
                        if (microseconds > best_microseconds) {
                            best_microseconds = microseconds;
                            snprintf(best_match, sizeof(best_match), "%s/%s", source_dir, entry->d_name);
                            ValkeyModule_Log(module_ctx, "debug", "Found matching file: %s (microseconds=%lld)",
                                           entry->d_name, microseconds);
                        }
                    } else {
                        /* File without microseconds - use it if no better match found */
                        if (best_microseconds < 0) {
                            snprintf(best_match, sizeof(best_match), "%s/%s", source_dir, entry->d_name);
                            ValkeyModule_Log(module_ctx, "debug", "Found matching file: %s (no microseconds)",
                                           entry->d_name);
                        }
                    }
                }
            }
            closedir(dir);
            
            if (best_match[0] != '\0') {
                ValkeyModule_Log(module_ctx, "notice", "Using prefix-matched file: %s", best_match);
                strncpy(storage_filename, best_match, sizeof(storage_filename) - 1);
                storage_filename[sizeof(storage_filename) - 1] = '\0';
                storage_file = fopen(storage_filename, "r");
            }
        }
    }
    
    if (!storage_file) {
        ValkeyModule_Log(module_ctx, "warning", "Failed to open storage dump file: %s", storage_filename);
        return EXTERNAL_NOT_FOUND;
    }

    /* Clear existing storage data */
    storageFlushFunction(module_ctx, storage_ctx, dbid);
    
    /* Mark that we're loading - this will cause incoming commands to be queued */
    is_loading[dbid] = 1;
    
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
    
    /* Done loading - allow incoming commands to be processed */
    is_loading[dbid] = 0;
    ValkeyModule_Log(module_ctx, "notice", "Loaded %d storage entries from %s", loaded_count, storage_filename);
    
    /* Drain queued commands that arrived during loading */
    drainCommandQueue(module_ctx, dbid);
    
    return EXTERNAL_SUCCESS;
}

/* Helper function to find databases from backup files - reused by get_state */
static int findDatabasesFromBackup(ValkeyModuleCtx *module_ctx, ValkeyModuleString *source, int **db_numbers, size_t *num_dbs) {
    /* Determine which directory to look in based on source parameter */
    char server_dir[1024];
    if (source != NULL) {
        /* We're on a replica - use the primary's directory */
        size_t source_len;
        const char *source_str = ValkeyModule_StringPtrLen(source, &source_len);
        snprintf(server_dir, sizeof(server_dir), "%s",
                 get_external_data_path(module_ctx, source_str));
        ValkeyModule_Log(module_ctx, "notice", "Looking for backups in primary's directory: %s", server_dir);
    } else {
        /* We're on primary - use local directory */
        snprintf(server_dir, sizeof(server_dir), "%s", get_external_data_path(module_ctx, server_id));
        ValkeyModule_Log(module_ctx, "notice", "Looking for backups in local directory: %s", server_dir);
    }
    
    /* Scan db subdirectories to find which databases have backups */
    int db_set[MAX_DB] = {0};
    int db_count = 0;
    
    DIR *dir = opendir(server_dir);
    if (!dir) {
        ValkeyModule_Log(module_ctx, "warning", "Cannot open directory: %s", server_dir);
        return EXTERNAL_ERROR;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Check if this is a db directory (db0, db1, etc.) */
        if (strncmp(entry->d_name, "db", 2) == 0) {
            int db = atoi(entry->d_name + 2);
            if (db >= 0 && db < MAX_DB) {
                /* Check if this database has any backup files */
                char db_dir[1024];
                snprintf(db_dir, sizeof(db_dir), "%s/%s", server_dir, entry->d_name);
                
                DIR *db_subdir = opendir(db_dir);
                if (db_subdir) {
                    struct dirent *file_entry;
                    while ((file_entry = readdir(db_subdir)) != NULL) {
                        /* Look for storage or filter backup files */
                        if ((strstr(file_entry->d_name, "_storage_") != NULL ||
                             strstr(file_entry->d_name, "_filter_") != NULL) &&
                            strstr(file_entry->d_name, ".dat") != NULL) {
                            /* Found a backup for this database */
                            if (!db_set[db]) {
                                db_set[db] = 1;
                                db_count++;
                                ValkeyModule_Log(module_ctx, "notice", "Found backup for db%d: %s", db, file_entry->d_name);
                            }
                            break;
                        }
                    }
                    closedir(db_subdir);
                }
            }
        }
    }
    closedir(dir);
    
    if (db_count == 0) {
        ValkeyModule_Log(module_ctx, "notice", "No database backups found in %s, returning empty list", server_dir);
        /* Return success with empty database list instead of error */
        *db_numbers = NULL;
        *num_dbs = 0;
        return EXTERNAL_SUCCESS;
    }
    
    /* Allocate and fill the db_numbers array */
    *db_numbers = ValkeyModule_Alloc(sizeof(int) * db_count);
    if (*db_numbers == NULL) {
        ValkeyModule_Log(module_ctx, "warning", "Failed to allocate memory for db_numbers");
        return EXTERNAL_ERROR;
    }
    
    int idx = 0;
    for (int db = 0; db < MAX_DB; db++) {
        if (db_set[db]) {
            (*db_numbers)[idx++] = db;
            ValkeyModule_Log(module_ctx, "notice", "Will initialize db%d", db);
        }
    }
    *num_dbs = db_count;
    
    ValkeyModule_Log(module_ctx, "notice", "Found %d database(s) with backups", db_count);
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

static int storageGetStateFunction(ValkeyModuleCtx *module_ctx,
                                  ValkeyModuleExternalStorageCtx *storage_ctx,
                                  ValkeyModuleString *source,
                                  int **db_numbers,
                                  size_t *num_dbs) {
    VALKEYMODULE_NOT_USED(storage_ctx);
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
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    
    if (ValkeyModule_Init(ctx, module_name, 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModule_Log(ctx, "warning", "=== ValkeyModule_Init SUCCESS for module: %s ===", module_name);

    // Get the actual server port and update server_id
    ValkeyModuleServerInfoData *info = ValkeyModule_GetServerInfo(ctx, "server");
    if (info) {
        long long port = ValkeyModule_ServerInfoGetFieldSigned(info, "tcp_port", NULL);
        if (port > 0) {
            snprintf(server_id, sizeof(server_id), "127.0.0.1:%lld", port);
            ValkeyModule_Log(ctx, "notice", "Module %s initialized with server_id=%s", module_name, server_id);
        }
        ValkeyModule_FreeServerInfo(ctx, info);
    }
    ValkeyModule_Log(ctx, "notice", "Module %s initialized with default server_id=%s", module_name, server_id);

    // Register runtime configuration for set_failure_percent
    if (ValkeyModule_RegisterNumericConfig(ctx, "set_failure_percent", 0,
                                           VALKEYMODULE_CONFIG_DEFAULT,
                                           0, 100,
                                           getFailurePercentConfig,
                                           setFailurePercentConfig,
                                           NULL, NULL) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    // Register runtime configuration for dump_every_write
    if (ValkeyModule_RegisterNumericConfig(ctx, "dump_every_write", 0,
                                           VALKEYMODULE_CONFIG_DEFAULT,
                                           0, 1,
                                           getDumpEveryWriteConfig,
                                           setDumpEveryWriteConfig,
                                           NULL, NULL) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    // Initialize storage methods
    ValkeyModuleExternalStorageMethods storage_methods = {
        .version = VALKEYMODULE_EXTERNAL_STORAGE_ABI_VERSION,
        .set = storageSetFunction,
        .get = storageGetFunction,
        .del = storageDelFunction,
        .set_readonly = storageSetReadonlyFunction,
        .drop_readonly = storageDropReadonlyFunction,
        .flush = storageFlushFunction,
        .swap = storageSwapFunction,
        .dump = storageDumpFunction,
        .load = storageLoadFunction,
        .get_state = storageGetStateFunction,
        .iterate = storageIterateFunction,
        .snapshot = storageSnapshotFunction,
        .free_snapshot = storageFreeSnapshotFunction,
    };

    // Initialize filter methods
    ValkeyModuleExternalFilterMethods filter_methods = {
        .version = VALKEYMODULE_EXTERNAL_FILTER_ABI_VERSION,
        .set = filterSetFunction,
        .get = filterGetFunction,
        .del = filterDelFunction,
        .set_readonly = filterSetReadonlyFunction,
        .drop_readonly = filterDropReadonlyFunction,
        .flush = filterFlushFunction,
        .swap = filterSwapFunction,
        .dump = filterDumpFunction,
        .load = filterLoadFunction,
        .get_state = filterGetStateFunction,
        .keys_count = filterKeysCountFunction,
        .snapshot = filterSnapshotFunction,
        .free_snapshot = filterFreeSnapshotFunction,
    };

    // Create memory pools
    for (int i = 0; i < MAX_DB; i++) {
        storage_mem_pool[i] = ValkeyModule_CreateDict(NULL);
        filter_mem_pool[i] = ValkeyModule_CreateDict(NULL);
    }

    // Load configurations
    if (ValkeyModule_LoadConfigs(ctx) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    // Register both storage and filter
    ValkeyModule_Log(ctx, "warning", "=== About to call ValkeyModule_RegisterExternalDataModule for module: %s ===", module_name);
    if (ValkeyModule_RegisterExternalDataModule(ctx, module_name, &storage_methods, &filter_methods) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "=== ValkeyModule_RegisterExternalDataModule FAILED for module: %s ===", module_name);
        return VALKEYMODULE_ERR;
    }
    
    ValkeyModule_Log(ctx, "warning", "=== ValkeyModule_RegisterExternalDataModule SUCCESS for module: %s ===", module_name);
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
