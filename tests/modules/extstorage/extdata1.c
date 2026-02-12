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

/* Maximum length for node identifiers (e.g., IP:port or custom IDs) */
#define NODE_ID_MAX_LEN 64

/* Maximum length for backup identifiers (timestamp-based format YYYYMMDD_HHMMSS) */
#define BACKUP_ID_MAX_LEN 256

/* Maximum length for backup options string */
#define OPTIONS_MAX_LEN 256

/* Directory path buffer sizes */
#define SERVER_DIR_MAX_LEN 2048
#define DB_DIR_MAX_LEN 4096
#define FILENAME_MAX_LEN 5120

/* Backup ID v0 format constants */
#define BACKUP_ID_VERSION_0 0
#define BACKUP_ID_MIN_SLOT -1
#define BACKUP_ID_MAX_SLOT 16383

static char node_id[NODE_ID_MAX_LEN] = {0};  /* Unique node identifier */


#define MAX_DB 16
ValkeyModuleDict *storage_mem_pool[MAX_DB];  // Memory pool for storage
ValkeyModuleDict *filter_mem_pool[MAX_DB];  // Memory pool for filter
ValkeyModuleDict *storage_snapshot_pool[MAX_DB]; // Snapshot pool for storage
ValkeyModuleDict *filter_snapshot_pool[MAX_DB]; // Snapshot pool for filter
ValkeyModuleDict *storage_slot_pool[MAX_DB];  // Slot tracking for storage
ValkeyModuleDict *filter_slot_pool[MAX_DB];   // Slot tracking for filter

/* Failure simulation configuration */
static long long set_failure_percent = 0;  // 0 = no failures, >0 = p out of 100 sets fail
static int set_operation_counter = 0;  // Counter for set operations

/* Auto-dump configuration */
static int dump_every_write = 0;  // 0 = no auto-dump, 1 = dump on every write

/* Backup ID coordination for paired dumps (storage/filter) */
static char last_backup_id[BACKUP_ID_MAX_LEN] = {0};
static int reuse_last_backup_id = 0;  /* Flag: 1 = reuse last_backup_id, 0 = generate new */

/* Cluster mode flag */
static int is_cluster_enabled = 0;  /* 0 = standalone, 1 = cluster */

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

/* Backup ID v0 format: v0:<node_id>:<slot>:<timestamp_millis>
 *
 * Format components:
 * - version: 0 (fixed)
 * - node_id: Node identifier string (max 64 chars)
 * - slot: Slot number (-1 for full dump, 0-16383 for slot-specific dump)
 * - timestamp_millis: Timestamp in milliseconds since epoch
 */
static void encodeBackupIdV0(char *backup_id, size_t backup_id_size,
                             const char *node_id, int slot, long long timestamp) {
    snprintf(backup_id, backup_id_size, "v%d:%s:%d:%lld",
             BACKUP_ID_VERSION_0, node_id, slot, timestamp);
}

static int parseBackupIdV0(ValkeyModuleCtx *module_ctx,
                           ValkeyModuleString *backup_id_str,
                           char *node_id, size_t node_id_size,
                           int *slot,
                           long long *timestamp,
                           char *options, size_t options_size) {
    VALKEYMODULE_NOT_USED(module_ctx);
    
    int version;
    char node_id_buf[NODE_ID_MAX_LEN];
    int slot_val;
    long long ts;
    char options_buf[OPTIONS_MAX_LEN] = {0};
    
    /* Get C string from ValkeyModuleString */
    size_t len;
    const char *backup_id_cstr = ValkeyModule_StringPtrLen(backup_id_str, &len);
    
    /* Format width specifiers use NODE_ID_MAX_LEN - 1 (63) and OPTIONS_MAX_LEN - 1 (255)
     * to match buffer sizes minus null terminator */
    /* Try parsing with options first: v0:<node_id>:<slot>:<timestamp>:<options> */
    int n = sscanf(backup_id_cstr, "v%d:%63[^:]:%d:%lld:%255[^\n]",
                   &version, node_id_buf, &slot_val, &ts, options_buf);
    
    if (n < 4) {
        /* Try parsing without options: v0:<node_id>:<slot>:<timestamp> */
        n = sscanf(backup_id_cstr, "v%d:%63[^:]:%d:%lld",
                   &version, node_id_buf, &slot_val, &ts);
        if (n < 4) {
            return EXTERNAL_ERROR;
        }
    }
    
    /* Validate version */
    if (version != BACKUP_ID_VERSION_0) {
        return EXTERNAL_ERROR;
    }
    
    /* Validate slot range */
    if (slot_val < BACKUP_ID_MIN_SLOT || slot_val > BACKUP_ID_MAX_SLOT) {
        return EXTERNAL_ERROR;
    }
    
    /* Copy node_id to output buffer */
    strncpy(node_id, node_id_buf, node_id_size - 1);
    node_id[node_id_size - 1] = '\0';
    
    /* Copy slot */
    if (slot != NULL) {
        *slot = slot_val;
    }
    
    /* Copy timestamp */
    if (timestamp != NULL) {
        *timestamp = ts;
    }
    
    /* Copy options to output buffer if provided */
    if (options && options_size > 0) {
        strncpy(options, options_buf, options_size - 1);
        options[options_size - 1] = '\0';
    }
    
    return EXTERNAL_SUCCESS;
}
/* Helper function to handle backup_id generation/reuse logic
 * Returns 0 on success, non-zero on error
 *
 * Parameters:
 * - module_ctx: Module context for logging
 * - backup_id: Pointer to ValkeyModuleString* for passed backup_id (may be NULL)
 * - timestamp: Timestamp in milliseconds for the backup
 * - target_node_id: Target node identifier
 * - slot: Slot number (-1 for full dump, 0-16383 for slot-specific dump)
 * - function_name: Name of calling function (for logging)
 * - backup_id_str: Output buffer for the backup_id string (size BACKUP_ID_MAX_LEN)
 */
static void generateOrReuseBackupId(ValkeyModuleCtx *module_ctx,
                                     ValkeyModuleString **backup_id,
                                     long long timestamp,
                                     const char *target_node_id,
                                     int slot,
                                     const char *function_name,
                                     char *backup_id_str) {
    if (backup_id != NULL && *backup_id != NULL) {
        /* backup_id was passed in - use it instead of creating a new one */
        size_t backup_id_len;
        const char *existing_backup_id = ValkeyModule_StringPtrLen(*backup_id, &backup_id_len);
        snprintf(backup_id_str, BACKUP_ID_MAX_LEN, "%.*s", (int)backup_id_len, existing_backup_id);
        ValkeyModule_Log(module_ctx, "notice", "%s: using passed backup_id=%s", function_name, backup_id_str);
    } else {
        /* backup_id not initialized - check if we should reuse last backup_id */
        if (reuse_last_backup_id && last_backup_id[0] != '\0') {
            /* Reuse the backup_id from previous dump */
            strncpy(backup_id_str, last_backup_id, BACKUP_ID_MAX_LEN - 1);
            backup_id_str[BACKUP_ID_MAX_LEN - 1] = '\0';
            reuse_last_backup_id = 0;  /* Clear flag after reuse */
            ValkeyModule_Log(module_ctx, "notice", "%s: reusing backup_id=%s", function_name, backup_id_str);
        } else {
            /* Generate new backup_id using new format v0:<node_id>:<slot>:<timestamp> */
            long long actual_timestamp = timestamp;
            
            /* If timestamp is 0 or negative, generate current time in milliseconds */
            if (actual_timestamp <= 0) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                actual_timestamp = (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
            }
            
            encodeBackupIdV0(backup_id_str, BACKUP_ID_MAX_LEN, target_node_id, slot, actual_timestamp);
            
            /* Store for potential reuse by paired dump (storage/filter) */
            strncpy(last_backup_id, backup_id_str, sizeof(last_backup_id) - 1);
            last_backup_id[sizeof(last_backup_id) - 1] = '\0';
            reuse_last_backup_id = 1;  /* Set flag for next dump to reuse */
            
            ValkeyModule_Log(module_ctx, "notice", "%s: calculated new backup_id=%s", function_name, backup_id_str);
        }
    }
}



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
        const char *msg = "set_failure_percent must be between 0 and 100";
        *err = ValkeyModule_CreateString(NULL, msg, strlen(msg));
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
        const char *msg = "dump_every_write must be 0 or 1";
        *err = ValkeyModule_CreateString(NULL, msg, strlen(msg));
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
/* Forward declarations */
static const char *find_node_id_by_address(ValkeyModuleCtx *ctx, const char *ip_port);

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

    ValkeyModule_Log(module_ctx, "debug", "storageSet: about to get slot for cluster mode%s",
                     is_cluster_enabled ? "enabled" : "disabled");
    int slot = is_cluster_enabled ? ValkeyModule_ClusterKeySlot((ValkeyModuleString *)key) : (unsigned int)EXTERNAL_ALL_SLOTS;
    ValkeyModule_Log(module_ctx, "notice", "storageSet: key=%s slot=%d",
                     ValkeyModule_StringPtrLen(key, NULL), slot);
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

    ValkeyModule_Log(module_ctx, "debug", "DEBUG: storageSetFunction - failure percent: %lld", set_failure_percent);
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
    
    /* Track slot information for this key */
    ValkeyModuleString *old_slot_value = ValkeyModule_DictGet(storage_slot_pool[dbid], (ValkeyModuleString *)key, NULL);
    ValkeyModuleString *slot_value = ValkeyModule_CreateStringPrintf(NULL, "%d", slot);
    ValkeyModule_Log(module_ctx, "warning", "STORING slot for key=%s: slot=%d (string='%s')",
                     ValkeyModule_StringPtrLen(key, NULL), slot,
                     ValkeyModule_StringPtrLen(slot_value, NULL));
    ValkeyModule_DictReplace(storage_slot_pool[dbid], (ValkeyModuleString *)key, slot_value);
    ValkeyModule_Log(module_ctx, "warning", "  storage_slot_pool[%d] now has %llu entries",
                     dbid, (unsigned long long)ValkeyModule_DictSize(storage_slot_pool[dbid]));
    ValkeyModule_RetainString(NULL, slot_value);
    if (old_slot_value != NULL) {
        ValkeyModule_FreeString(NULL, old_slot_value);
    }
    
    /* Auto-dump on every write if configured */
    if (dump_every_write) {
        ValkeyModule_Log(module_ctx, "notice", "Auto-dumping data after SET operation (dump_every_write=1)");
        ValkeyModule_Log(module_ctx, "debug", "AUTO_DUMP: node_id=%s, dbid=%d", node_id, dbid);
        /* Call storage dump function with appropriate parameters */
        ValkeyModuleString *backup_id = NULL;
        
        ValkeyModule_Log(module_ctx, "debug", "AUTO_DUMP: Calling storageDumpFunction with target=%s, slot=%d", node_id, slot);
        ValkeyModuleString *node_id_str = ValkeyModule_CreateStringPrintf(NULL, "%s", node_id);
        storageDumpFunction(module_ctx, storage_ctx, dbid, slot, 0, node_id_str, &backup_id);
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
    
    int slot = is_cluster_enabled ? ValkeyModule_ClusterKeySlot((ValkeyModuleString *)key) : (unsigned int)EXTERNAL_ALL_SLOTS;
    ValkeyModule_Log(module_ctx, "notice", "storageGet: key=%s slot=%d",
                     ValkeyModule_StringPtrLen(key, NULL), slot);
    
    size_t dict_size = storage_mem_pool[dbid] ? ValkeyModule_DictSize(storage_mem_pool[dbid]) : 0;
    ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: storageGetFunction: dbid=%d, key=%s, dict_size=%zu, node_id=%s",
                     dbid, ValkeyModule_StringPtrLen(key, NULL), dict_size, node_id);

    void *value = ValkeyModule_DictGet(storage_mem_pool[dbid], (ValkeyModuleString *)key, NULL);
    if (!value) {
        ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: storageGetFunction: key not found in storage_mem_pool[%d]", dbid);
        return 0;
    }

    ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: storageGetFunction: key found, returning value");
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
    
    int slot = is_cluster_enabled ? ValkeyModule_ClusterKeySlot((ValkeyModuleString *)key) : (unsigned int)EXTERNAL_ALL_SLOTS;
    ValkeyModule_Log(module_ctx, "notice", "storageDel: key=%s slot=%d",
                     ValkeyModule_StringPtrLen(key, NULL), slot);
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
    
    /* Also remove slot tracking */
    ValkeyModuleString *old_slot_value = ValkeyModule_DictGet(storage_slot_pool[dbid], (ValkeyModuleString *)key, NULL);
    ValkeyModule_DictDel(storage_slot_pool[dbid], (ValkeyModuleString *)key, NULL);
    if (old_slot_value != NULL) {
        ValkeyModule_FreeString(NULL, old_slot_value);
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
    
    if (key == NULL) {
        ValkeyModule_Log(module_ctx, "error", "ERROR: filterSetFunction called with NULL key");
        return EXTERNAL_ERROR;
    }
    
    int slot = is_cluster_enabled ? ValkeyModule_ClusterKeySlot((ValkeyModuleString *)key) : (unsigned int)EXTERNAL_ALL_SLOTS;
    ValkeyModule_Log(module_ctx, "notice", "filterSet: key=%s slot=%d",
                     ValkeyModule_StringPtrLen(key, NULL), slot);
    ValkeyModule_Log(module_ctx, "debug", "DEBUG: filterSetFunction called");
    
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
    
    /* Track slot information for this key */
    ValkeyModuleString *old_slot_value = ValkeyModule_DictGet(filter_slot_pool[dbid], (ValkeyModuleString *)key, NULL);
    ValkeyModuleString *slot_value = ValkeyModule_CreateStringPrintf(NULL, "%d", slot);
    ValkeyModule_DictReplace(filter_slot_pool[dbid], (ValkeyModuleString *)key, slot_value);
    ValkeyModule_RetainString(NULL, slot_value);
    if (old_slot_value != NULL) {
        ValkeyModule_FreeString(NULL, old_slot_value);
    }

    ValkeyModule_Log(module_ctx, "debug", "DEBUG: filterSetFunction - set successfully: %zu",
                     ValkeyModule_DictSize(filter_mem_pool[dbid]));

    /* Auto-dump on every write if configured */
    if (dump_every_write) {
        ValkeyModule_Log(module_ctx, "notice", "Auto-dumping filter data after SET operation (dump_every_write=1), slot=%d", slot);
        ValkeyModuleString *backup_id = NULL;
        ValkeyModuleString *node_id_str = ValkeyModule_CreateStringPrintf(NULL, "%s", node_id);
        filterDumpFunction(module_ctx, filter_ctx, dbid, slot, 0, node_id_str, &backup_id);
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
    
    int slot = is_cluster_enabled ? ValkeyModule_ClusterKeySlot((ValkeyModuleString *)key) : (unsigned int)EXTERNAL_ALL_SLOTS;
    ValkeyModule_Log(module_ctx, "notice", "filterGet: key=%s slot=%d",
                     ValkeyModule_StringPtrLen(key, NULL), slot);
    
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
    
    int slot = is_cluster_enabled ? ValkeyModule_ClusterKeySlot((ValkeyModuleString *)key) : (unsigned int)EXTERNAL_ALL_SLOTS;
    ValkeyModule_Log(module_ctx, "notice", "filterDel: key=%s slot=%d",
                     ValkeyModule_StringPtrLen(key, NULL), slot);
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
    
    /* Also remove slot tracking */
    ValkeyModuleString *old_slot_value = ValkeyModule_DictGet(filter_slot_pool[dbid], (ValkeyModuleString *)key, NULL);
    ValkeyModule_DictDel(filter_slot_pool[dbid], (ValkeyModuleString *)key, NULL);
    if (old_slot_value != NULL) {
        ValkeyModule_FreeString(NULL, old_slot_value);
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
    ValkeyModule_AutoMemory(module_ctx);

    /* Check if we have a snapshot available in the global pool */
    ValkeyModuleDict *snapshot_dict = NULL;
    if (dbid >= 0 && dbid < MAX_DB && filter_snapshot_pool[dbid]) {
        snapshot_dict = filter_snapshot_pool[dbid];
        ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: using snapshot for dump of db%d", dbid);
    }
    
    /* Always use local node_id for DUMP operations.
     * This ensures backups are always created on the local node,
     * simplifying the architecture and avoiding cross-node complexity.
     * The TARGET parameter is intentionally not supported for dumps. */
    char target_node_id[NODE_ID_MAX_LEN];
    snprintf(target_node_id, sizeof(target_node_id), "%s", node_id);
    ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: using local node_id=%s", target_node_id);
    
    /* Handle backup_id: use passed value if initialized, otherwise generate new one */
    char backup_id_str[BACKUP_ID_MAX_LEN];
    generateOrReuseBackupId(module_ctx, backup_id, timestamp, target_node_id, slot,
                            "filterDumpFunction", backup_id_str);
    
    /* Create directory for this server instance */
    char server_dir[SERVER_DIR_MAX_LEN];
    snprintf(server_dir, sizeof(server_dir), "/tmp/external_data/%s", target_node_id);
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
        char db_dir[DB_DIR_MAX_LEN];
        snprintf(db_dir, sizeof(db_dir), "%s/db%d", server_dir, dbid);
        mkdir(db_dir, 0755);
        
        ValkeyModuleDict *dict_to_dump = snapshot_dict ? snapshot_dict : filter_mem_pool[dbid];
        
        /* Create single file with timestamp */
        char file_path[FILENAME_MAX_LEN];
        snprintf(file_path, sizeof(file_path), "%s/%s_filter_%s.dat",
                 db_dir, module_name, backup_id_str);
        
        FILE *fp = fopen(file_path, "w");
        if (!fp) {
            ValkeyModule_Log(module_ctx, "warning", "Failed to create filter file: %s", file_path);
            return EXTERNAL_ERROR;
        }
        
        /* Iterate ALL keys and write with metadata */
        ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(dict_to_dump, "^", NULL, 0);
        const char *key;
        size_t key_len;
        
        while ((key = ValkeyModule_DictNextC(iter, &key_len, NULL)) != NULL) {
            /* Get slot for this key */
            ValkeyModuleString *key_copy = ValkeyModule_CreateString(NULL, key, key_len);
            ValkeyModuleString *slot_str = ValkeyModule_DictGet(filter_slot_pool[dbid], key_copy, NULL);
            int key_slot = 0;  // Default for standalone
            
            if (slot_str != NULL) {
                const char *slot_cstr = ValkeyModule_StringPtrLen(slot_str, NULL);
                key_slot = atoi(slot_cstr);
                /* Map EXTERNAL_ALL_SLOTS to 0 */
                if (!is_cluster_enabled && key_slot == EXTERNAL_ALL_SLOTS) {
                    key_slot = 0;
                }
            }
            
            ValkeyModule_FreeString(NULL, key_copy);
            
            /* Write line with metadata: <slot> <key_len> <key> */
            fprintf(fp, "%d %zu %.*s\n",
                    key_slot, key_len, (int)key_len, key);
        }
        
        ValkeyModule_DictIteratorStop(iter);
        fclose(fp);
        
        ValkeyModule_Log(module_ctx, "notice", "Filter dumped: %s", file_path);
        
    } else if (dbid == EXTERNAL_ALL_DBS) {
        ValkeyModule_Log(module_ctx, "notice", "filterDumpFunction: dumping all databases");
        for (int db = 0; db < MAX_DB; db++) {
            if (filterDumpFunction(module_ctx,
                                   filter_ctx,
                                   db,
                                   slot,
                                   0,
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
    ValkeyModule_AutoMemory(module_ctx);

    /* Check if we have a snapshot available in the global pool */
    ValkeyModuleDict *snapshot_dict = NULL;
    if (dbid >= 0 && dbid < MAX_DB && storage_snapshot_pool[dbid]) {
        snapshot_dict = storage_snapshot_pool[dbid];
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: using snapshot for dump of db%d", dbid);
    }
    
    /* Always use local node_id for DUMP operations.
     * This ensures backups are always created on the local node,
     * simplifying the architecture and avoiding cross-node complexity.
     * The TARGET parameter is intentionally not supported for dumps. */
    char target_node_id[NODE_ID_MAX_LEN];
    snprintf(target_node_id, sizeof(target_node_id), "%s", node_id);
    ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: using local node_id=%s", target_node_id);
    
    /* Handle backup_id: use passed value if initialized, otherwise generate new one */
    char backup_id_str[BACKUP_ID_MAX_LEN];
    generateOrReuseBackupId(module_ctx, backup_id, timestamp, target_node_id, slot,
                            "storageDumpFunction", backup_id_str);
    
    /* Create directory for this server instance */
    char server_dir[SERVER_DIR_MAX_LEN];
    snprintf(server_dir, sizeof(server_dir), "/tmp/external_data/%s", target_node_id);
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
        char db_dir[DB_DIR_MAX_LEN];
        snprintf(db_dir, sizeof(db_dir), "%s/db%d", server_dir, dbid);
        mkdir(db_dir, 0755);
        
        ValkeyModuleDict *dict_to_dump = snapshot_dict ? snapshot_dict : storage_mem_pool[dbid];
        
        /* Create single file with timestamp */
        char file_path[FILENAME_MAX_LEN];
        snprintf(file_path, sizeof(file_path), "%s/%s_storage_%s.dat",
                 db_dir, module_name, backup_id_str);
        
        FILE *fp = fopen(file_path, "w");
        if (!fp) {
            ValkeyModule_Log(module_ctx, "warning", "Failed to create storage file: %s", file_path);
            return EXTERNAL_ERROR;
        }
        
        /* Iterate ALL keys and write with metadata */
        ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(dict_to_dump, "^", NULL, 0);
        const char *key;
        size_t key_len;
        ValkeyModuleString *value;
        
        while ((key = ValkeyModule_DictNextC(iter, &key_len, (void **)&value)) != NULL) {
            if (value == NULL) {
                continue;
            }
            
            /* Get slot for this key */
            ValkeyModuleString *key_copy = ValkeyModule_CreateString(NULL, key, key_len);
            ValkeyModuleString *slot_str = ValkeyModule_DictGet(storage_slot_pool[dbid], key_copy, NULL);
            int key_slot = 0;  // Default for standalone
            
            if (slot_str != NULL) {
                const char *slot_cstr = ValkeyModule_StringPtrLen(slot_str, NULL);
                key_slot = atoi(slot_cstr);
                /* Map EXTERNAL_ALL_SLOTS to 0 */
                if (!is_cluster_enabled && key_slot == EXTERNAL_ALL_SLOTS) {
                    key_slot = 0;
                }
            }
            
            ValkeyModule_FreeString(NULL, key_copy);
            
            /* Get value */
            size_t val_len;
            const char *val_str = ValkeyModule_StringPtrLen(value, &val_len);
            
            /* Write line with metadata: <slot> <key_len> <key> <val_len> <val> */
            fprintf(fp, "%d %zu %.*s %zu %.*s\n",
                    key_slot, key_len, (int)key_len, key,
                    val_len, (int)val_len, val_str);
        }
        
        ValkeyModule_DictIteratorStop(iter);
        fclose(fp);
        
        ValkeyModule_Log(module_ctx, "notice", "Storage dumped: %s", file_path);
        
    } else if (dbid == EXTERNAL_ALL_DBS) {
        ValkeyModule_Log(module_ctx, "notice", "storageDumpFunction: dumping all databases");
        for (int db = 0; db < MAX_DB; db++) {
            if (storageDumpFunction(module_ctx,
                                    storage_ctx,
                                    db,
                                    slot,
                                    0,
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

/* Helper function to find the latest backup_id in a directory
 * Returns EXTERNAL_SUCCESS and sets backup_id_out if found, EXTERNAL_NOT_FOUND otherwise */
/* Structure to hold multiple backup IDs */
#define MAX_BACKUP_IDS 32
typedef struct {
    char backup_ids[MAX_BACKUP_IDS][BACKUP_ID_MAX_LEN];
    int count;
} BackupIdList;

/* Find ALL backup IDs with the latest timestamp for a given file type */
static int findAllLatestBackupIds(ValkeyModuleCtx *module_ctx, const char *source_node_id,
                                   int dbid, const char *file_type,
                                   BackupIdList *backup_list) {
    char db_dir[DB_DIR_MAX_LEN];
    snprintf(db_dir, sizeof(db_dir), "/tmp/external_data/%s/db%d", source_node_id, dbid);
    
    DIR *dir = opendir(db_dir);
    if (!dir) {
        ValkeyModule_Log(module_ctx, "warning", "Cannot open directory: %s", db_dir);
        return EXTERNAL_NOT_FOUND;
    }
    
    backup_list->count = 0;
    long long max_timestamp = -1;
    
    /* First pass: find the maximum timestamp */
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Look for files matching pattern: helloextdata1_<file_type>_v0:<node_id>:<timestamp>.dat */
        if (strstr(entry->d_name, file_type) != NULL &&
            strstr(entry->d_name, ".dat") != NULL &&
            strstr(entry->d_name, "v0:") != NULL) {
            
            /* Extract backup_id from filename */
            const char *backup_start = strstr(entry->d_name, "v0:");
            if (backup_start) {
                /* Find the end of backup_id (before .dat) */
                const char *backup_end = strstr(backup_start, ".dat");
                if (backup_end) {
                    size_t backup_len = backup_end - backup_start;
                    if (backup_len < BACKUP_ID_MAX_LEN) {
                        char current_backup_id[BACKUP_ID_MAX_LEN];
                        memcpy(current_backup_id, backup_start, backup_len);
                        current_backup_id[backup_len] = '\0';
                        
                        /* Parse to extract timestamp */
                        char temp_node_id[NODE_ID_MAX_LEN];
                        int temp_slot;
                        long long timestamp;
                        char temp_options[OPTIONS_MAX_LEN];
                        
                        ValkeyModuleString *backup_str = ValkeyModule_CreateString(NULL, current_backup_id, backup_len);
                        if (parseBackupIdV0(module_ctx, backup_str,
                                          temp_node_id, sizeof(temp_node_id),
                                          &temp_slot,
                                          &timestamp, temp_options, sizeof(temp_options)) == EXTERNAL_SUCCESS) {
                            if (timestamp > max_timestamp) {
                                max_timestamp = timestamp;
                            }
                        }
                        ValkeyModule_FreeString(NULL, backup_str);
                    }
                }
            }
        }
    }
    
    if (max_timestamp < 0) {
        closedir(dir);
        ValkeyModule_Log(module_ctx, "warning", "No backup files found in %s", db_dir);
        return EXTERNAL_NOT_FOUND;
    }
    
    /* Second pass: collect ALL backup_ids with the max timestamp */
    rewinddir(dir);
    while ((entry = readdir(dir)) != NULL && backup_list->count < MAX_BACKUP_IDS) {
        if (strstr(entry->d_name, file_type) != NULL &&
            strstr(entry->d_name, ".dat") != NULL &&
            strstr(entry->d_name, "v0:") != NULL) {
            
            const char *backup_start = strstr(entry->d_name, "v0:");
            if (backup_start) {
                const char *backup_end = strstr(backup_start, ".dat");
                if (backup_end) {
                    size_t backup_len = backup_end - backup_start;
                    if (backup_len < BACKUP_ID_MAX_LEN) {
                        char current_backup_id[BACKUP_ID_MAX_LEN];
                        memcpy(current_backup_id, backup_start, backup_len);
                        current_backup_id[backup_len] = '\0';
                        
                        /* Parse to check timestamp */
                        char temp_node_id[NODE_ID_MAX_LEN];
                        int temp_slot;
                        long long timestamp;
                        char temp_options[OPTIONS_MAX_LEN];
                        
                        ValkeyModuleString *backup_str = ValkeyModule_CreateString(NULL, current_backup_id, backup_len);
                        if (parseBackupIdV0(module_ctx, backup_str,
                                          temp_node_id, sizeof(temp_node_id),
                                          &temp_slot,
                                          &timestamp, temp_options, sizeof(temp_options)) == EXTERNAL_SUCCESS) {
                            /* Only collect backups with the max timestamp */
                            if (timestamp == max_timestamp) {
                                strncpy(backup_list->backup_ids[backup_list->count],
                                       current_backup_id,
                                       BACKUP_ID_MAX_LEN - 1);
                                backup_list->backup_ids[backup_list->count][BACKUP_ID_MAX_LEN - 1] = '\0';
                                ValkeyModule_Log(module_ctx, "notice",
                                               "Discovered backup [%d]: %s (timestamp=%lld)",
                                               backup_list->count, current_backup_id, timestamp);
                                backup_list->count++;
                            }
                        }
                        ValkeyModule_FreeString(NULL, backup_str);
                    }
                }
            }
        }
    }
    closedir(dir);
    
    if (backup_list->count > 0) {
        ValkeyModule_Log(module_ctx, "notice",
                       "Found %d backup(s) for db%d with timestamp=%lld",
                       backup_list->count, dbid, max_timestamp);
        return EXTERNAL_SUCCESS;
    }
    
    ValkeyModule_Log(module_ctx, "warning", "No backup files found with timestamp %lld", max_timestamp);
    return EXTERNAL_NOT_FOUND;
}

/* Legacy function for single backup discovery - kept for backward compatibility */
static int findLatestBackupId(ValkeyModuleCtx *module_ctx, const char *source_node_id,
                               int dbid, const char *file_type,
                               char *backup_id_out, size_t backup_id_out_size) {
    BackupIdList backup_list;
    if (findAllLatestBackupIds(module_ctx, source_node_id, dbid, file_type, &backup_list) == EXTERNAL_SUCCESS) {
        if (backup_list.count > 0) {
            strncpy(backup_id_out, backup_list.backup_ids[0], backup_id_out_size - 1);
            backup_id_out[backup_id_out_size - 1] = '\0';
            return EXTERNAL_SUCCESS;
        }
    }
    return EXTERNAL_NOT_FOUND;
}

static int filterLoadFunction(ValkeyModuleCtx *module_ctx,
                              ValkeyModuleExternalFilterCtx *filter_ctx,
                              int dbid,
                              ValkeyModuleString *backup_id) {
    VALKEYMODULE_NOT_USED(filter_ctx);
    ValkeyModule_AutoMemory(module_ctx);
   
    if (dbid == EXTERNAL_ALL_DBS) {
        int success = 0;
        for (int db = 0; db < MAX_DB; db++) {
            int res = filterLoadFunction(module_ctx, filter_ctx, db, backup_id);
            if (res == EXTERNAL_ERROR) return EXTERNAL_ERROR;
            if (res == EXTERNAL_SUCCESS) success = 1;
        }
        return success ? EXTERNAL_SUCCESS : EXTERNAL_ERROR;
    }

    /* Handle NULL backup_id OR timestamp=0 - auto-discover latest */
    char auto_backup_id[BACKUP_ID_MAX_LEN];
    ValkeyModuleString *backup_id_to_use = backup_id;
    ValkeyModuleString *created_backup_id = NULL;
    char source_node_id[NODE_ID_MAX_LEN];
    int parsed_slot = -1;
    long long parsed_timestamp = 0;
    
    /* First, check if we need auto-discovery */
    int need_auto_discovery = 0;
    if (backup_id == NULL) {
        need_auto_discovery = 1;
    } else {
        /* Parse the provided backup_id to check if timestamp is 0 */
        size_t backup_id_len;
        const char *backup_id_str = ValkeyModule_StringPtrLen(backup_id, &backup_id_len);
        char parsed_options[OPTIONS_MAX_LEN];
        
        if (parseBackupIdV0(module_ctx, backup_id,
                           source_node_id, sizeof(source_node_id),
                           &parsed_slot,
                           &parsed_timestamp,
                           parsed_options, sizeof(parsed_options)) != EXTERNAL_SUCCESS) {
            ValkeyModule_Log(module_ctx, "warning", "Invalid backup_id format: %.*s",
                           (int)backup_id_len, backup_id_str);
            return EXTERNAL_ERROR;
        }
        
        if (parsed_timestamp == 0) {
            need_auto_discovery = 1;
        }
    }
    
    if (need_auto_discovery) {
        /* Auto-discover ALL latest backups (for cluster mode with multiple slots) */
        const char *search_node_id = (backup_id == NULL) ? node_id : source_node_id;
        BackupIdList backup_list;
        
        if (findAllLatestBackupIds(module_ctx, search_node_id, dbid, "filter", &backup_list) != EXTERNAL_SUCCESS) {
            ValkeyModule_Log(module_ctx, "warning", "No filter backup found for auto-load in node_id=%s directory", search_node_id);
            return EXTERNAL_NOT_FOUND;
        }
        
        /* Clear existing filter data once before loading all backups */
        filterFlushFunction(module_ctx, filter_ctx, dbid);
        
        /* Mark that we're loading - this will cause incoming commands to be queued */
        is_loading[dbid] = 1;
        
        int total_loaded = 0;
        int success_count = 0;
        
        /* Load ALL discovered backups */
        for (int i = 0; i < backup_list.count; i++) {
            const char *current_backup_id = backup_list.backup_ids[i];
            ValkeyModule_Log(module_ctx, "notice", "Loading filter backup [%d/%d]: %s",
                           i + 1, backup_list.count, current_backup_id);
            
            /* Parse backup_id to get source_node_id and slot */
            char current_source_node_id[NODE_ID_MAX_LEN];
            int current_slot;
            long long current_timestamp;
            char current_options[OPTIONS_MAX_LEN];
            
            ValkeyModuleString *backup_str = ValkeyModule_CreateString(NULL, current_backup_id, strlen(current_backup_id));
            if (parseBackupIdV0(module_ctx, backup_str,
                               current_source_node_id, sizeof(current_source_node_id),
                               &current_slot,
                               &current_timestamp,
                               current_options, sizeof(current_options)) != EXTERNAL_SUCCESS) {
                ValkeyModule_Log(module_ctx, "warning", "Invalid backup_id format: %s", current_backup_id);
                ValkeyModule_FreeString(NULL, backup_str);
                continue;
            }
            ValkeyModule_FreeString(NULL, backup_str);
            
            /* Construct file path */
            char file_path[FILENAME_MAX_LEN];
            snprintf(file_path, sizeof(file_path),
                     "/tmp/external_data/%s/db%d/%s_filter_%s.dat",
                     current_source_node_id, dbid, module_name, current_backup_id);
            
            FILE *fp = fopen(file_path, "r");
            if (!fp) {
                ValkeyModule_Log(module_ctx, "warning", "Filter file not found: %s", file_path);
                continue;
            }
            
            /* Read and parse each line: <slot> <key_len> <key> */
            char line[8192];
            int loaded_count = 0;
            while (fgets(line, sizeof(line), fp) != NULL) {
                int line_slot;
                size_t key_len;
                
                /* Parse metadata: <slot> <key_len> */
                if (sscanf(line, "%d %zu", &line_slot, &key_len) != 2) {
                    ValkeyModule_Log(module_ctx, "warning", "Invalid line format in %s", file_path);
                    continue;
                }
                
                /* Find position after "<slot> <key_len> " */
                char *key_start = line;
                for (int i = 0; i < 2; i++) {
                    key_start = strchr(key_start, ' ');
                    if (!key_start) break;
                    key_start++;
                }
                
                if (!key_start || strlen(key_start) < key_len) {
                    ValkeyModule_Log(module_ctx, "warning", "Invalid key in line");
                    continue;
                }
                
                /* Read key (handle potential newline at end) */
                char *key_buf = malloc(key_len + 1);
                if (!key_buf) continue;
                
                memcpy(key_buf, key_start, key_len);
                key_buf[key_len] = '\0';
                
                /* Store in memory pool */
                ValkeyModuleString *key_str = ValkeyModule_CreateString(NULL, key_buf, key_len);
                ValkeyModule_DictReplace(filter_mem_pool[dbid], key_str, "");
                
                /* Store slot information */
                char slot_str[16];
                snprintf(slot_str, sizeof(slot_str), "%d", line_slot);
                ValkeyModuleString *slot_value = ValkeyModule_CreateString(NULL, slot_str, strlen(slot_str));
                ValkeyModule_DictReplace(filter_slot_pool[dbid], key_str, slot_value);
                ValkeyModule_RetainString(NULL, slot_value);
                
                ValkeyModule_FreeString(NULL, key_str);
                free(key_buf);
                loaded_count++;
                
                ValkeyModule_Log(module_ctx, "debug",
                    "Loaded filter key: db=%d, slot=%d, key_len=%zu",
                    dbid, line_slot, key_len);
            }
            
            fclose(fp);
            
            if (loaded_count > 0) {
                ValkeyModule_Log(module_ctx, "notice",
                               "Loaded %d filter entries from slot:%d backup: %s",
                               loaded_count, current_slot, current_backup_id);
                total_loaded += loaded_count;
                success_count++;
            }
        }
        
        /* Done loading - allow incoming commands to be processed */
        is_loading[dbid] = 0;
        
        /* Drain queued commands that arrived during loading */
        drainCommandQueue(module_ctx, dbid);
        
        if (success_count > 0) {
            ValkeyModule_Log(module_ctx, "notice",
                           "Successfully loaded %d filter entries from %d backup(s)",
                           total_loaded, success_count);
            return EXTERNAL_SUCCESS;
        } else {
            ValkeyModule_Log(module_ctx, "warning", "Failed to load any filter backups");
            return EXTERNAL_ERROR;
        }
    } else {
        backup_id_to_use = backup_id;
        
        /* At this point, we have a valid backup_id with real timestamp */
        size_t backup_id_len;
        const char *backup_id_str = ValkeyModule_StringPtrLen(backup_id_to_use, &backup_id_len);
        
        /* Construct file path for single filter file */
        char file_path[FILENAME_MAX_LEN];
        snprintf(file_path, sizeof(file_path),
                 "/tmp/external_data/%s/db%d/%s_filter_%.*s.dat",
                 source_node_id, dbid, module_name, (int)backup_id_len, backup_id_str);
        
        FILE *fp = fopen(file_path, "r");
        if (!fp) {
            ValkeyModule_Log(module_ctx, "warning", "Filter file not found: %s", file_path);
            return EXTERNAL_NOT_FOUND;
        }
        
        /* Clear existing filter data */
        filterFlushFunction(module_ctx, filter_ctx, dbid);
        
        /* Mark that we're loading - this will cause incoming commands to be queued */
        is_loading[dbid] = 1;
        
        int loaded_count = 0;
        
        /* Read and parse each line: <slot> <key_len> <key> */
        char line[8192];
        while (fgets(line, sizeof(line), fp) != NULL) {
            int line_slot;
            size_t key_len;
            
            /* Parse metadata: <slot> <key_len> */
            if (sscanf(line, "%d %zu", &line_slot, &key_len) != 2) {
                ValkeyModule_Log(module_ctx, "warning", "Invalid line format in %s", file_path);
                continue;
            }
            
            /* Find position after "<slot> <key_len> " */
            char *key_start = line;
            for (int i = 0; i < 2; i++) {
                key_start = strchr(key_start, ' ');
                if (!key_start) break;
                key_start++;
            }
            
            if (!key_start || strlen(key_start) < key_len) {
                ValkeyModule_Log(module_ctx, "warning", "Invalid key in line");
                continue;
            }
            
            /* Read key (handle potential newline at end) */
            char *key_buf = malloc(key_len + 1);
            if (!key_buf) continue;
            
            memcpy(key_buf, key_start, key_len);
            key_buf[key_len] = '\0';
            
            /* Store in memory pool */
            ValkeyModuleString *key_str = ValkeyModule_CreateString(NULL, key_buf, key_len);
            ValkeyModule_DictReplace(filter_mem_pool[dbid], key_str, "");
            
            /* Store slot information */
            char slot_str[16];
            snprintf(slot_str, sizeof(slot_str), "%d", line_slot);
            ValkeyModuleString *slot_value = ValkeyModule_CreateString(NULL, slot_str, strlen(slot_str));
            ValkeyModule_DictReplace(filter_slot_pool[dbid], key_str, slot_value);
            ValkeyModule_RetainString(NULL, slot_value);
            
            ValkeyModule_FreeString(NULL, key_str);
            free(key_buf);
            loaded_count++;
            
            ValkeyModule_Log(module_ctx, "debug",
                "Loaded filter key: db=%d, slot=%d, key_len=%zu",
                dbid, line_slot, key_len);
        }
        
        fclose(fp);
        
        ValkeyModule_Log(module_ctx, "notice", "Filter loaded from: %s (%d entries)", file_path, loaded_count);
        
        /* Done loading - allow incoming commands to be processed */
        is_loading[dbid] = 0;
        
        /* Drain queued commands that arrived during loading */
        drainCommandQueue(module_ctx, dbid);
        
        return loaded_count > 0 ? EXTERNAL_SUCCESS : EXTERNAL_NOT_FOUND;
    }
}

static int storageLoadFunction(ValkeyModuleCtx *module_ctx,
                               ValkeyModuleExternalStorageCtx *storage_ctx,
                               int dbid,
                               ValkeyModuleString *backup_id) {
    VALKEYMODULE_NOT_USED(storage_ctx);
    ValkeyModule_AutoMemory(module_ctx);
   
    ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: storageLoadFunction called - dbid=%d, backup_id=%s, node_id=%s",
                     dbid,
                     backup_id ? ValkeyModule_StringPtrLen(backup_id, NULL) : "NULL",
                     node_id);
   
    if (dbid == EXTERNAL_ALL_DBS) {
        int success = 0;
        for (int db = 0; db < MAX_DB; db++) {
            int res = storageLoadFunction(module_ctx, storage_ctx, db, backup_id);
            if (res == EXTERNAL_ERROR) return EXTERNAL_ERROR;
            if (res == EXTERNAL_SUCCESS) success = 1;
        }
        return success ? EXTERNAL_SUCCESS : EXTERNAL_ERROR;
    }

    /* Handle NULL backup_id OR timestamp=0 - auto-discover latest */
    char auto_backup_id[BACKUP_ID_MAX_LEN];
    ValkeyModuleString *backup_id_to_use = backup_id;
    ValkeyModuleString *created_backup_id = NULL;
    char source_node_id[NODE_ID_MAX_LEN];
    int parsed_slot = -1;
    long long parsed_timestamp = 0;
    
    /* First, check if we need auto-discovery */
    int need_auto_discovery = 0;
    if (backup_id == NULL) {
        ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: backup_id is NULL, need auto-discovery");
        need_auto_discovery = 1;
    } else {
        /* Parse the provided backup_id to check if timestamp is 0 */
        size_t backup_id_len;
        const char *backup_id_str = ValkeyModule_StringPtrLen(backup_id, &backup_id_len);
        char parsed_options[OPTIONS_MAX_LEN];
        
        if (parseBackupIdV0(module_ctx, backup_id,
                           source_node_id, sizeof(source_node_id),
                           &parsed_slot,
                           &parsed_timestamp,
                           parsed_options, sizeof(parsed_options)) != EXTERNAL_SUCCESS) {
            ValkeyModule_Log(module_ctx, "warning", "Invalid backup_id format: %.*s",
                           (int)backup_id_len, backup_id_str);
            return EXTERNAL_ERROR;
        }
        
        if (parsed_timestamp == 0) {
            ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: backup_id has timestamp=0, need auto-discovery for node_id=%s", source_node_id);
            need_auto_discovery = 1;
        }
    }
    
    if (need_auto_discovery) {
        /* Auto-discover ALL latest backups (for cluster mode with multiple slots) */
        const char *search_node_id = (backup_id == NULL) ? node_id : source_node_id;
        BackupIdList backup_list;
        
        if (findAllLatestBackupIds(module_ctx, search_node_id, dbid, "storage", &backup_list) != EXTERNAL_SUCCESS) {
            ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: No storage backup found for auto-load in node_id=%s directory", search_node_id);
            return EXTERNAL_NOT_FOUND;
        }
        
        /* Clear existing storage data once before loading all backups */
        storageFlushFunction(module_ctx, storage_ctx, dbid);
        
        /* Mark that we're loading - this will cause incoming commands to be queued */
        is_loading[dbid] = 1;
        
        int total_loaded = 0;
        int success_count = 0;
        
        /* Load ALL discovered backups */
        for (int i = 0; i < backup_list.count; i++) {
            const char *current_backup_id = backup_list.backup_ids[i];
            ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: Loading storage backup [%d/%d]: %s",
                           i + 1, backup_list.count, current_backup_id);
            
            /* Parse backup_id to get source_node_id and slot */
            char current_source_node_id[NODE_ID_MAX_LEN];
            int current_slot;
            long long current_timestamp;
            char current_options[OPTIONS_MAX_LEN];
            
            ValkeyModuleString *backup_str = ValkeyModule_CreateString(NULL, current_backup_id, strlen(current_backup_id));
            if (parseBackupIdV0(module_ctx, backup_str,
                               current_source_node_id, sizeof(current_source_node_id),
                               &current_slot,
                               &current_timestamp,
                               current_options, sizeof(current_options)) != EXTERNAL_SUCCESS) {
                ValkeyModule_Log(module_ctx, "warning", "Invalid backup_id format: %s", current_backup_id);
                ValkeyModule_FreeString(NULL, backup_str);
                continue;
            }
            ValkeyModule_FreeString(NULL, backup_str);
            
            /* Construct file path */
            char file_path[FILENAME_MAX_LEN];
            snprintf(file_path, sizeof(file_path),
                     "/tmp/external_data/%s/db%d/%s_storage_%s.dat",
                     current_source_node_id, dbid, module_name, current_backup_id);
            
            ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: Looking for storage file at: %s", file_path);
            
            FILE *fp = fopen(file_path, "r");
            if (!fp) {
                ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: Storage file not found: %s (errno=%d)", file_path, errno);
                continue;
            }
            
            ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: Successfully opened storage file: %s", file_path);
            
            /* Read and parse each line: <slot> <key_len> <key> <val_len> <val> */
            char line[8192];
            int loaded_count = 0;
            while (fgets(line, sizeof(line), fp) != NULL) {
                int line_slot;
                size_t key_len, val_len;
                
                /* Parse metadata: <slot> <key_len> */
                if (sscanf(line, "%d %zu", &line_slot, &key_len) != 2) {
                    ValkeyModule_Log(module_ctx, "warning", "Invalid line format in %s", file_path);
                    continue;
                }
                
                /* Find position after "<slot> <key_len> " */
                char *key_start = line;
                for (int i = 0; i < 2; i++) {
                    key_start = strchr(key_start, ' ');
                    if (!key_start) break;
                    key_start++;
                }
                
                if (!key_start || strlen(key_start) < key_len) {
                    ValkeyModule_Log(module_ctx, "warning", "Invalid key in line");
                    continue;
                }
                
                /* Read key */
                char *key_buf = malloc(key_len + 1);
                if (!key_buf) continue;
                memcpy(key_buf, key_start, key_len);
                key_buf[key_len] = '\0';
                
                /* Find value length position */
                char *val_len_start = key_start + key_len;
                if (*val_len_start != ' ') {
                    free(key_buf);
                    continue;
                }
                val_len_start++;
                
                if (sscanf(val_len_start, "%zu", &val_len) != 1) {
                    free(key_buf);
                    continue;
                }
                
                /* Find value start */
                char *val_start = strchr(val_len_start, ' ');
                if (!val_start) {
                    free(key_buf);
                    continue;
                }
                val_start++;
                
                /* Read value (handle potential newline at end) */
                char *val_buf = malloc(val_len + 1);
                if (!val_buf) {
                    free(key_buf);
                    continue;
                }
                
                size_t available = strlen(val_start);
                if (available < val_len) {
                    free(key_buf);
                    free(val_buf);
                    continue;
                }
                
                memcpy(val_buf, val_start, val_len);
                val_buf[val_len] = '\0';
                
                /* Store in memory pools */
                ValkeyModuleString *key_str = ValkeyModule_CreateString(NULL, key_buf, key_len);
                ValkeyModuleString *val_str = ValkeyModule_CreateString(NULL, val_buf, val_len);
                
                ValkeyModule_DictReplace(storage_mem_pool[dbid], key_str, val_str);
                ValkeyModule_RetainString(NULL, val_str);
                
                /* Store slot information */
                char slot_str[16];
                snprintf(slot_str, sizeof(slot_str), "%d", line_slot);
                ValkeyModuleString *slot_value = ValkeyModule_CreateString(NULL, slot_str, strlen(slot_str));
                ValkeyModule_DictReplace(storage_slot_pool[dbid], key_str, slot_value);
                ValkeyModule_RetainString(NULL, slot_value);
                
                ValkeyModule_FreeString(NULL, key_str);
                free(key_buf);
                free(val_buf);
                loaded_count++;
                
                ValkeyModule_Log(module_ctx, "debug",
                    "Loaded storage key: db=%d, slot=%d, key_len=%zu, val_len=%zu",
                    dbid, line_slot, key_len, val_len);
            }
            
            fclose(fp);
            
            if (loaded_count > 0) {
                ValkeyModule_Log(module_ctx, "warning",
                               "DEBUG_SYNC: Loaded %d storage entries from slot:%d backup: %s",
                               loaded_count, current_slot, current_backup_id);
                total_loaded += loaded_count;
                success_count++;
            }
        }
        
        ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: Total storage entries loaded: %d from %d backup(s), storage_mem_pool[%d] size=%llu",
                         total_loaded, success_count, dbid,
                         (unsigned long long)ValkeyModule_DictSize(storage_mem_pool[dbid]));
        
        /* Done loading - allow incoming commands to be processed */
        is_loading[dbid] = 0;
        
        /* Drain queued commands that arrived during loading */
        drainCommandQueue(module_ctx, dbid);
        
        if (success_count > 0) {
            return EXTERNAL_SUCCESS;
        } else {
            ValkeyModule_Log(module_ctx, "warning", "Failed to load any storage backups");
            return EXTERNAL_ERROR;
        }
    } else {
        backup_id_to_use = backup_id;
        
        /* At this point, we have a valid backup_id with real timestamp */
        size_t backup_id_len;
        const char *backup_id_str = ValkeyModule_StringPtrLen(backup_id_to_use, &backup_id_len);
        
        /* Construct file path for single storage file */
        char file_path[FILENAME_MAX_LEN];
        snprintf(file_path, sizeof(file_path),
                 "/tmp/external_data/%s/db%d/%s_storage_%.*s.dat",
                 source_node_id, dbid, module_name, (int)backup_id_len, backup_id_str);
        
        ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: Looking for storage file at: %s", file_path);
        
        FILE *fp = fopen(file_path, "r");
        if (!fp) {
            ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: Storage file not found: %s (errno=%d)", file_path, errno);
            return EXTERNAL_NOT_FOUND;
        }
        
        ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: Successfully opened storage file: %s", file_path);
        
        /* Clear existing storage data */
        storageFlushFunction(module_ctx, storage_ctx, dbid);
        
        /* Mark that we're loading - this will cause incoming commands to be queued */
        is_loading[dbid] = 1;
        
        int loaded_count = 0;
        
        /* Read and parse each line: <slot> <key_len> <key> <val_len> <val> */
        char line[8192];
        while (fgets(line, sizeof(line), fp) != NULL) {
            int line_slot;
            size_t key_len, val_len;
            
            /* Parse metadata: <slot> <key_len> */
            if (sscanf(line, "%d %zu", &line_slot, &key_len) != 2) {
                ValkeyModule_Log(module_ctx, "warning", "Invalid line format in %s", file_path);
                continue;
            }
            
            /* Find position after "<slot> <key_len> " */
            char *key_start = line;
            for (int i = 0; i < 2; i++) {
                key_start = strchr(key_start, ' ');
                if (!key_start) break;
                key_start++;
            }
            
            if (!key_start || strlen(key_start) < key_len) {
                ValkeyModule_Log(module_ctx, "warning", "Invalid key in line");
                continue;
            }
            
            /* Read key */
            char *key_buf = malloc(key_len + 1);
            if (!key_buf) continue;
            memcpy(key_buf, key_start, key_len);
            key_buf[key_len] = '\0';
            
            /* Find value length position */
            char *val_len_start = key_start + key_len;
            if (*val_len_start != ' ') {
                free(key_buf);
                continue;
            }
            val_len_start++;
            
            if (sscanf(val_len_start, "%zu", &val_len) != 1) {
                free(key_buf);
                continue;
            }
            
            /* Find value start */
            char *val_start = strchr(val_len_start, ' ');
            if (!val_start) {
                free(key_buf);
                continue;
            }
            val_start++;
            
            /* Read value (handle potential newline at end) */
            char *val_buf = malloc(val_len + 1);
            if (!val_buf) {
                free(key_buf);
                continue;
            }
            
            size_t available = strlen(val_start);
            if (available < val_len) {
                free(key_buf);
                free(val_buf);
                continue;
            }
            
            memcpy(val_buf, val_start, val_len);
            val_buf[val_len] = '\0';
            
            /* Store in memory pools */
            ValkeyModuleString *key_str = ValkeyModule_CreateString(NULL, key_buf, key_len);
            ValkeyModuleString *val_str = ValkeyModule_CreateString(NULL, val_buf, val_len);
            
            ValkeyModule_DictReplace(storage_mem_pool[dbid], key_str, val_str);
            ValkeyModule_RetainString(NULL, val_str);
            
            /* Store slot information */
            char slot_str[16];
            snprintf(slot_str, sizeof(slot_str), "%d", line_slot);
            ValkeyModuleString *slot_value = ValkeyModule_CreateString(NULL, slot_str, strlen(slot_str));
            ValkeyModule_DictReplace(storage_slot_pool[dbid], key_str, slot_value);
            ValkeyModule_RetainString(NULL, slot_value);
            
            ValkeyModule_FreeString(NULL, key_str);
            free(key_buf);
            free(val_buf);
            loaded_count++;
            
            ValkeyModule_Log(module_ctx, "debug",
                "Loaded storage key: db=%d, slot=%d, key_len=%zu, val_len=%zu",
                dbid, line_slot, key_len, val_len);
        }
        
        fclose(fp);
        
        ValkeyModule_Log(module_ctx, "warning", "DEBUG_SYNC: Storage loaded from: %s (%d entries), storage_mem_pool[%d] size=%llu",
                         file_path, loaded_count, dbid,
                         (unsigned long long)ValkeyModule_DictSize(storage_mem_pool[dbid]));
        
        /* Done loading - allow incoming commands to be processed */
        is_loading[dbid] = 0;
        
        /* Drain queued commands that arrived during loading */
        drainCommandQueue(module_ctx, dbid);
        
        return loaded_count > 0 ? EXTERNAL_SUCCESS : EXTERNAL_NOT_FOUND;
    }
}

/* Helper function to find databases from backup files - reused by get_state */
static int findDatabasesFromBackup(ValkeyModuleCtx *module_ctx, ValkeyModuleString *source, int **db_numbers, size_t *num_dbs) {
    /* Determine which directory to look in based on source parameter */
    char server_dir[1024];
    if (source != NULL) {
        /* We're on a replica - use the primary's directory */
        size_t source_len;
        const char *source_str = ValkeyModule_StringPtrLen(source, &source_len);
        /* Source might be ip:port, need to look up node_id in mapping */
        const char *looked_up_node_id = find_node_id_by_address(module_ctx, source_str);
        if (looked_up_node_id) {
            source_str = looked_up_node_id;
        }
        snprintf(server_dir, sizeof(server_dir), "/tmp/external_data/%s", source_str);
        ValkeyModule_Log(module_ctx, "notice", "Looking for backups in primary's directory: %s", server_dir);
    } else {
        /* We're on primary - use local directory */
        snprintf(server_dir, sizeof(server_dir), "/tmp/external_data/%s", node_id);
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
                char db_dir[SERVER_DIR_MAX_LEN];
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

/* Module command to get slot for a key from storage */
int StorageGetSlotCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) {
        return ValkeyModule_WrongArity(ctx);
    }
    
    /* Parse db number */
    const char *db_str = ValkeyModule_StringPtrLen(argv[1], NULL);
    if (strncmp(db_str, "db", 2) != 0) {
        ValkeyModule_ReplyWithError(ctx, "ERR invalid database format");
        return VALKEYMODULE_OK;
    }
    int dbid = atoi(db_str + 2);
    if (dbid < 0 || dbid >= MAX_DB) {
        ValkeyModule_ReplyWithError(ctx, "ERR invalid database number");
        return VALKEYMODULE_OK;
    }
    
    /* Get slot for key */
    ValkeyModuleString *key = argv[2];
    ValkeyModuleString *slot_value = ValkeyModule_DictGet(storage_slot_pool[dbid], key, NULL);
    
    if (slot_value) {
        ValkeyModule_ReplyWithString(ctx, slot_value);
    } else {
        ValkeyModule_ReplyWithLongLong(ctx, EXTERNAL_ALL_SLOTS);
    }
    
    return VALKEYMODULE_OK;
}

/* Module command to get slot for a key from filter */
int FilterGetSlotCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) {
        return ValkeyModule_WrongArity(ctx);
    }
    
    /* Parse db number */
    const char *db_str = ValkeyModule_StringPtrLen(argv[1], NULL);
    if (strncmp(db_str, "db", 2) != 0) {
        ValkeyModule_ReplyWithError(ctx, "ERR invalid database format");
        return VALKEYMODULE_OK;
    }
    int dbid = atoi(db_str + 2);
    if (dbid < 0 || dbid >= MAX_DB) {
        ValkeyModule_ReplyWithError(ctx, "ERR invalid database number");
        return VALKEYMODULE_OK;
    }
    
    /* Get slot for key */
    ValkeyModuleString *key = argv[2];
    ValkeyModuleString *slot_value = ValkeyModule_DictGet(filter_slot_pool[dbid], key, NULL);
    
    if (slot_value) {
        ValkeyModule_ReplyWithString(ctx, slot_value);
    } else {
        ValkeyModule_ReplyWithLongLong(ctx, EXTERNAL_ALL_SLOTS);
    }
    
    return VALKEYMODULE_OK;
}

/* Node ID and mapping storage */
static ValkeyModuleDict *node_mapping_dict = NULL; /* In-memory mapping of ip:port -> node_id */

/* Note: load_node_id() and save_node_id() removed - using mapping file as source of truth */

/* Load node mappings from /tmp/external_data/node_mappings.dat */
static void load_node_mappings(ValkeyModuleCtx *ctx) {
    const char *filepath = "/tmp/external_data/node_mappings.dat";
    FILE *f = fopen(filepath, "r");
    if (!f) {
        ValkeyModule_Log(ctx, "notice", "No existing node mappings file");
        return;
    }
    
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        
        /* Parse line: ip:port,node_id */
        char *comma = strchr(line, ',');
        if (!comma) continue;
        
        *comma = '\0';
        char *ip_port = line;
        char *node_id_value = comma + 1;
        
        /* Store in dictionary */
        ValkeyModuleString *key = ValkeyModule_CreateString(ctx, ip_port, strlen(ip_port));
        ValkeyModuleString *val = ValkeyModule_CreateString(ctx, node_id_value, strlen(node_id_value));
        ValkeyModule_DictSet(node_mapping_dict, key, val);
        
        count++;
    }
    
    fclose(f);
    ValkeyModule_Log(ctx, "notice", "Loaded %d node mappings", count);
}

/* Save a single node mapping to /tmp/external_data/node_mappings.dat */
static int save_node_mapping(ValkeyModuleCtx *ctx, const char *ip_port, const char *node_id_value) {
    const char *dirpath = "/tmp/external_data";
    const char *filepath = "/tmp/external_data/node_mappings.dat";
    
    /* Create directory if it doesn't exist */
    struct stat st = {0};
    if (stat(dirpath, &st) == -1) {
        if (mkdir(dirpath, 0755) == -1) {
            ValkeyModule_Log(ctx, "warning", "Failed to create directory %s: %s", 
                           dirpath, strerror(errno));
            return 0;
        }
    }
    
    /* First, load all existing mappings into memory */
    FILE *f = fopen(filepath, "r");
    char **lines = NULL;
    int line_count = 0;
    int found_existing = 0;
    
    if (f) {
        /* Read all lines */
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            /* Check if this line is for our ip_port */
            char *comma = strchr(line, ',');
            if (comma) {
                *comma = '\0';
                if (strcmp(line, ip_port) == 0) {
                    found_existing = 1;
                    /* Replace with new mapping */
                    snprintf(line, sizeof(line), "%s,%s\n", ip_port, node_id_value);
                    *comma = ',';
                } else {
                    *comma = ',';
                }
            }
            
            /* Store line */
            lines = realloc(lines, sizeof(char*) * (line_count + 1));
            lines[line_count] = strdup(line);
            line_count++;
        }
        fclose(f);
    }
    
    /* Write back all lines */
    f = fopen(filepath, "w");
    if (!f) {
        ValkeyModule_Log(ctx, "warning", "Failed to open %s for writing: %s", 
                       filepath, strerror(errno));
        /* Free allocated memory */
        for (int i = 0; i < line_count; i++) {
            free(lines[i]);
        }
        free(lines);
        return 0;
    }
    
    for (int i = 0; i < line_count; i++) {
        fputs(lines[i], f);
        free(lines[i]);
    }
    free(lines);
    
    /* If not found, append new mapping */
    if (!found_existing) {
        fprintf(f, "%s,%s\n", ip_port, node_id_value);
    }
    
    fclose(f);
    ValkeyModule_Log(ctx, "debug", "Saved mapping: %s -> %s", ip_port, node_id_value);
    return 1;
}

/* Helper function to get the announced IP:port for this node
 * Returns a dynamically allocated string that must be freed by the caller.
 * Format: "ip:port"
 * 
 * In cluster mode, respects cluster-announce-ip and cluster-announce-port
 * In standalone mode, respects replica-announce-ip and replica-announce-port
 * Falls back to actual bind address if announce addresses are not set
 */
static char *get_announced_address(ValkeyModuleCtx *ctx) {
    char *result = NULL;
    
    if (is_cluster_enabled) {
        /* Cluster mode: use ValkeyModule_GetClusterNodeInfo to get announced address */
        const char *my_id = ValkeyModule_GetMyClusterID();
        if (my_id) {
            char ip[256] = {0};
            int port = 0;
            
            /* Get this node's cluster info (includes announced IP and port) */
            if (ValkeyModule_GetClusterNodeInfo(ctx, my_id, ip, NULL, &port, NULL) == VALKEYMODULE_OK) {
                /* Allocate and format result */
                result = ValkeyModule_Alloc(strlen(ip) + 16);  /* IP + ":" + port + null */
                snprintf(result, strlen(ip) + 16, "%s:%d", ip, port);
                ValkeyModule_Log(ctx, "notice", "Cluster mode: announced address = %s", result);
            } else {
                ValkeyModule_Log(ctx, "warning", "Failed to get cluster node info");
            }
        } else {
            ValkeyModule_Log(ctx, "warning", "Failed to get cluster ID");
        }
    } else {
        /* Standalone mode: use ValkeyModule_GetServerInfo */
        ValkeyModuleServerInfoData *info = ValkeyModule_GetServerInfo(ctx, "server");
        if (info) {
            /* Get TCP port from server info */
            int err = 0;
            long long port = ValkeyModule_ServerInfoGetFieldSigned(info, "tcp_port", &err);
            
            if (err == VALKEYMODULE_OK && port > 0) {
                /* Try to get announced IP from replication section */
                ValkeyModule_FreeServerInfo(ctx, info);
                info = ValkeyModule_GetServerInfo(ctx, "replication");
                
                const char *announced_ip = NULL;
                if (info) {
                    /* Try to get replica-announce-ip if configured */
                    announced_ip = ValkeyModule_ServerInfoGetFieldC(info, "replica_announce_ip");
                }
                
                /* If no announced IP, use localhost as fallback */
                if (!announced_ip || announced_ip[0] == '\0') {
                    announced_ip = "127.0.0.1";
                }
                
                /* Allocate and format result */
                result = ValkeyModule_Alloc(strlen(announced_ip) + 32);
                snprintf(result, strlen(announced_ip) + 32, "%s:%lld", announced_ip, port);
                ValkeyModule_Log(ctx, "notice", "Standalone mode: announced address = %s", result);
                
                if (info) {
                    ValkeyModule_FreeServerInfo(ctx, info);
                }
            } else {
                ValkeyModule_Log(ctx, "warning", "Failed to get tcp_port from server info");
                if (info) {
                    ValkeyModule_FreeServerInfo(ctx, info);
                }
            }
        } else {
            ValkeyModule_Log(ctx, "warning", "Failed to get server info");
        }
    }
    
    return result;
}

/* Find node_id by ip:port address */
static const char *find_node_id_by_address(ValkeyModuleCtx *ctx, const char *ip_port) {
    if (!ip_port || strlen(ip_port) == 0) {
        /* Empty string means self */
        return node_id;
    }
    
    ValkeyModuleString *key = ValkeyModule_CreateString(ctx, ip_port, strlen(ip_port));
    ValkeyModuleString *val = ValkeyModule_DictGet(node_mapping_dict, key, NULL);
    
    if (val) {
        size_t len;
        const char *result = ValkeyModule_StringPtrLen(val, &len);
        return result;
    }
    
    return NULL;
}

/* Get backup_id for loading from a specific address.
 * Constructs a v0-format backup_id that can be used to load
 * from the node at the given address.
 *
 * Format: v0:<node_id>:<slot>:<timestamp>
 * - v0: version 0 format
 * - node_id: the node's identifier
 * - slot: -1 (all slots for standalone mode)
 * - timestamp: 0 (find most recent)
 */
static const char *get_backup_id(ValkeyModuleCtx *ctx, const char *address) {
    const char *node_id = find_node_id_by_address(ctx, address);
    if (!node_id) {
        return NULL;
    }
    
    /* Construct v0-format backup_id: v0:<node_id>:-1:0
     * -1 = all slots (standalone mode), 0 = find most recent timestamp */
    static char backup_id_buf[256];
    snprintf(backup_id_buf, sizeof(backup_id_buf), "v0:%s:-1:0", node_id);
    
    ValkeyModule_Log(ctx, "notice", "get_backup_id: address=%s -> backup_id=%s",
                     address, backup_id_buf);
    
    return backup_id_buf;
}
/* Test command for backup ID parsing */
int TestBackupIdCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_ReplyWithError(ctx, "Usage: extdata.testbackupid <backup_id>");
        return VALKEYMODULE_OK;
    }
    
    int slot;
    long long timestamp;
    char parsed_node_id[NODE_ID_MAX_LEN];
    char options[OPTIONS_MAX_LEN];
    
    int result = parseBackupIdV0(ctx, argv[1],
                                  parsed_node_id, sizeof(parsed_node_id),
                                  &slot,
                                  &timestamp,
                                  options, sizeof(options));
    
    if (result == EXTERNAL_SUCCESS) {
        ValkeyModule_ReplyWithArray(ctx, 8);
        ValkeyModule_ReplyWithSimpleString(ctx, "node_id");
        ValkeyModule_ReplyWithSimpleString(ctx, parsed_node_id);
        ValkeyModule_ReplyWithSimpleString(ctx, "slot");
        ValkeyModule_ReplyWithLongLong(ctx, slot);
        ValkeyModule_ReplyWithSimpleString(ctx, "timestamp");
        ValkeyModule_ReplyWithLongLong(ctx, timestamp);
        ValkeyModule_ReplyWithSimpleString(ctx, "options");
        ValkeyModule_ReplyWithSimpleString(ctx, options[0] ? options : "");
    } else {
        ValkeyModule_ReplyWithError(ctx, "Invalid backup_id format");
    }
    
    return VALKEYMODULE_OK;
}

/* Module initialization */

/* Module initialization */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {   
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    
    if (ValkeyModule_Init(ctx, module_name, 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModule_Log(ctx, "warning", "=== ValkeyModule_Init SUCCESS for module: %s ===", module_name);

    // Initialize node mapping dictionary
    // Step 1: Initialize node mapping dictionary
    node_mapping_dict = ValkeyModule_CreateDict(ctx);

    // Step 2: Get the announced address for this node
    char *server_address = get_announced_address(ctx);
    if (!server_address) {
        ValkeyModule_Log(ctx, "warning", "Failed to get announced address");
        return VALKEYMODULE_ERR;
    }
    
    ValkeyModule_Log(ctx, "notice", "Announced address: %s", server_address);
    
    // Step 3: Load existing mappings from file
    load_node_mappings(ctx);
    
    // Step 4: Read node_id from Valkey configuration
    ValkeyModuleString *config_value = ValkeyModule_GetConfigValue(ctx, "ext-data-id");
    if (!config_value) {
        ValkeyModule_Log(ctx, "warning", "Failed to read ext-data-id from config");
        return VALKEYMODULE_ERR;
    }
    
    size_t len;
    const char *config_node_id = ValkeyModule_StringPtrLen(config_value, &len);
    strncpy(node_id, config_node_id, sizeof(node_id) - 1);
    node_id[sizeof(node_id) - 1] = '\0';
    ValkeyModule_FreeString(ctx, config_value);
    
    ValkeyModule_Log(ctx, "notice", "Using node_id from config: %s", node_id);
    
    // Step 5: Update mapping with self entry
    ValkeyModuleString *key = ValkeyModule_CreateString(ctx, server_address, strlen(server_address));
    ValkeyModuleString *val = ValkeyModule_CreateString(ctx, node_id, strlen(node_id));
    ValkeyModule_DictSet(node_mapping_dict, key, val);
    save_node_mapping(ctx, server_address, node_id);
    
    ValkeyModule_Log(ctx, "notice", "Module initialized: node_id=%s address=%s",
                        node_id, server_address);
    
    /* Free the allocated server_address */
    ValkeyModule_Free(server_address);

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

    // Get cluster mode flag
    int flags = ValkeyModule_GetContextFlags(ctx);
    is_cluster_enabled = (flags & VALKEYMODULE_CTX_FLAGS_CLUSTER) ? 1 : 0;
    ValkeyModule_Log(ctx, "notice", "Module %s cluster mode: %s", module_name,
                     is_cluster_enabled ? "enabled" : "disabled");

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
        .get_backup_id = get_backup_id,
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
        storage_slot_pool[i] = ValkeyModule_CreateDict(NULL);
        filter_slot_pool[i] = ValkeyModule_CreateDict(NULL);
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
    
    /* Register module commands for slot debugging */
    if (ValkeyModule_CreateCommand(ctx, "helloextdata1.storage_getslot",
                                   StorageGetSlotCommand, "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "=== ValkeyModule_CreateCommand storage_getslot FAILED for module: %s ===", module_name);
        return VALKEYMODULE_ERR;
    }
    
    if (ValkeyModule_CreateCommand(ctx, "helloextdata1.filter_getslot",
                                   FilterGetSlotCommand, "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "=== ValkeyModule_CreateCommand filter_getslot FAILED for module: %s ===", module_name);
        return VALKEYMODULE_ERR;
    }
    
    if (ValkeyModule_CreateCommand(ctx, "extdata.testbackupid", TestBackupIdCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    
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
        ValkeyModule_FreeDict(NULL, storage_slot_pool[i]);
        ValkeyModule_FreeDict(NULL, filter_slot_pool[i]);
    }

    return VALKEYMODULE_OK;
}
