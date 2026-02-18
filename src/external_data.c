#include "external_data.h"

#include "bio.h"
#include "dict.h"
#include "sds.h"
#include "server.h"
#include "cluster.h"
#include "valkeymodule.h"
#include "zmalloc.h"
#include <assert.h>
#include <stdlib.h>
#include <strings.h>
#include "module.h"
#include <stdatomic.h>
#include <time.h>

/* Buffer size constants */
/* Maximum length for primary address string (IP:port format, e.g., "192.168.1.1:6379") */
#define PRIMARY_ADDRESS_BUFFER_SIZE 256

/* Maximum length for database name string (format: "db" + up to 10 digits + null terminator) */
#define DB_NAME_MAX_LEN 14

/* Time-related constants */
/* Sleep interval in milliseconds when polling for external data load completion */
#define LOAD_CHECK_SLEEP_MS 100

/* Conversion factor from milliseconds to microseconds (used with usleep) */
#define MICROSECONDS_PER_MILLISECOND 1000

/* Forward declarations */
static void moduleStatsDispose(void *obj);
static void externalDataAutoInitFromModule(externalDataModule *module);
static int tryAutoInitFromModuleState(externalDataModule *module);
static void setupModuleCtx(externalDataModuleInstance *mi);
static void queueDeferredInit(const char *db_name, const char *reason);
static int isDatabaseAlreadyInitialized(const char *db_name);

/* Helper function to check if this node is a replica (standalone or cluster mode) */
static inline int is_replica(void) {
    int result = 0;
    if (server.primary_host) {
        /* Standalone replica */
        result = 1;
    } else if (server.cluster_enabled) {
        /* Cluster mode - check if this node is a replica */
        result = clusterNodeIsReplica(getMyClusterNode());
    }
    return result;
}

/* Helper function to get primary address for replica nodes
 * Returns a newly allocated string that must be freed by caller, or NULL if not a replica */
static char *get_primary_address(void) {
    static char addr_buf[PRIMARY_ADDRESS_BUFFER_SIZE];
    
    if (server.primary_host) {
        /* Standalone replica - use server.primary_host and server.primary_port */
        snprintf(addr_buf, sizeof(addr_buf), "%s:%d",
                 server.primary_host, server.primary_port);
        return addr_buf;
    }
    
    if (server.cluster_enabled && clusterNodeIsReplica(getMyClusterNode())) {
        /* Cluster replica - get primary node info */
        clusterNode *myself = getMyClusterNode();
        clusterNode *primary = clusterNodeGetPrimary(myself);
        if (primary) {
            const char *ip = clusterNodeIp(primary, NULL);
            int port = clusterNodeClientPort(primary, 0, NULL);
            snprintf(addr_buf, sizeof(addr_buf), "%s:%d", ip, port);
            return addr_buf;
        }
    }
    
    return NULL;
}

/* Creates a string object from the configured node ID.
 * Returns NULL if node ID is not configured. */
static inline robj *createNodeIdStringObject(void) {
    if (!server.ext_data_id || server.ext_data_id[0] == '\0') {
        return NULL;
    }
    return createStringObject(server.ext_data_id, strlen(server.ext_data_id));
}

/* Helper function to calculate the slot for a given key.
 * Returns:
 *   -1 for standalone mode (no clustering)
 *   0-16383 for cluster mode (calculated using CRC16 hash)
 */
static int externalDataGetKeySlot(robj *key) {
    if (!server.cluster_enabled) {
        return EXTERNAL_ALL_SLOTS;  /* Standalone mode - no slot concept */
    }
    /* Cluster mode - calculate slot using key hash */
    return keyHashSlot(objectGetVal(key), sdslen(objectGetVal(key)));
}

struct externalDataCtx {
    dict *modules;       /* Module name -> Module object */
    dict *dbdata;        /* Database name -> Database data */
    size_t cache_memory; /* Overhead memory (structs, dictionaries, ..) used by all the modules */
    dict *modules_stats; /* Per module statistics */
    ExternalDataDeferredCtx *deferred_ctx; /* Deferred initialization context */
};

typedef struct externalDataModule {
    sds name;                       /* Name of the module */
    ValkeyModule *module;           /* The module that implements the storage and filter */
    atomic_uint used_count;         /* Counter for the module usage */
    storageMethods storage_methods; /* Storage methods */
    filterMethods filter_methods;   /* Filter methods */
} externalDataModule;

typedef struct externalDataModuleInstance {
    externalDataModule *external_module; /* Module struct */
    storageCtx *storage_ctx;             /* Storage specific context */
    filterCtx *filter_ctx;               /* Filter specific context */
    ValkeyModuleCtx *module_ctx;         /* Cache of the module context object */
} externalDataModuleInstance;

typedef struct externalDbData {
    externalDataModuleInstance *module_instance; /* Module instance used for a certain db */
} externalDbData;

typedef struct moduleStats {
    size_t n_dbs;
} moduleStats;

/* External data Ctx. */
static externalDataCtx *curr_external_data_ctx = NULL;

static uint64_t dictStrCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char *)key, strlen((char *)key));
}

dictType moduleDictType = {
    dictStrCaseHash,       /* hash function */
    NULL,                  /* key dup */
    dictSdsKeyCaseCompare, /* key compare */
    NULL,                  /* key destructor */
    NULL,                  /* val destructor */
    NULL                   /* allow to expand */
};

dictType dbdataDictType = {
    dictStrCaseHash,       /* hash function */
    NULL,                  /* key dup */
    dictSdsKeyCaseCompare, /* key compare */
    dictSdsDestructor,     /* key destructor */
    NULL,                  /* val destructor */
    NULL};

dictType moduleStatsDictType = {
    dictSdsCaseHash,       /* hash function */
    dictSdsDup,            /* key dup */
    dictSdsKeyCaseCompare, /* key compare */
    dictSdsDestructor,     /* key destructor */
    moduleStatsDispose,    /* val destructor */
    NULL                   /* allow to expand */
};

/* Create a new external data ctx */
externalDataCtx *externalDataCtxCreate(void) {
    externalDataCtx *ret = zmalloc(sizeof(externalDataCtx));
    ret->modules = dictCreate(&moduleDictType);
    ret->dbdata = dictCreate(&dbdataDictType);
    ret->modules_stats = dictCreate(&moduleStatsDictType);
    ret->cache_memory = 0;
    
    /* Initialize deferred initialization context */
    ret->deferred_ctx = zmalloc(sizeof(ExternalDataDeferredCtx));
    ret->deferred_ctx->queue = listCreate();
    ret->deferred_ctx->max_defer_ms = server.ext_data_defer_max_ms;
    ret->deferred_ctx->queued = 0;
    ret->deferred_ctx->retried = 0;
    ret->deferred_ctx->succeeded = 0;
    ret->deferred_ctx->expired = 0;
    
    return ret;
}

/* Dispose stats memory */
static void moduleStatsDispose(void *obj) {
    moduleStats *stats = obj;
    zfree(stats);
}
/* Helper function to get backup_id for a slot from a given address.
 * Returns a newly allocated sds that must be freed by caller, or NULL on failure.
 * This function finds the appropriate external module and constructs a backup_id
 * based on the provided address and slot number. */
sds externalDataGetBackupId(sds address, int slot) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) {
        return NULL;
    }
    
    /* Find the first database with a module instance to use for backup_id generation */
    dictIterator *di = dictGetIterator(ctx->dbdata);
    dictEntry *de;
    externalDataModuleInstance *mi = NULL;
    
    while ((de = dictNext(di)) != NULL) {
        externalDbData *dbData = dictGetVal(de);
        if (dbData && dbData->module_instance) {
            mi = dbData->module_instance;
            break;
        }
    }
    dictReleaseIterator(di);
    
    if (!mi || !mi->external_module || !mi->external_module->storage_methods.get_backup_id) {
        return NULL;
    }
    
    const char *backup_id = mi->external_module->storage_methods.get_backup_id(
        mi->module_ctx, address, slot);
    
    return backup_id ? sdsnew(backup_id) : NULL;
}



/* Process external data loading for full sync.
 * This function handles both async and sync modes of external data loading.
 * It should be called after RDB loading is complete and after external data module loading. */
void processExternalDataLoadForFullSync(void) {
    /* Load external data for full sync if enabled
     * We check if external data context exists, which means modules have been loaded */
    if (!isExtDataOn()) {
        return;
    }

    /* Process any deferred inits first - handles previously queued attempts */
    processDeferredInits();

    externalDataCtx *ctx = getCurrentExternalDataCtx();
    int db_count = ctx ? dictSize(ctx->dbdata) : 0;
    int is_repl = is_replica();
    
    serverLog(LL_NOTICE, "processExternalDataLoadForFullSync: db_count=%d, is_replica=%d", db_count, is_repl);
    
    /* If no databases are initialized yet and we're a replica, try auto-initialization
        * This handles cluster replicas where topology isn't established during module load */
    if (ctx && db_count == 0 && is_repl) {
        serverLog(LL_NOTICE, "No databases initialized, attempting auto-initialization from modules");
        
        if (server.ext_data_async_load) {
            /* Async mode: Try once, queue for deferred retry if it fails */
            dictIterator *di = dictGetIterator(ctx->modules);
            dictEntry *de;
            while ((de = dictNext(di))) {
                externalDataModule *module = dictGetVal(de);
                serverLog(LL_NOTICE, "Trying auto-init for module: %s (async mode)", module->name);
                int initialized = tryAutoInitFromModuleState(module);
                serverLog(LL_NOTICE, "Auto-init for module %s: %s", module->name, initialized ? "SUCCESS" : "FAILED");
            }
            dictReleaseIterator(di);
            
        } else {
            /* Sync mode: Retry with exponential backoff until timeout */
            mstime_t start_time = mstime();
            mstime_t timeout_ms = ctx->deferred_ctx->max_defer_ms;
            int delay_ms = 100;
            // mstime_t next_check = start_time + 100; /* Check every 100ms initially */
            
            serverLog(LL_NOTICE, "Sync mode: Starting auto-init with timeout %lld ms", timeout_ms);
            
            while ((mstime() - start_time) < timeout_ms) {
                dictIterator *di = dictGetIterator(ctx->modules);
                dictEntry *de;
                int any_success = 0;
                
                while ((de = dictNext(di))) {
                    externalDataModule *module = dictGetVal(de);
                    if (tryAutoInitFromModuleState(module) == EXTERNAL_SUCCESS) {
                        any_success = 1;
                        serverLog(LL_NOTICE, "Auto-init for module %s succeeded (elapsed: %lld ms)",
                                 module->name, mstime() - start_time);
                    }
                }
                dictReleaseIterator(di);
                
                /* Check if init succeeded */
                db_count = dictSize(ctx->dbdata);
                if (db_count > 0 || any_success) {
                    serverLog(LL_NOTICE, "Sync mode auto-init completed successfully (elapsed: %lld ms)",
                             mstime() - start_time);
                    break;
                }
                
                /* Sleep with exponential backoff (capped at 1600ms) */
                usleep(delay_ms * 1000);
                delay_ms = (delay_ms < 1600) ? delay_ms * 2 : 1600;

                // /* Process events while waiting - allows serverCron and deferred queue processing */
                // while (mstime() < next_check) {
                //     processEventsWhileBlocked();
                // }
                
                // /* Exponential backoff for next check (100ms -> 200ms -> 400ms -> ... -> 1600ms) */
                // mstime_t delay = next_check - start_time;
                // delay = (delay < 1600) ? delay * 2 : 1600;
                // next_check = mstime() + delay;
            }
            
            if (db_count == 0) {
                serverLog(LL_WARNING, "Sync mode auto-init timed out after %lld ms",
                         mstime() - start_time);
            }
        }
        
        /* Re-check db_count after auto-init attempts */
        db_count = ctx ? dictSize(ctx->dbdata) : 0;
        serverLog(LL_NOTICE, "After auto-init: db_count=%d", db_count);
    }
    
    if (server.ext_data_async_load) {
        /* Async mode: Start load in background and proceed immediately */
        serverLog(LL_NOTICE, "Starting async external data load for full sync");
        if (externalDataLoadForFullSync() == EXTERNAL_SUCCESS) {
            serverLog(LL_NOTICE, "External data async load initiated");
        } else {
            serverLog(LL_WARNING, "Failed to initiate async external data load");
        }
    } else {
        /* Sync mode: Start load and wait for completion */
        serverLog(LL_NOTICE, "Starting sync external data load for full sync");
        if (externalDataLoadForFullSync() == EXTERNAL_SUCCESS) {
            /* Wait for load to complete */
            int sleep_ms = LOAD_CHECK_SLEEP_MS;
            int max_wait = server.ext_data_load_timeout_ms / sleep_ms; /* Convert ms to iterations */
            int waited = 0;
            while (waited < max_wait) {
                if (externalDataLoadCheckComplete() == C_OK) {
                    serverLog(LL_NOTICE, "External data loaded synchronously for full sync");
                    break;
                }
                usleep(sleep_ms * MICROSECONDS_PER_MILLISECOND);
                waited++;
            }
            if (waited >= max_wait) {
                serverLog(LL_WARNING, "Timeout waiting for external data load to complete");
            }
        } else {
            serverLog(LL_WARNING, "Failed to initiate external data load for full sync");
        }
    }
}

/* Registers a new external module with both storage and filter methods.
 *
 * - `name`: the name of the external module.
 * - `module`: the module that implements the storage and filter.
 * - `storage_methods`: storage callback functions.
 * - `filter_methods`: filter callback functions.
 *
 * Returns C_ERR in case of an error during registration.
 */
int externalDataModuleRegister(const char *name,
                               ValkeyModule *module,
                               storageMethods *storage_methods,
                               filterMethods *filter_methods) {
    serverLog(LL_NOTICE, "=== externalDataModuleRegister called for module: %s ===", name);
    
    if (!isExtDataOn()) {
        serverLog(LL_NOTICE, "externalDataModuleRegister: external data is OFF, returning C_ERR");
        return C_ERR;
    }
    assert(getCurrentExternalDataCtx() != NULL);

    sds name_sds = sdsnew(name);
    if (dictFind(curr_external_data_ctx->modules, name_sds)) {
        serverLog(LL_WARNING, "External module '%s' is already registered in the server", name_sds);
        sdsfree(name_sds);
        return C_ERR;
    }

    serverLog(LL_NOTICE, "Registering external module: %s", name_sds);

    externalDataModule *m = zmalloc(sizeof(*m));
    *m = (externalDataModule){
        .name = name_sds,
        .module = module,
        .storage_methods = *storage_methods,
        .filter_methods = *filter_methods,
    };

    dictAdd(curr_external_data_ctx->modules, name_sds, m);
    serverLog(LL_NOTICE, "Module %s added to modules dictionary", name_sds);

    /* Auto init databases data from module, if there are any */
    serverLog(LL_NOTICE, "Calling externalDataAutoInitFromModule for module: %s", name_sds);
    externalDataAutoInitFromModule(m);
    serverLog(LL_NOTICE, "externalDataAutoInitFromModule returned for module: %s", name_sds);

    return C_OK;
}

/* Removes an external module.
 *
 * - `module_name`: name of the module to remove
 */
int externalDataModuleUnregister(const char *module_name) {
    dictEntry *entry = dictFind(curr_external_data_ctx->modules, module_name);
    if (entry == NULL) {
        serverLog(LL_WARNING, "There's no module registered with name %s", module_name);
        return C_ERR;
    }

    externalDataModule *m = dictGetVal(entry);
    if (m->used_count > 0) {
        serverLog(LL_WARNING, "It's impossible to remove used module %s, drop it from all dbs first: %d", module_name, m->used_count);
        return C_ERR;
    }

    dictDelete(curr_external_data_ctx->modules, module_name);
    sdsfree(m->name);
    zfree(m);

    return C_OK;
}

int qsortCompareNames(const void *n1, const void *n2) {
    return strcmp(*(char **)n1, (*(char **)n2));
}

/*
 * EXTERNAL_DATA LOADED
 *
 * Return general information about loaded modules:
 * * Module name
 *
 */
void externalDataLoadedCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, EXTDATAOFFERRMSG);
        return;
    }
    assert(getCurrentExternalDataCtx() != NULL);

    int size = dictSize(curr_external_data_ctx->modules);
    addReplyArrayLen(c, size);
    if (size == 0) {
        return;
    }

    sds module_names[size];
    int num_modules = 0;
    dictIterator *modules_iter = dictGetIterator(curr_external_data_ctx->modules);
    dictEntry *module_entry = NULL;
    while ((module_entry = dictNext(modules_iter))) {
        externalDataModule *es = dictGetVal(module_entry);
        module_names[num_modules++] = es->name;
    }
    dictReleaseIterator(modules_iter);

    qsort(module_names, num_modules, sizeof(sds), qsortCompareNames);
    for (int i = 0; i < num_modules; i++) {
        addReplyBulkCString(c, module_names[i]);
    }
}

/*
 * EXTERNAL_DATA STATS
 *
 * Return general information about loaded modules:
 * * Module name
 * * Databases list
 *
 */
void externalDataStatsCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, EXTDATAOFFERRMSG);
        return;
    }
    assert(getCurrentExternalDataCtx() != NULL);

    /* Parse subcommand */
    robj *subcommand = c->argv[2];
    
    /* Handle NODEID subcommand */
    if (!strcasecmp(objectGetVal(subcommand), "nodeid")) {
        /* Return ext-data-id from server config */
        if (!server.ext_data_id || server.ext_data_id[0] == '\0') {
            addReplyError(c, "ERR ext-data-id not configured");
            return;
        }
        
        /* Return node_id as simple string */
        addReplyBulkCString(c, server.ext_data_id);
        return;
    }
    
    /* Handle DBS subcommand (default behavior) */
    int size = dictSize(curr_external_data_ctx->dbdata);
    
    addReplyArrayLen(c, size);
    if (size == 0) {
        return;
    }

    sds lines[size];
    int num_lines = 0;
    dictIterator *dbdata_iter = dictGetIterator(curr_external_data_ctx->dbdata);
    dictEntry *dbdata_entry = NULL;
    while ((dbdata_entry = dictNext(dbdata_iter))) {
        sds name = dictGetKey(dbdata_entry);
        externalDbData *ed = dictGetVal(dbdata_entry);

        char line[sizeof(name) + sizeof(ed->module_instance->external_module->name) + 2];
        snprintf(line, sizeof(line), "%s:%s", name, ed->module_instance->external_module->name);
        lines[num_lines++] = sdsnew(line);
    }
    dictReleaseIterator(dbdata_iter);

    qsort(lines, num_lines, sizeof(sds), qsortCompareNames);
    for (int i = 0; i < num_lines; i++) {
        addReplyBulkCString(c, lines[i]);
        sdsfree(lines[i]);
    }
    return;
}

int checkDbNum(client *c, const sds db_name) {
    int db_num;
    if (sscanf(db_name, "db%d", &db_num) != 1) {
        addReplyErrorFormat(c, "failed to parse db number from %s, expect db0, db10, etc.", db_name);
        return -1;
    }
    if (db_num >= server.dbnum) {
        addReplyErrorFormat(c, "db number %d exceeds used on server 0-%d", db_num, server.dbnum - 1);
        return -1;
    }
    return db_num;
}

sds getDBName(int db_num) {
    char db_name[DB_NAME_MAX_LEN];
    snprintf(db_name, sizeof(db_name), "db%d", db_num);
    return sdsnew(db_name);
}

/* Helper function to create and initialize external data structures for a database
 * Returns the created externalDbData on success, NULL on failure
 * On failure, all allocated resources are cleaned up */
static externalDbData *createExternalDbData(externalDataModule *module) {
    module->used_count++;

    storageCtx *storage_ctx = zmalloc(sizeof(*storage_ctx));
    *storage_ctx = (storageCtx){
        .state = VMES_STATE_READY,
        .ext_data_timeout = server.ext_data_timeout,
        .ext_data_dump_status = VMES_DUMP_STATE_NONE,
        .ext_data_dump_backup_id = NULL,
        .ext_data_load_status = VMES_LOAD_STATE_NONE,
    };
    filterCtx *filter_ctx = zmalloc(sizeof(*filter_ctx));
    *filter_ctx = (filterCtx){
        .state = VMEF_STATE_READY,
        .ext_data_timeout = server.ext_data_timeout,
        .ext_data_dump_status = VMEF_DUMP_STATE_NONE,
        .ext_data_dump_backup_id = NULL,
        .ext_data_load_status = VMEF_LOAD_STATE_NONE,
    };

    externalDataModuleInstance *module_instance = zmalloc(sizeof(*module_instance));
    *module_instance = (externalDataModuleInstance){
        .external_module = module,
        .storage_ctx = storage_ctx,
        .filter_ctx = filter_ctx,
        .module_ctx = moduleAllocateContext(),
    };

    externalDbData *e = zmalloc(sizeof(*e));
    *e = (externalDbData){
        .module_instance = module_instance,
    };

    return e;
}

/* Helper function to cleanup externalDbData and all its allocated resources */
static void freeExternalDbData(externalDbData *e) {
    if (!e) return;
    
    if (e->module_instance) {
        if (e->module_instance->module_ctx) {
            moduleFreeContext(e->module_instance->module_ctx);
            zfree(e->module_instance->module_ctx);
        }
        if (e->module_instance->storage_ctx) {
            zfree(e->module_instance->storage_ctx);
        }
        if (e->module_instance->filter_ctx) {
            zfree(e->module_instance->filter_ctx);
        }
        zfree(e->module_instance);
    }
    zfree(e);
}

/* Helper function to initialize databases from module state data
 * Returns C_OK on success, C_ERR on failure */
static int initializeDatabasesFromState(externalDataCtx *ctx, externalDataModule *module, int *db_numbers, size_t num_dbs) {
    const char *module_name = module->name;
    
    for (size_t i = 0; i < num_dbs; i++) {
        int db_num = db_numbers[i];
        sds db_name = getDBName(db_num);
        
        /* Check if already initialized */
        if (!dictFind(ctx->dbdata, db_name)) {
            /* Initialize the database */
            externalDbData *e = createExternalDbData(module);
            if (!e) {
                sdsfree(db_name);
                continue;
            }
        
            serverLog(LL_NOTICE, "DEBUG: initializeDatabasesFromState - about to dictAdd: ctx=%p, ctx->dbdata=%p, db_name=%s",
                      (void*)ctx, (void*)ctx->dbdata, db_name);
            if (dictAdd(ctx->dbdata, db_name, e) == DICT_OK) {
                serverLog(LL_NOTICE, "Auto-initialized %s with module %s from state (ctx=%p, dictSize now=%lu)",
                          db_name, module_name, (void*)ctx, (unsigned long)dictSize(ctx->dbdata));
            } else {
                /* Cleanup on failure */
                freeExternalDbData(e);
                sdsfree(db_name);
            }
        } else {
            sdsfree(db_name);
        }
    }
    
    return C_OK;
}

/* Helper function to get external database data by name
 * Returns the externalDbData if found, NULL otherwise */
void *externalDataGetDatabase(const char *db_name) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) return NULL;
    
    dictEntry *db = dictFind(ctx->dbdata, db_name);
    if (!db) return NULL;
    
    return dictGetVal(db);
}

/* Check if database is already initialized or queued */
static int isDatabaseAlreadyInitialized(const char *db_name) {
    /* Check if already initialized */
    if (externalDataGetDatabase(db_name) != NULL) return 1;
    
    /* Check if already in deferred queue */
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (ctx && ctx->deferred_ctx) {
        listIter li;
        listNode *ln;
        listRewind(ctx->deferred_ctx->queue, &li);
        while ((ln = listNext(&li))) {
            DeferredInit *entry = listNodeValue(ln);
            if (strcmp(entry->db_name, db_name) == 0) return 1;
        }
    }
    
    return 0;
}

/* Queue a failed initialization for deferred retry */
static void queueDeferredInit(const char *db_name, const char *reason) {
    /* Check if already queued */
    if (isDatabaseAlreadyInitialized(db_name)) return;
    
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx || !ctx->deferred_ctx) return;
    
    DeferredInit *entry = zmalloc(sizeof(*entry));
    entry->db_name = sdsnew(db_name);
    entry->first_attempt = commandTimeSnapshot();
    entry->next_retry = entry->first_attempt + 100; /* First retry after 100ms */
    entry->attempts = 1;
    
    listAddNodeTail(ctx->deferred_ctx->queue, entry);
    ctx->deferred_ctx->queued++;
    
    serverLog(LL_NOTICE, "Deferred external data init for '%s': %s (will retry)",
              db_name, reason);
}

/* Process deferred initialization queue with exponential backoff */
void processDeferredInits(void) {
    externalDataCtx *ext_ctx = getCurrentExternalDataCtx();
    if (!ext_ctx || !ext_ctx->deferred_ctx) return;
    
    ExternalDataDeferredCtx *ctx = ext_ctx->deferred_ctx;
    mstime_t now = commandTimeSnapshot();
    listNode *node = listFirst(ctx->queue);
    
    serverLog(LL_DEBUG, "Processing deferred init queue (%lu entries)",
              listLength(ctx->queue));
    
    while (node) {
        DeferredInit *entry = listNodeValue(node);
        listNode *next = listNextNode(node);
        
        /* Check if expired */
        if ((now - entry->first_attempt) > ctx->max_defer_ms) {
            serverLog(LL_WARNING,
                      "Giving up on deferred init for '%s' after %lld ms",
                      entry->db_name, now - entry->first_attempt);
            ctx->expired++;
            sdsfree(entry->db_name);
            zfree(entry);
            listDelNode(ctx->queue, node);
            node = next;
            continue;
        }
        
        /* Check if it's time to retry */
        if (now >= entry->next_retry) {
            /* Try auto-init again */
            ctx->retried++;
            entry->attempts++;
            
            if (tryAutoInitFromModuleState(NULL) == C_OK) {
                serverLog(LL_NOTICE,
                          "Deferred init succeeded for '%s' on attempt %d",
                          entry->db_name, entry->attempts);
                ctx->succeeded++;
                sdsfree(entry->db_name);
                zfree(entry);
                listDelNode(ctx->queue, node);
            } else {
                /* Schedule next retry with exponential backoff */
                mstime_t backoff = 100 * (1 << (entry->attempts - 1)); /* 100, 200, 400, 800, ... */
                if (backoff > 5000) backoff = 5000; /* Cap at 5 seconds */
                entry->next_retry = now + backoff;
            }
        }
        
        node = next;
    }
}

/*
 * EXTERNAL_DATA INIT db m
 *
 * Init module for a certain db
 *
 */
void externalDataInitCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, EXTDATAOFFERRMSG);
        return;
    }
    assert(getCurrentExternalDataCtx() != NULL);

    sds db_name = objectGetVal(c->argv[2]);
    if (checkDbNum(c, db_name) < 0) {
        return;
    }

    if (dictFind(curr_external_data_ctx->dbdata, db_name)) {
        serverLog(LL_DEBUG, "EXTERNAL_DATA INIT: %s is already initialized", db_name);
        addReplyErrorFormat(c, "%s is already initialized", db_name);
        return;
    }

    sds module_name = objectGetVal(c->argv[3]);
    serverLog(LL_DEBUG, "EXTERNAL_DATA INIT: Initializing %s with module %s (loading=%d)",
              db_name, module_name, server.loading);
    externalDataModule *module = dictFetchValue(curr_external_data_ctx->modules, module_name);
    if (!module) {
        serverLog(LL_WARNING, "EXTERNAL_DATA INIT: module %s is not loaded", module_name);
        addReplyErrorFormat(c, "module %s is not loaded", module_name);
        return;
    }
    sds db_name_sds = sdsnew(db_name);

    externalDbData *e = createExternalDbData(module);
    if (!e) {
        sdsfree(db_name_sds);
        addReplyError(c, "Failed to create external data structures");
        return;
    }

    /* Add to dictionary - if this fails, clean up all allocated resources */
    if (dictAdd(curr_external_data_ctx->dbdata, db_name_sds, e) != DICT_OK) {
        /* Dictionary add failed, clean up everything we allocated */
        freeExternalDbData(e);
        sdsfree(db_name_sds);
        addReplyErrorFormat(c, "Failed to add database %s to external data", db_name);
        return;
    }

    addReply(c, shared.ok);
    return;
}

/*
 * EXTERNAL_DATA DROP db
 *
 * Drops external data for a certain db
 *
 */
void externalDataDropCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, EXTDATAOFFERRMSG);
        return;
    }
    assert(getCurrentExternalDataCtx() != NULL);

    sds db_name = objectGetVal(c->argv[2]);
    if (checkDbNum(c, db_name) < 0) {
        return;
    }

    dictEntry *dbEntry = dictFind(curr_external_data_ctx->dbdata, db_name);
    if (!dbEntry) {
        addReplyErrorFormat(c, "%s is not initialized", db_name);
        return;
    }

    if (c->argc <= 3 || strcasecmp(objectGetVal(c->argv[3]), "force")) {
        addReplyErrorFormat(c, "Leads to persistent storage data loss for %s, use FORCE if sure", db_name);
        return;
    }

    externalDbData *dbData = dictGetVal(dbEntry);

    /* Free backup_id sds strings if allocated */
    if (dbData->module_instance->storage_ctx->ext_data_dump_backup_id) {
        sdsfree(dbData->module_instance->storage_ctx->ext_data_dump_backup_id);
    }
    if (dbData->module_instance->filter_ctx->ext_data_dump_backup_id) {
        sdsfree(dbData->module_instance->filter_ctx->ext_data_dump_backup_id);
    }

    zfree(dbData->module_instance->storage_ctx);
    zfree(dbData->module_instance->filter_ctx);
    dbData->module_instance->external_module->used_count--;
    /* Free the module context - this will flush any pending logs */
    if (dbData->module_instance->module_ctx != NULL) {
        moduleFreeContext(dbData->module_instance->module_ctx);
        zfree(dbData->module_instance->module_ctx);
    }
    zfree(dbData->module_instance);

    zfree(dbData);
    dictDelete(curr_external_data_ctx->dbdata, dictGetKey(dbEntry));

    addReply(c, shared.ok);
    return;
}

static void setupModuleCtx(externalDataModuleInstance *mi) {
    if (mi->external_module != NULL && mi->external_module->module != NULL) {
        serverAssert(mi->module_ctx != NULL);
        moduleExternalStorageInitContext(mi->module_ctx, mi->external_module->module);
    }
}

static void teardownModuleCtx(externalDataModuleInstance *mi) {
    if (mi->external_module != NULL && mi->external_module->module != NULL) {
        serverAssert(mi->module_ctx != NULL);
        moduleFreeContext(mi->module_ctx);
    }
}

int externalStorageCallSetFunc(externalDataModuleInstance *mi, int dbid, robj *key, robj *value) {
    setupModuleCtx(mi);

    // Validate parameters before calling module
    if (!key) {
        serverLog(LL_WARNING, "externalStorageCallSetFunc: NULL key parameter for db %d", dbid);
        teardownModuleCtx(mi);
        return EXTERNAL_ERROR;
    }
    if (!value) {
        serverLog(LL_WARNING, "externalStorageCallSetFunc: NULL value parameter for db %d", dbid);
        teardownModuleCtx(mi);
        return EXTERNAL_ERROR;
    }

    int slot = externalDataGetKeySlot(key);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, slot};
    int success = mi->external_module->storage_methods.set(mi->module_ctx, mi->storage_ctx, &key_ctx, value);

    teardownModuleCtx(mi);
    return success;
}

int externalStorageCallGetFunc(externalDataModuleInstance *si, int dbid, robj *key, void **found) {
    setupModuleCtx(si);

    // Validate parameters before calling module
    if (!key) {
        serverLog(LL_WARNING, "externalStorageCallGetFunc: NULL key parameter for db %d", dbid);
        teardownModuleCtx(si);
        return 0;
    }
    if (!found) {
        serverLog(LL_WARNING, "externalStorageCallGetFunc: NULL found parameter for db %d", dbid);
        teardownModuleCtx(si);
        return 0;
    }

    serverAssert(si->external_module != NULL && si->storage_ctx != NULL && si->module_ctx != NULL);
    int slot = externalDataGetKeySlot(key);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, slot};
    int exists = si->external_module->storage_methods.get(si->module_ctx, si->storage_ctx, &key_ctx, found);

    teardownModuleCtx(si);
    return exists;
}

int externalStorageCallDelFunc(externalDataModuleInstance *mi, int dbid, robj *key, robj **value) {
    setupModuleCtx(mi);

    // Validate parameters before calling module
    if (!key) {
        serverLog(LL_WARNING, "externalStorageCallDelFunc: NULL key parameter for db %d", dbid);
        teardownModuleCtx(mi);
        return 0;
    }
    if (!value) {
        serverLog(LL_WARNING, "externalStorageCallDelFunc: NULL value parameter for db %d", dbid);
        teardownModuleCtx(mi);
        return 0;
    }

    serverAssert(mi->external_module != NULL && mi->storage_ctx != NULL && mi->module_ctx != NULL);

    int slot = externalDataGetKeySlot(key);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, slot};
    int result = mi->external_module->storage_methods.del(mi->module_ctx, mi->storage_ctx, &key_ctx, value);

    teardownModuleCtx(mi);
    return result == EXTERNAL_SUCCESS;
}

int externalFilterCallSetFunc(externalDataModuleInstance *fi, int dbid, robj *key) {
    setupModuleCtx(fi);

    // Validate parameters before calling module
    if (!key) {
        serverLog(LL_WARNING, "externalFilterCallSetFunc: NULL key parameter for db %d", dbid);
        teardownModuleCtx(fi);
        return EXTERNAL_ERROR;
    }

    serverAssert(fi->external_module != NULL && fi->filter_ctx != NULL && fi->module_ctx != NULL);
    int slot = externalDataGetKeySlot(key);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, slot};
    int success = fi->external_module->filter_methods.set(fi->module_ctx, fi->filter_ctx, &key_ctx);

    teardownModuleCtx(fi);
    return success;
}

int externalFilterCallGetFunc(externalDataModuleInstance *fi, int dbid, robj *key) {
    setupModuleCtx(fi);

    // Validate parameters before calling module
    if (!key) {
        serverLog(LL_WARNING, "externalFilterCallGetFunc: NULL key parameter for db %d", dbid);
        teardownModuleCtx(fi);
        return 0;
    }

    serverAssert(fi->external_module != NULL && fi->filter_ctx != NULL && fi->module_ctx != NULL);
    int slot = externalDataGetKeySlot(key);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, slot};
    int exists = fi->external_module->filter_methods.get(fi->module_ctx, fi->filter_ctx, &key_ctx);

    teardownModuleCtx(fi);
    return exists;
}

int externalFilterCallDelFunc(externalDataModuleInstance *fi, int dbid, robj *key, robj **value) {
    setupModuleCtx(fi);

    // Validate parameters before calling module
    if (!key) {
        serverLog(LL_WARNING, "externalFilterCallDelFunc: NULL key parameter for db %d", dbid);
        teardownModuleCtx(fi);
        return 0;
    }
    if (!value) {
        serverLog(LL_WARNING, "externalFilterCallDelFunc: NULL value parameter for db %d", dbid);
        teardownModuleCtx(fi);
        return 0;
    }

    int slot = externalDataGetKeySlot(key);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, slot};
    int result = fi->external_module->filter_methods.del(fi->module_ctx, fi->filter_ctx, &key_ctx, value);

    teardownModuleCtx(fi);
    return result == EXTERNAL_SUCCESS;
}

int externalDataCallSetReadonlyFunc(externalDataModuleInstance *mi) {
    setupModuleCtx(mi);

    /* Set both storage and filter to readonly */
    int result = mi->external_module->storage_methods.set_readonly(mi->module_ctx, mi->storage_ctx);
    if (result != EXTERNAL_SUCCESS) {
        teardownModuleCtx(mi);
        return result;
    }
    result = mi->external_module->filter_methods.set_readonly(mi->module_ctx, mi->filter_ctx);
    if (result != EXTERNAL_SUCCESS) {
        teardownModuleCtx(mi);
        return result;
    }

    teardownModuleCtx(mi);

    mi->storage_ctx->state = VMES_STATE_READONLY;
    mi->filter_ctx->state = VMEF_STATE_READONLY;
    return EXTERNAL_SUCCESS;
}

int externalDataCallDropReadonlyFunc(externalDataModuleInstance *mi) {
    setupModuleCtx(mi);

    /* Drop readonly for both storage and filter */
    int result = mi->external_module->storage_methods.drop_readonly(mi->module_ctx, mi->storage_ctx);
    if (result != EXTERNAL_SUCCESS) {
        teardownModuleCtx(mi);
        return result;
    }
    result = mi->external_module->filter_methods.drop_readonly(mi->module_ctx, mi->filter_ctx);
    if (result != EXTERNAL_SUCCESS) {
        teardownModuleCtx(mi);
        return result;
    }

    teardownModuleCtx(mi);

    mi->storage_ctx->state = VMES_STATE_READY;
    mi->filter_ctx->state = VMEF_STATE_READY;
    unblockPostponedClients();
    return EXTERNAL_SUCCESS;
}

/* Call the dump callback function for a module instance
 * Dumps data for all databases at once
 * Returns EXTERNAL_SUCCESS on success, EXTERNAL_ERROR on failure */
int externalDataCallDumpFunc(externalDataModuleInstance *mi,
                             int slot,
                             long long timestamp,
                             ValkeyModuleString *target,
                             ValkeyModuleString **backup_id) {
    if (!mi || !mi->external_module || !mi->storage_ctx || !mi->module_ctx) {
        return EXTERNAL_ERROR;
    }

    /* Check if dump function is available */
    if (!mi->external_module->storage_methods.dump) {
        return EXTERNAL_ERROR;
    }

    setupModuleCtx(mi);

    /* Use ext-data-id from config if target is NULL */
    ValkeyModuleString *server_target = target;
    if (target == NULL) {
        /* Use configured ext-data-id */
        server_target = (ValkeyModuleString *)createNodeIdStringObject();
        
        /* Check if string creation failed */
        if (server_target == NULL) {
            serverLog(LL_WARNING, "Failed to create server target string for external data dump");
            return EXTERNAL_ERROR;
        }
    }

    /* Pass -1 as dbid to indicate all databases */
    int result = mi->external_module->storage_methods.dump(
        mi->module_ctx,
        mi->storage_ctx,
        EXTERNAL_ALL_DBS,
        slot,
        timestamp,
        server_target,
        backup_id);

    if (result != EXTERNAL_SUCCESS) {
        serverLog(LL_WARNING, "External data dump failed");
        if (server_target) {
            decrRefCount((robj *)server_target);
        }
        return EXTERNAL_ERROR;
    }

    /* Call filter dump method if available, after storage dump */
    if (mi->external_module->filter_methods.dump) {
        int filter_result = mi->external_module->filter_methods.dump(
            mi->module_ctx,
            mi->filter_ctx,
            EXTERNAL_ALL_DBS,
            slot,
            timestamp,
            server_target,
            backup_id);
        
        if (filter_result != EXTERNAL_SUCCESS) {
            /* If filter dump failed, log but don't fail the overall operation */
            serverLog(LL_WARNING, "Filter dump failed");
            if (server_target) {
                decrRefCount((robj *)server_target);
            }
            return EXTERNAL_ERROR;
        }
    }

    if (server_target) {
        decrRefCount((robj *)server_target);
    }

    teardownModuleCtx(mi);
    return result;
}
/* Get a module instance for a specific database.
 * Returns NULL if no external data is initialized for the specified database. */
externalDataModuleInstance *externalDataGetModuleInstance(int dbid) {
    externalDataCtx *curr_external_data_ctx = getCurrentExternalDataCtx();
    if (!curr_external_data_ctx || !curr_external_data_ctx->dbdata) {
        return NULL;
    }
    
    /* Look up the specific database */
    sds db_name = getDBName(dbid);
    dictEntry *de = dictFind(curr_external_data_ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!de) {
        return NULL;
    }
    
    externalDbData *dbData = dictGetVal(de);
    return dbData->module_instance;
}


/* Call the load callback function for a module instance
 * Loads data for all databases at once
 * Returns EXTERNAL_SUCCESS on success, EXTERNAL_ERROR on failure */
int externalDataCallLoadFunc(externalDataModuleInstance *mi,
                             ValkeyModuleString *backup_id) {
    if (!mi || !mi->external_module || !mi->storage_ctx || !mi->module_ctx) {
        return EXTERNAL_ERROR;
    }

    /* Check if load function is available */
    if (!mi->external_module->storage_methods.load) {
        return EXTERNAL_ERROR;
    }

    setupModuleCtx(mi);

    /* Pass -1 as dbid to indicate all databases */
    int result = mi->external_module->storage_methods.load(
        mi->module_ctx,
        mi->storage_ctx,
        EXTERNAL_ALL_DBS,
        backup_id);

    /* Call filter load method if available, after storage load */
    if (result == EXTERNAL_SUCCESS && mi->external_module->filter_methods.load) {
        int filter_result = mi->external_module->filter_methods.load(
            mi->module_ctx,
            mi->filter_ctx,
            EXTERNAL_ALL_DBS,
            backup_id);
        
        /* If filter load failed, log but don't fail the overall operation */
        if (filter_result != EXTERNAL_SUCCESS) {
            serverLog(LL_WARNING, "Filter load failed but storage load succeeded");
        }
    }

    teardownModuleCtx(mi);
    return result;
}

/* Call the get_state callback function for a module instance
 * Returns EXTERNAL_SUCCESS on success, EXTERNAL_ERROR on failure */
int externalDataCallGetStateFunc(externalDataModuleInstance *mi,
                                 ValkeyModuleString *source,
                                 int **db_numbers,
                                 size_t *num_dbs) {
    if (!mi || !mi->external_module || !mi->storage_ctx || !mi->module_ctx) {
        return EXTERNAL_ERROR;
    }

    /* Check if get_state function is available */
    if (!mi->external_module->storage_methods.get_state) {
        return EXTERNAL_ERROR;
    }

    setupModuleCtx(mi);

    /* Call storage get_state method */
    int result = mi->external_module->storage_methods.get_state(
        mi->module_ctx,
        mi->storage_ctx,
        source,
        db_numbers,
        num_dbs);

    teardownModuleCtx(mi);
    return result;
}

struct extStorageInstanceIterator {
    externalDbData *dbdata;
    ValkeyModuleString *match;
    long long *type;
    int dbid;
    ValkeyModuleDictIter *iter;
};

externalStorageInstanceIterator *externalStorageInstanceIteratorInit(int dbid, robj *match, long long *type) {
    assert(getCurrentExternalDataCtx() != NULL);

    sds db_name = getDBName(dbid);
    dictEntry *db = dictFind(curr_external_data_ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!db) return NULL;

    externalDbData *dbData = dictGetVal(db);
    externalStorageInstanceIterator *esi_it = zmalloc(sizeof(externalStorageInstanceIterator));
    esi_it->dbdata = dbData;
    esi_it->match = match;
    esi_it->type = type;
    esi_it->dbid = dbid;
    esi_it->iter = NULL;
    return esi_it;
}

int externalStorageInstanceIteratorNext(externalStorageInstanceIterator *esi_it, robj **next) {
    /* Get the next element from the current iterator of the external storage instance. */
    assert(esi_it != NULL); // could be NULL when storage is not initialized for this db
    return esi_it->dbdata->module_instance->external_module->storage_methods.iterate(
        esi_it->dbdata->module_instance->module_ctx,
        esi_it->dbid,
        esi_it->match,
        esi_it->type,
        next,
        &esi_it->iter);
}

void externalStorageInstanceIteratorRelease(externalStorageInstanceIterator *esi_it) {
    zfree(esi_it);
}

/*
 * EXTERNAL_DATA DEBUG db STORAGE|FILTER set|del k [v]
 *
 * Manipulate storage and filter data directly to debug a certain db
 *
 */
void externalDataDebugCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, EXTDATAOFFERRMSG);
        return;
    }
    assert(getCurrentExternalDataCtx() != NULL);

    sds db_name = objectGetVal(c->argv[2]);
    if (checkDbNum(c, db_name) < 0) {
        return;
    }

    dictEntry *dbEntry = dictFind(curr_external_data_ctx->dbdata, db_name);
    if (!dbEntry) {
        addReplyErrorFormat(c, "%s is not initialized", db_name);
        return;
    }
    externalDbData *dbData = dictGetVal(dbEntry);
    sds dbIdSds = dictGetKey(dbEntry);
    int dbId = atoi(dbIdSds);

    int j = 3;
    if (!strcasecmp(objectGetVal(c->argv[j]), "storage")) {
        j++;
        if (!strcasecmp(objectGetVal(c->argv[j]), "set")) {
            robj *key = c->argv[++j];
            robj *value = c->argv[++j];
            if (externalStorageCallSetFunc(dbData->module_instance, dbId, key, value) != EXTERNAL_SUCCESS) {
                addReplyErrorFormat(c, "%s set failed", (char *)objectGetVal(key));
                return;
            }
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "get")) {
            robj *key = c->argv[++j];
            void *found = NULL;
            int exists = externalStorageCallGetFunc(dbData->module_instance, dbId, key, &found);
            if (exists && found != NULL) {
                addReplyBulkCString(c, objectGetVal((robj *)found));
                decrRefCount((robj *)found);
            } else {
                addReplyBulkCString(c, "");
            }
            return;
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "del")) {
            robj *key = c->argv[++j];
            robj *value = NULL;
            int deleted = externalStorageCallDelFunc(dbData->module_instance, dbId, key, &value);
            if (deleted && value != NULL) {
                addReplyBulkCString(c, objectGetVal(value));
                decrRefCount(value);
            } else {
                addReplyBulkCString(c, "");
            }
            return;
        } else {
            sds cmd = objectGetVal(c->argv[j]);
            addReplyErrorFormat(c, "unknown subcommand %s", cmd);
            return;
        }
    } else if (!strcasecmp(objectGetVal(c->argv[j]), "filter")) {
        j++;
        if (!strcasecmp(objectGetVal(c->argv[j]), "set")) {
            robj *key = c->argv[++j];
            if (externalFilterCallSetFunc(dbData->module_instance, dbId, key) != EXTERNAL_SUCCESS) {
                addReplyErrorFormat(c, "%s set failed", (char *)objectGetVal(key));
                return;
            }
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "get")) {
            robj *key = c->argv[++j];
            int exists = externalFilterIsIn(dbId, key);
            addReplyLongLong(c, exists);
            return;
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "del")) {
            robj *key = c->argv[++j];
            robj *value = NULL;
            int deleted = externalFilterCallDelFunc(dbData->module_instance, dbId, key, &value);
            if (deleted && value != NULL) {
                addReplyBulkCString(c, objectGetVal(value));
                decrRefCount(value);
            } else {
                addReplyBulkCString(c, "0");
            }
            return;
        } else {
            sds cmd = objectGetVal(c->argv[j]);
            addReplyErrorFormat(c, "unknown subcommand %s", cmd);
            return;
        }
    } else if (!strcasecmp(objectGetVal(c->argv[j]), "setro")) {
        int result = externalDataCallSetReadonlyFunc(dbData->module_instance);
        if (result != EXTERNAL_SUCCESS) {
            addReplyErrorFormat(c, "error code setting readonly: %d", result);
            return;
        }
    } else if (!strcasecmp(objectGetVal(c->argv[j]), "dropro")) {
        int result = externalDataCallDropReadonlyFunc(dbData->module_instance);
        if (result != EXTERNAL_SUCCESS) {
            addReplyErrorFormat(c, "error code setting readonly: %d", result);
            return;
        }
    } else {
        sds cmd = objectGetVal(c->argv[j]);
        addReplyErrorFormat(c, "unknown subcommand %s", cmd);
        return;
    }

    addReply(c, shared.ok);
    return;
}

/*
 * EXTERNAL_DATA DUMP [SLOT <slot>]
 *
 * Dumps external data for backup or replication (all databases at once)
 * Arguments can be specified in any order
 *
 */
void externalDataDumpCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, EXTDATAOFFERRMSG);
        return;
    }
    assert(getCurrentExternalDataCtx() != NULL);

    /* Parse optional arguments in any order */
    int slot = EXTERNAL_ALL_SLOTS;

    /* Parse arguments - they can appear in any order */
    for (int j = 2; j < c->argc; j++) {
        sds arg = objectGetVal(c->argv[j]);
        int remaining = c->argc - j - 1;

        if (!strcasecmp(arg, "slot") && remaining >= 1) {
            if (getLongFromObjectOrReply(c, c->argv[j+1], (long *)&slot, NULL) != C_OK) {
                return;
            }
            j++; /* Skip the value */
        } else {
            addReplyErrorFormat(c, "Unknown or incomplete argument: %s", arg);
            return;
        }
    }

    /* Find any initialized database to get the module instance
     * Since dump operates on all databases, we just need any one */
    dictIterator *di = dictGetIterator(curr_external_data_ctx->dbdata);
    dictEntry *de = dictNext(di);
    dictReleaseIterator(di);

    if (!de) {
        addReplyError(c, "No external data initialized for any database");
        return;
    }

    externalDbData *dbData = dictGetVal(de);
    ValkeyModuleString *backup_id = NULL;

    /* Call the dump function - it will dump all databases, using local node_id */
    int result = externalDataCallDumpFunc(dbData->module_instance, slot, mstime(), NULL, &backup_id);

    if (result != EXTERNAL_SUCCESS || backup_id == NULL) {
        addReplyError(c, "Failed to create backup");
        return;
    }

    /* Return the backup ID */
    addReplyBulkCBuffer(c, objectGetVal(backup_id), sdslen(objectGetVal(backup_id)));
}

/*
 * EXTERNAL_DATA LOAD [backup-id]
 *
 * Loads external data from backup for restore or replication (all databases at once)
 *
 */
void externalDataLoadCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, EXTDATAOFFERRMSG);
        return;
    }
    assert(getCurrentExternalDataCtx() != NULL);

    /* Parse optional backup-id argument */
    ValkeyModuleString *backup_id = NULL;
    if (c->argc >= 3) {
        backup_id = c->argv[2];
    }

    /* Find any initialized database to get the module instance
     * Since load operates on all databases, we just need any one */
    dictIterator *di = dictGetIterator(curr_external_data_ctx->dbdata);
    dictEntry *de = dictNext(di);
    dictReleaseIterator(di);

    if (!de) {
        addReplyError(c, "No external data initialized for any database");
        return;
    }

    externalDbData *dbData = dictGetVal(de);
    ValkeyModuleString *backup_id_to_use = backup_id;
    robj *created_backup_id = NULL;

    /* If we're a replica and no backup_id provided, get primary's node_id */
    if (!backup_id && is_replica()) {
        char *primary_addr = get_primary_address();
        
        if (primary_addr) {
            /* Ask module to construct backup_id for primary address */
            sds primary_addr_sds = sdsnew(primary_addr);
            sds primary_backup_id_sds = externalDataGetBackupId(primary_addr_sds, EXTERNAL_ALL_SLOTS);
            sdsfree(primary_addr_sds);
            
            if (primary_backup_id_sds) {
                /* Create a backup_id with primary's backup_id */
                created_backup_id = createStringObject(primary_backup_id_sds, sdslen(primary_backup_id_sds));
                backup_id_to_use = (ValkeyModuleString *)created_backup_id;
                serverLog(LL_NOTICE, "Replica loading from primary backup_id=%s (address=%s)",
                         primary_backup_id_sds, primary_addr);
                sdsfree(primary_backup_id_sds);
            } else {
                serverLog(LL_WARNING, "Failed to resolve primary address %s to backup_id", primary_addr);
            }
        }
    }

    /* Call the load function - it will load all databases */
    int result = externalDataCallLoadFunc(dbData->module_instance, backup_id_to_use);

    /* Free created backup_id if we made one */
    if (created_backup_id) {
        decrRefCount(created_backup_id);
    }

    if (result != EXTERNAL_SUCCESS) {
        addReplyError(c, "Failed to load backup");
        return;
    }

    server.dirty++;
    forceCommandPropagation(c, PROPAGATE_REPL);

    addReply(c, shared.ok);
}

int externalDataFind(int id, void *key, void **found) {
    assert(getCurrentExternalDataCtx() != NULL);

    sds db_name = getDBName(id);
    dictEntry *db = dictFind(curr_external_data_ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!db) return 0;

    externalDbData *dbData = dictGetVal(db);
    externalDataModuleInstance *mi = dbData->module_instance;
    if (!mi) return 0;

    if (!externalFilterCallGetFunc(mi, id, key)) return 0;

    return externalStorageCallGetFunc(mi, id, key, found);
}

int externalFilterIsIn(int id, void *key) {
    assert(getCurrentExternalDataCtx() != NULL);

    sds db_name = getDBName(id);
    dictEntry *db = dictFind(curr_external_data_ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!db) return 0;

    externalDbData *dbData = dictGetVal(db);
    externalDataModuleInstance *mi = dbData->module_instance;
    if (!mi) return 0;

    return externalFilterCallGetFunc(mi, id, key);
}

int externalDataWrite(int id, void *key, void *value) {
    assert(getCurrentExternalDataCtx() != NULL);

    sds db_name = getDBName(id);
    dictEntry *db = dictFind(curr_external_data_ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!db) {
        serverLog(LL_WARNING, "externalDataWrite: db not found for %d", id);
        return EXTERNAL_ERROR;
    }

    externalDbData *dbData = dictGetVal(db);
    externalDataModuleInstance *mi = dbData->module_instance;
    if (!mi) {
        serverLog(LL_WARNING, "externalDataWrite: mi not found for %d", id);
        return EXTERNAL_ERROR;
    }

    // Validate key and value parameters
    if (!key) {
        serverLog(LL_WARNING, "externalDataWrite: NULL key parameter for db %d", id);
        return EXTERNAL_ERROR;
    }
    if (!value) {
        serverLog(LL_WARNING, "externalDataWrite: NULL value parameter for db %d", id);
        return EXTERNAL_ERROR;
    }

    // Check if both storage and filter are in readonly state
    ValkeyModuleExternalStorageState storage_state = mi->storage_ctx->state;
    ValkeyModuleExternalFilterState filter_state = mi->filter_ctx->state;

    // If both are readonly, return 2 to signal the client should be blocked
    // The client will be retried later when the state changes
    // However, replicated clients (from primary or AOF) should never be blocked
    if ((storage_state == VMES_STATE_READONLY || filter_state == VMEF_STATE_READONLY) &&
        server.current_client && !mustObeyClient(server.current_client)) {
        return EXTERNAL_READONLY; // Signal to block the client
    }

    if (externalStorageCallSetFunc(mi, id, key, value) != EXTERNAL_SUCCESS) {
        serverLog(LL_WARNING, "externalDataWrite: storage set failed for %d", id);
        return EXTERNAL_ERROR;
    }
    return externalFilterCallSetFunc(mi, id, key);
}

/* Get current external data context */
externalDataCtx *getCurrentExternalDataCtx(void) {
    if (!curr_external_data_ctx) curr_external_data_ctx = externalDataCtxCreate();
    return curr_external_data_ctx;
}

/* Delete a key from external storage for a specific database */
int externalStorageDeleteKey(int dbid, robj *key, robj **value) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) return 0;

    sds db_name = getDBName(dbid);
    dictEntry *db = dictFind(ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!db) return 0;

    externalDbData *dbData = dictGetVal(db);
    externalDataModuleInstance *mi = dbData->module_instance;
    if (!mi) return 0;

    return externalStorageCallDelFunc(mi, dbid, key, value);
}

/* Delete a key from external filter for a specific database */
int externalFilterDeleteKey(int dbid, robj *key, robj **value) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) return 0;

    sds db_name = getDBName(dbid);
    dictEntry *db = dictFind(ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!db) return 0;

    externalDbData *dbData = dictGetVal(db);
    externalDataModuleInstance *mi = dbData->module_instance;
    if (!mi) return 0;

    return externalFilterCallDelFunc(mi, dbid, key, value);
}

/* Flush external data for a specific database and slot */
void externalDataFlushDb(int dbid, int slot) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) return;

    sds db_name = getDBName(dbid);
    dictEntry *db = dictFind(ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!db) return;

    externalDbData *dbData = dictGetVal(db);
    externalDataModuleInstance *mi = dbData->module_instance;
    if (!mi) return;

    setupModuleCtx(mi);

    /* Use native flush functions if available (O(1)), otherwise fall back to iteration (O(n)) */
    if (mi->external_module->storage_methods.flush != NULL &&
        mi->external_module->filter_methods.flush != NULL) {
        /* Fast path: use native flush functions for O(1) performance */
        mi->external_module->storage_methods.flush(mi->module_ctx, mi->storage_ctx, dbid, slot);
        mi->external_module->filter_methods.flush(mi->module_ctx, mi->filter_ctx, dbid, slot);
    } else {
        /* Slow path: iterate and delete each key for backward compatibility with modules
         * that don't implement flush functions. We collect all keys first to avoid iterator
         * invalidation during deletion */
        list *keys_to_delete = listCreate();
        externalStorageInstanceIterator *esi_it = externalStorageInstanceIteratorInit(dbid, NULL, NULL);
        if (esi_it != NULL) {
            robj *next;
            while (externalStorageInstanceIteratorNext(esi_it, &next)) {
                listAddNodeTail(keys_to_delete, next);
            }
            externalStorageInstanceIteratorRelease(esi_it);
        }

        /* Now delete all the collected keys */
        listIter li;
        listNode *ln;
        listRewind(keys_to_delete, &li);
        while ((ln = listNext(&li))) {
            robj *key = listNodeValue(ln);

            /* Delete from storage - context already set up, so call methods directly */
            robj *value = NULL;
            ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
            mi->external_module->storage_methods.del(mi->module_ctx, mi->storage_ctx, &key_ctx, &value);
            if (value) {
                decrRefCount(value);
            }

            /* Delete from filter - context already set up, so call methods directly */
            robj *filter_value = NULL;
            ValkeyModuleKeyOptCtx filter_key_ctx = {key, NULL, dbid, -1};
            mi->external_module->filter_methods.del(mi->module_ctx, mi->filter_ctx, &filter_key_ctx, &filter_value);
            if (filter_value) {
                decrRefCount(filter_value);
            }

            decrRefCount(key);
        }

        listRelease(keys_to_delete);
    }

    teardownModuleCtx(mi);
}

/* Flush external data for all databases */
void externalDataFlushAll(void) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) return;

    /* Iterate through all databases and flush each one */
    for (int i = 0; i < server.dbnum; i++) {
        externalDataFlushDb(i, EXTERNAL_ALL_SLOTS);
    }
}

/* Swap external data between two databases */
void externalDataSwapDb(int id1, int id2) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) return;

    sds db_name1 = getDBName(id1);
    sds db_name2 = getDBName(id2);

    dictEntry *dbEntry1 = dictFind(ctx->dbdata, db_name1);
    dictEntry *dbEntry2 = dictFind(ctx->dbdata, db_name2);

    /* If both databases are not initialized, nothing to do */
    if (!dbEntry1 && !dbEntry2) {
        sdsfree(db_name1);
        sdsfree(db_name2);
        return;
    }

    /* If only one database is initialized, we need to move the external data
     * from one to the other */
    if (!dbEntry1 || !dbEntry2) {
        sds src_name = dbEntry1 ? db_name1 : db_name2;
        sds dst_name = dbEntry1 ? db_name2 : db_name1;

        /* Remove the destination entry if it exists */
        dictDelete(ctx->dbdata, dst_name);

        /* Add a new entry for the destination with the source's data */
        externalDbData *srcData = dictGetVal(dbEntry1 ? dbEntry1 : dbEntry2);
        externalDbData *dstData = zmalloc(sizeof(*dstData));
        *dstData = *srcData;

        /* Update the module context to use the new database ID */
        if (dstData->module_instance && dstData->module_instance->module_ctx) {
            moduleFreeContext(dstData->module_instance->module_ctx);
            zfree(dstData->module_instance->module_ctx);
            dstData->module_instance->module_ctx = moduleAllocateContext();
            moduleExternalStorageInitContext(dstData->module_instance->module_ctx,
                                             dstData->module_instance->external_module->module);
        }

        /* Add to dictionary - if this fails, clean up the allocated data */
        if (dictAdd(ctx->dbdata, dst_name, dstData) != DICT_OK) {
            /* Dictionary add failed, clean up the allocated data */
            if (dstData->module_instance && dstData->module_instance->module_ctx) {
                moduleFreeContext(dstData->module_instance->module_ctx);
                zfree(dstData->module_instance->module_ctx);
            }
            zfree(dstData);
            sdsfree(dst_name);
            sdsfree(db_name1);
            sdsfree(db_name2);
            return;
        }

        /* Remove the source entry */
        dictDelete(ctx->dbdata, src_name);

        sdsfree(db_name1);
        sdsfree(db_name2);
        return;
    }

    /* Both databases are initialized, swap their external data */
    externalDbData *dbData1 = dictGetVal(dbEntry1);
    externalDbData *dbData2 = dictGetVal(dbEntry2);

    /* Remove both entries */
    dictDelete(ctx->dbdata, db_name1);
    dictDelete(ctx->dbdata, db_name2);

    /* Re-add them with swapped keys - if this fails, restore original state */
    if (dictAdd(ctx->dbdata, db_name1, dbData2) != DICT_OK) {
        /* Restore original entries */
        dictAdd(ctx->dbdata, db_name1, dbData1);
        dictAdd(ctx->dbdata, db_name2, dbData2);
        sdsfree(db_name1);
        sdsfree(db_name2);
        return;
    }

    if (dictAdd(ctx->dbdata, db_name2, dbData1) != DICT_OK) {
        /* Restore original entries */
        dictDelete(ctx->dbdata, db_name1);
        dictAdd(ctx->dbdata, db_name1, dbData1);
        dictAdd(ctx->dbdata, db_name2, dbData2);
        sdsfree(db_name1);
        sdsfree(db_name2);
        return;
    }

    /* Update the module contexts to use the new database IDs */
    if (dbData1->module_instance && dbData1->module_instance->module_ctx) {
        moduleFreeContext(dbData1->module_instance->module_ctx);
        zfree(dbData1->module_instance->module_ctx);
        dbData1->module_instance->module_ctx = moduleAllocateContext();
        moduleExternalStorageInitContext(dbData1->module_instance->module_ctx,
                                         dbData1->module_instance->external_module->module);
    }

    if (dbData2->module_instance && dbData2->module_instance->module_ctx) {
        moduleFreeContext(dbData2->module_instance->module_ctx);
        zfree(dbData2->module_instance->module_ctx);
        dbData2->module_instance->module_ctx = moduleAllocateContext();
        moduleExternalStorageInitContext(dbData2->module_instance->module_ctx,
                                         dbData2->module_instance->external_module->module);
    }

    /* Call the swap function on the module if it's available, otherwise fall back to iteration */
    if (dbData1->module_instance && dbData1->module_instance->external_module) {
        /* Handle storage swap */
        if (dbData1->module_instance->external_module->storage_methods.swap) {
            /* Fast path: use native swap function for O(1) performance */
            setupModuleCtx(dbData1->module_instance);
            dbData1->module_instance->external_module->storage_methods.swap(
                dbData1->module_instance->module_ctx,
                dbData1->module_instance->storage_ctx,
                id1, id2);
            teardownModuleCtx(dbData1->module_instance);
        } else {
            /* Slow path: iterate and move each key for backward compatibility with modules
             * that don't implement swap functions. We collect all keys first to avoid iterator
             * invalidation during movement */
            setupModuleCtx(dbData1->module_instance);
            setupModuleCtx(dbData2->module_instance);

            list *keys_to_move = listCreate();
            externalStorageInstanceIterator *esi_it = externalStorageInstanceIteratorInit(id1, NULL, NULL);
            if (esi_it != NULL) {
                robj *next;
                while (externalStorageInstanceIteratorNext(esi_it, &next)) {
                    listAddNodeTail(keys_to_move, next);
                }
                externalStorageInstanceIteratorRelease(esi_it);
            }

            /* Now move all the collected keys from db1 to db2 */
            listIter li;
            listNode *ln;
            listRewind(keys_to_move, &li);
            while ((ln = listNext(&li))) {
                robj *key = listNodeValue(ln);

                /* Get the value from db1 - context already set up */
                void *value = NULL;
                ValkeyModuleKeyOptCtx key_ctx = {key, NULL, id1, -1};
                if (dbData1->module_instance->external_module->storage_methods.get(
                        dbData1->module_instance->module_ctx, dbData1->module_instance->storage_ctx, &key_ctx, &value)) {
                    /* Set the value in db2 - context already set up */
                    ValkeyModuleKeyOptCtx key_ctx2 = {key, NULL, id2, -1};
                    if (dbData2->module_instance->external_module->storage_methods.set(
                            dbData2->module_instance->module_ctx, dbData2->module_instance->storage_ctx, &key_ctx2, (robj *)value) == EXTERNAL_SUCCESS) {
                        /* Delete from db1 - context already set up */
                        robj *deleted_value = NULL;
                        ValkeyModuleKeyOptCtx del_key_ctx = {key, NULL, id1, -1};
                        dbData1->module_instance->external_module->storage_methods.del(
                            dbData1->module_instance->module_ctx, dbData1->module_instance->storage_ctx, &del_key_ctx, &deleted_value);
                        if (deleted_value) decrRefCount(deleted_value);
                    }
                    if (value) decrRefCount((robj *)value);
                }

                decrRefCount(key);
            }

            listRelease(keys_to_move);

            teardownModuleCtx(dbData1->module_instance);
            teardownModuleCtx(dbData2->module_instance);
        }

        /* Handle filter swap */
        if (dbData1->module_instance->external_module->filter_methods.swap) {
            /* Fast path: use native swap function for O(1) performance */
            setupModuleCtx(dbData1->module_instance);
            dbData1->module_instance->external_module->filter_methods.swap(
                dbData1->module_instance->module_ctx,
                dbData1->module_instance->filter_ctx,
                id1, id2);
            teardownModuleCtx(dbData1->module_instance);
        } else {
            /* Slow path: iterate and move each key for backward compatibility with modules
             * that don't implement swap functions */
            setupModuleCtx(dbData1->module_instance);
            setupModuleCtx(dbData2->module_instance);

            list *keys_to_move = listCreate();
            externalStorageInstanceIterator *esi_it = externalStorageInstanceIteratorInit(id1, NULL, NULL);
            if (esi_it != NULL) {
                robj *next;
                while (externalStorageInstanceIteratorNext(esi_it, &next)) {
                    listAddNodeTail(keys_to_move, next);
                }
                externalStorageInstanceIteratorRelease(esi_it);
            }

            /* Now move all the collected keys from db1 to db2 */
            listIter li;
            listNode *ln;
            listRewind(keys_to_move, &li);
            while ((ln = listNext(&li))) {
                robj *key = listNodeValue(ln);

                /* Check if key is in filter for db1 - context already set up */
                ValkeyModuleKeyOptCtx key_ctx = {key, NULL, id1, -1};
                if (dbData1->module_instance->external_module->filter_methods.get(
                        dbData1->module_instance->module_ctx, dbData1->module_instance->filter_ctx, &key_ctx)) {
                    /* Add to filter for db2 - context already set up */
                    ValkeyModuleKeyOptCtx key_ctx2 = {key, NULL, id2, -1};
                    if (dbData2->module_instance->external_module->filter_methods.set(
                            dbData2->module_instance->module_ctx, dbData2->module_instance->filter_ctx, &key_ctx2) == EXTERNAL_SUCCESS) {
                        /* Delete from filter for db1 - context already set up */
                        robj *deleted_value = NULL;
                        ValkeyModuleKeyOptCtx del_key_ctx = {key, NULL, id1, -1};
                        dbData1->module_instance->external_module->filter_methods.del(
                            dbData1->module_instance->module_ctx, dbData1->module_instance->filter_ctx, &del_key_ctx, &deleted_value);
                        if (deleted_value) decrRefCount(deleted_value);
                    }
                }

                decrRefCount(key);
            }

            listRelease(keys_to_move);

            teardownModuleCtx(dbData1->module_instance);
            teardownModuleCtx(dbData2->module_instance);
        }
    }

    /* Note: We don't free db_name1 and db_name2 here because they are now owned by the dictionary */
}

/* Count the number of external data keys for a specific database */
unsigned long long externalDataCountKeys(int dbid) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) return 0;

    sds db_name = getDBName(dbid);
    dictEntry *db = dictFind(ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!db) return 0;

    externalDbData *dbData = dictGetVal(db);
    externalDataModuleInstance *mi = dbData->module_instance;
    if (!mi) return 0;

    setupModuleCtx(mi);

    unsigned long long count = 0;

    /* Try to use the keys_count method if available (more efficient) */
    if (mi->external_module->filter_methods.keys_count) {
        if (mi->external_module->filter_methods.keys_count(mi->module_ctx, mi->filter_ctx, dbid, &count) == EXTERNAL_SUCCESS) {
            /* If keys_count succeeded, we're done */
            teardownModuleCtx(mi);
            return count;
        }
        /* If keys_count failed, fall back to iterator method */
    }

    /* Use the iterator method as a fallback */
    externalStorageInstanceIterator *esi_it = externalStorageInstanceIteratorInit(dbid, NULL, NULL);
    if (esi_it != NULL) {
        robj *next;
        while (externalStorageInstanceIteratorNext(esi_it, &next)) {
            /* Check if the key exists in the filter */
            if (externalFilterCallGetFunc(mi, dbid, next)) {
                count++;
            }
            decrRefCount(next);
        }
        externalStorageInstanceIteratorRelease(esi_it);
    }

    teardownModuleCtx(mi);
    return count;
}

/* Process external data dump in BIO thread */
void bioExternalDataDumpForFullSync(void) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) {
        return;
    }
    
    /* Check if there are any initialized databases */
    if (dictSize(ctx->dbdata) == 0) {
        serverLog(LL_NOTICE, "No external data initialized for full sync dump - creating empty backup");
        /* When there are no databases, we should still mark the dump as successful
         * to prevent replication failures in cluster mode */
        
        /* Create an empty backup ID to indicate successful dump */
        ValkeyModuleString *empty_backup_id = (ValkeyModuleString *)createStringObject("empty_backup", 12);
        if (!empty_backup_id) {
            serverLog(LL_WARNING, "Failed to create empty backup ID");
            return;
        }
        
        /* Set the backup ID in all module contexts to indicate successful dump */
        dictIterator *di = dictGetIterator(ctx->modules);
        dictEntry *de;
        while ((de = dictNext(di))) {
            externalDataModule *module = dictGetVal(de);
            
            /* Find any database using this module */
            dictIterator *db_di = dictGetIterator(ctx->dbdata);
            dictEntry *db_de;
            while ((db_de = dictNext(db_di))) {
                externalDbData *dbData = dictGetVal(db_de);
                if (dbData->module_instance->external_module == module) {
                    storageCtx *storage_ctx = dbData->module_instance->storage_ctx;
                    filterCtx *filter_ctx = dbData->module_instance->filter_ctx;
                    
                    /* Set both storage and filter dump status to success */
                    atomic_store_explicit(&storage_ctx->ext_data_dump_status, VMES_DUMP_STATE_SUCCESS, memory_order_release);
                    atomic_store_explicit(&filter_ctx->ext_data_dump_status, VMEF_DUMP_STATE_SUCCESS, memory_order_release);
                    
                    /* Store backup ID in both storage and filter contexts */
                    if (storage_ctx->ext_data_dump_backup_id) {
                        sdsfree(storage_ctx->ext_data_dump_backup_id);
                    }
                    storage_ctx->ext_data_dump_backup_id = sdsnew("empty_backup");
                    
                    if (filter_ctx->ext_data_dump_backup_id) {
                        sdsfree(filter_ctx->ext_data_dump_backup_id);
                    }
                    filter_ctx->ext_data_dump_backup_id = sdsnew("empty_backup");
                    
                    serverLog(LL_NOTICE, "Set empty backup for module %s", module->name);
                    break;
                }
            }
            dictReleaseIterator(db_di);
        }
        dictReleaseIterator(di);
        
        /* Free the empty backup ID */
        decrRefCount((robj *)empty_backup_id);
        return;
    }
    
    /* Iterate all databases and initiate dump on each unique module instance
     * Track which module instances we've already processed to avoid duplicates */
    dictIterator *di = dictGetIterator(ctx->dbdata);
    dictEntry *de;
    dict *processed_modules = dictCreate(&moduleDictType);
    
    while ((de = dictNext(di))) {
        sds db_name = dictGetKey(de);
        externalDbData *dbData = dictGetVal(de);
        externalDataModuleInstance *mi = dbData->module_instance;

        storageCtx *storage_ctx = mi->storage_ctx;
        filterCtx *filter_ctx = mi->filter_ctx;
        
        atomic_store_explicit(&storage_ctx->ext_data_dump_status, VMES_DUMP_STATE_IN_PROGRESS, memory_order_release);
        atomic_store_explicit(&filter_ctx->ext_data_dump_status, VMEF_DUMP_STATE_IN_PROGRESS, memory_order_release);
        
        /* Check if we've already processed this module instance
         * Multiple databases could share the same module instance */
        if (dictFind(processed_modules, mi->external_module->name)) {
            serverLog(LL_DEBUG, "Skipping %s - module instance already processed", db_name);
            continue;
        }
        
        /* Mark this module instance as processed */
        dictAdd(processed_modules, mi->external_module->name, mi);
        
        serverLog(LL_NOTICE, "Starting dump for module instance: %s (database: %s)",
                  mi->external_module->name, db_name);
        
        ValkeyModuleString *backup_id = NULL;
        
        /* Call dump with timestamp 0 for full backup, -1 for all slots */
        int result = externalDataCallDumpFunc(mi, EXTERNAL_ALL_SLOTS, 0, NULL, &backup_id);
        
        if (result != EXTERNAL_SUCCESS) {
            serverLog(LL_WARNING, "Failed to dump external data for module %s",
                      mi->external_module->name);
            atomic_store_explicit(&storage_ctx->ext_data_dump_status, VMES_DUMP_STATE_FAILED, memory_order_release);
            atomic_store_explicit(&filter_ctx->ext_data_dump_status, VMEF_DUMP_STATE_FAILED, memory_order_release);
            continue;
        }
        
        /* If dump succeeded but backup_id is NULL, create a dummy backup ID to indicate success
         * This can happen when there are no databases to dump */
        if (!backup_id) {
            backup_id = (ValkeyModuleString *)createStringObject("empty_backup", 12);
            if (!backup_id) {
                serverLog(LL_WARNING, "Failed to create backup ID for module %s",
                          mi->external_module->name);
                atomic_store_explicit(&storage_ctx->ext_data_dump_status, VMES_DUMP_STATE_FAILED, memory_order_release);
                atomic_store_explicit(&filter_ctx->ext_data_dump_status, VMEF_DUMP_STATE_FAILED, memory_order_release);
                continue;
            }
        }

        atomic_store_explicit(&storage_ctx->ext_data_dump_status, VMES_DUMP_STATE_SUCCESS, memory_order_release);
        atomic_store_explicit(&filter_ctx->ext_data_dump_status, VMEF_DUMP_STATE_SUCCESS, memory_order_release);
        
        /* Store backup ID in both storage and filter contexts */
        if (storage_ctx->ext_data_dump_backup_id) {
            sdsfree(storage_ctx->ext_data_dump_backup_id);
        }
        storage_ctx->ext_data_dump_backup_id = sdsnew((char *)objectGetVal(backup_id));
        
        if (filter_ctx->ext_data_dump_backup_id) {
            sdsfree(filter_ctx->ext_data_dump_backup_id);
        }
        filter_ctx->ext_data_dump_backup_id = sdsnew((char *)objectGetVal(backup_id));
        
        serverLog(LL_NOTICE, "External data dumped for module %s: %s",
                  mi->external_module->name, (char *)objectGetVal(backup_id));
    }
    dictReleaseIterator(di);
    dictRelease(processed_modules);
}

/* Start async dump of external data for full sync */
int externalDataDumpForFullSync(void) {
    if (!isExtDataOn()) return EXTERNAL_ERROR;

    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (ctx) {
        /* Iterate all databases and create snapshots if supported */
        dictIterator *di = dictGetIterator(ctx->dbdata);
        dictEntry *de;
        while ((de = dictNext(di))) {
            externalDbData *dbData = dictGetVal(de);
            externalDataModuleInstance *mi = dbData->module_instance;
            sds db_name = dictGetKey(de);
            int dbid = atoi(db_name + 2); /* Skip "db" prefix */

            /* Create storage snapshot */
            if (mi->external_module->storage_methods.snapshot) {
                mi->storage_ctx->snapshot = mi->external_module->storage_methods.snapshot(
                    mi->module_ctx, mi->storage_ctx, dbid);
            } else {
                mi->storage_ctx->snapshot = NULL;
            }

            /* Create filter snapshot */
            if (mi->external_module->filter_methods.snapshot) {
                mi->filter_ctx->snapshot = mi->external_module->filter_methods.snapshot(
                    mi->module_ctx, mi->filter_ctx, dbid);
            } else {
                mi->filter_ctx->snapshot = NULL;
            }
        }
        dictReleaseIterator(di);
    }
    
    /* Submit BIO job - state will be set in the BIO thread */
    bioCreateExtDataDumpJob();
    
    return EXTERNAL_SUCCESS;
}

/* Check if external data dump is complete and get result */
int externalDataDumpCheckComplete(ValkeyModuleString **backup_id) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) return C_ERR;
    
    /* Check if there are any initialized databases */
    if (dictSize(ctx->dbdata) == 0) {
        serverLog(LL_NOTICE, "No external data initialized - dump completed successfully");
        if (backup_id) {
            *backup_id = (ValkeyModuleString *)createStringObject("empty_backup", 12);
        }
        return C_OK; /* No databases to dump = successful dump */
    }
    
    /* Find any initialized database to get the module instance */
    dictIterator *di = dictGetIterator(ctx->dbdata);
    dictEntry *de = dictNext(di);
    dictReleaseIterator(di);
    
    if (!de) return C_ERR;
    
    externalDbData *dbData = dictGetVal(de);
    storageCtx *storage_ctx = dbData->module_instance->storage_ctx;
    filterCtx *filter_ctx = dbData->module_instance->filter_ctx;
    
    /* Check both storage and filter dump states */
    int storage_status = atomic_load_explicit(&storage_ctx->ext_data_dump_status, memory_order_acquire);
    int filter_status = atomic_load_explicit(&filter_ctx->ext_data_dump_status, memory_order_acquire);
    
    if (storage_status == VMES_DUMP_STATE_IN_PROGRESS || filter_status == VMEF_DUMP_STATE_IN_PROGRESS) {
        return C_ERR; /* Still in progress */
    }
    
    /* Handle the case where dump completed with empty_backup */
    if (storage_ctx->ext_data_dump_backup_id &&
        strcmp(storage_ctx->ext_data_dump_backup_id, "empty_backup") == 0) {
        serverLog(LL_NOTICE, "External data dump completed successfully: empty_backup");
        if (backup_id) {
            *backup_id = (ValkeyModuleString *)createStringObject("empty_backup", 12);
        }
        return C_OK;
    }
    
    if (storage_status == VMES_DUMP_STATE_SUCCESS && filter_status == VMEF_DUMP_STATE_SUCCESS) {
        /* Free snapshots */
        if (dbData->module_instance->external_module->storage_methods.free_snapshot && storage_ctx->snapshot) {
            dbData->module_instance->external_module->storage_methods.free_snapshot(
                dbData->module_instance->module_ctx, storage_ctx, storage_ctx->snapshot);
            storage_ctx->snapshot = NULL;
        }
        if (dbData->module_instance->external_module->filter_methods.free_snapshot && filter_ctx->snapshot) {
            dbData->module_instance->external_module->filter_methods.free_snapshot(
                dbData->module_instance->module_ctx, filter_ctx, filter_ctx->snapshot);
            filter_ctx->snapshot = NULL;
        }

        if (backup_id) {
            *backup_id = (ValkeyModuleString *)createStringObject(storage_ctx->ext_data_dump_backup_id,
                                                                   sdslen(storage_ctx->ext_data_dump_backup_id));
        }
        serverLog(LL_NOTICE, "External data dump completed successfully: %s",
                  storage_ctx->ext_data_dump_backup_id ? storage_ctx->ext_data_dump_backup_id : "empty");
        return C_OK;
    }
    
    serverLog(LL_WARNING, "External data dump check failed - storage_status=%d, filter_status=%d, backup_id=%s",
              storage_status, filter_status,
              storage_ctx->ext_data_dump_backup_id ? storage_ctx->ext_data_dump_backup_id : "NULL");
    return C_ERR;
}

/* Helper: Set load status for both storage and filter contexts */
static void setModuleLoadStatus(storageCtx *storage_ctx, filterCtx *filter_ctx, int status) {
    atomic_store_explicit(&storage_ctx->ext_data_load_status, status, memory_order_release);
    atomic_store_explicit(&filter_ctx->ext_data_load_status, status, memory_order_release);
}

/* Helper: Try to auto-initialize from all registered modules
 * Returns 1 if any module was successfully initialized, 0 otherwise */
static int tryAutoInitializeAllModules(externalDataCtx *ctx) {
    dictIterator *mod_di = dictGetIterator(ctx->modules);
    dictEntry *mod_de;
    int any_initialized = 0;
    
    while ((mod_de = dictNext(mod_di))) {
        externalDataModule *module = dictGetVal(mod_de);
        
        int init_result = tryAutoInitFromModuleState(module);
        if (init_result == EXTERNAL_SUCCESS) {
            any_initialized = 1;
        }
    }
    dictReleaseIterator(mod_di);
    
    return any_initialized;
}

/* Helper: Mark module load as complete when no databases exist
 * This prevents replication failures in cluster mode */
static void markModuleLoadAsComplete(externalDataCtx *ctx) {
    serverLog(LL_NOTICE, "No external data initialized for sync load - marking as completed");
    
    /* Find any module to mark load as successful */
    dictIterator *di = dictGetIterator(ctx->modules);
    dictEntry *de;
    
    while ((de = dictNext(di))) {
        externalDataModule *module = dictGetVal(de);
        
        /* Find any database using this module (shouldn't exist, but be safe) */
        dictIterator *db_di = dictGetIterator(ctx->dbdata);
        dictEntry *db_de;
        
        while ((db_de = dictNext(db_di))) {
            externalDbData *dbData = dictGetVal(db_de);
            if (dbData->module_instance->external_module == module) {
                setModuleLoadStatus(dbData->module_instance->storage_ctx,
                                   dbData->module_instance->filter_ctx,
                                   VMES_LOAD_STATE_SUCCESS);
                
                serverLog(LL_NOTICE, "Marked load as successful for module %s (no databases)", module->name);
                break;
            }
        }
        dictReleaseIterator(db_di);
    }
    dictReleaseIterator(di);
}

/* Helper: Prepare backup_id for replica nodes
 * Returns backup_id to use and sets created_backup_id if we allocated one */
static ValkeyModuleString *prepareReplicaBackupId(externalDataModuleInstance *mi, robj **created_backup_id) {
    ValkeyModuleString *backup_id_to_use = NULL;
    *created_backup_id = NULL;
    
    /* If we're a replica, get primary's backup_id */
    if (!is_replica()) {
        return NULL;
    }
    
    char *primary_addr = get_primary_address();
    
    if (primary_addr) {
        /* Ask module to construct backup_id for primary address */
        sds primary_addr_sds = sdsnew(primary_addr);
        sds primary_backup_id_sds = externalDataGetBackupId(primary_addr_sds, EXTERNAL_ALL_SLOTS);
        sdsfree(primary_addr_sds);
        
        if (primary_backup_id_sds) {
            /* Use the backup_id provided by the module */
            *created_backup_id = createStringObject(primary_backup_id_sds, sdslen(primary_backup_id_sds));
            backup_id_to_use = (ValkeyModuleString *)(*created_backup_id);
            serverLog(LL_NOTICE, "Replica auto-loading from primary backup_id=%s (address=%s)",
                     primary_backup_id_sds, primary_addr);
            sdsfree(primary_backup_id_sds);
        } else {
            serverLog(LL_WARNING, "Failed to resolve primary address %s to backup_id", primary_addr);
        }
    } else {
        serverLog(LL_WARNING, "Primary address is NULL, cannot load from primary");
    }
    
    return backup_id_to_use;
}

/* Helper: Load external data for a module instance and update status */
static void loadModuleInstanceData(externalDataModuleInstance *mi,
                                   ValkeyModuleString *backup_id_to_use,
                                   robj *created_backup_id) {
    storageCtx *storage_ctx = mi->storage_ctx;
    filterCtx *filter_ctx = mi->filter_ctx;
    
    /* Call load function with backup_id (NULL for primary, primary's node_id for replica)
     * The module will determine the exact backup file to load */
    serverLog(LL_NOTICE, "Loading external data from backup for module %s",
              mi->external_module->name);
    int result = externalDataCallLoadFunc(mi, backup_id_to_use);
    
    /* Free created backup_id if we made one */
    if (created_backup_id) {
        decrRefCount(created_backup_id);
    }
    
    if (result != EXTERNAL_SUCCESS) {
        serverLog(LL_WARNING, "Failed to load external data for module %s",
                  mi->external_module->name);
        setModuleLoadStatus(storage_ctx, filter_ctx, VMES_LOAD_STATE_FAILED);
        return;
    }
    
    serverLog(LL_NOTICE, "External data loaded for module %s",
              mi->external_module->name);
    setModuleLoadStatus(storage_ctx, filter_ctx, VMES_LOAD_STATE_SUCCESS);
}

/* Process external data load in BIO thread */
void bioExternalDataLoadForFullSync(void) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) {
        return;
    }
    
    /* Check if there are any initialized databases */
    if (dictSize(ctx->dbdata) == 0) {
        /* Try to auto-initialize from each module's state before giving up */
        tryAutoInitializeAllModules(ctx);
        
        /* If still no databases after auto-init, mark as completed */
        if (dictSize(ctx->dbdata) == 0) {
            markModuleLoadAsComplete(ctx);
            return;
        }
    }
    
    /* Iterate all databases and initiate load on each unique module instance
     * Track which module instances we've already processed to avoid duplicates */
    dictIterator *di = dictGetIterator(ctx->dbdata);
    dictEntry *de;
    dict *processed_modules = dictCreate(&moduleDictType);
    
    while ((de = dictNext(di))) {
        sds db_name = dictGetKey(de);
        externalDbData *dbData = dictGetVal(de);
        externalDataModuleInstance *mi = dbData->module_instance;
        
        /* Check if we've already processed this module instance
         * Multiple databases could share the same module instance */
        if (dictFind(processed_modules, mi->external_module->name)) {
            serverLog(LL_DEBUG, "Skipping %s - module instance already processed", db_name);
            continue;
        }

        setModuleLoadStatus(mi->storage_ctx, mi->filter_ctx, VMES_LOAD_STATE_IN_PROGRESS);
        
        /* Mark this module instance as processed */
        dictAdd(processed_modules, mi->external_module->name, mi);
        
        serverLog(LL_NOTICE, "Starting load for module instance: %s (database: %s)",
                  mi->external_module->name, db_name);
        
        /* Prepare backup_id for replicas */
        robj *created_backup_id = NULL;
        ValkeyModuleString *backup_id_to_use = prepareReplicaBackupId(mi, &created_backup_id);
        
        /* Load external data and update status */
        loadModuleInstanceData(mi, backup_id_to_use, created_backup_id);
    }
    dictReleaseIterator(di);
    dictRelease(processed_modules);
}

/* Start async load of external data for full sync */
int externalDataLoadForFullSync(void) {
    if (!isExtDataOn()) return EXTERNAL_ERROR;
    
    /* Submit BIO job - state will be set in the BIO thread */
    bioCreateExtDataLoadJob();
    
    return EXTERNAL_SUCCESS;
}

/* Check if external data load is complete */
int externalDataLoadCheckComplete(void) {
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    if (!ctx) return C_ERR;
    
    /* Check all databases to ensure all loads are complete */
    for (int dbid = 0; dbid < server.dbnum; dbid++) {
        externalDataModuleInstance *mi = externalDataGetModuleInstance(dbid);
        if (!mi) continue;
        
        externalDbData *dbData = NULL;
        sds db_name = getDBName(dbid);
        dictEntry *de = dictFind(ctx->dbdata, db_name);
        sdsfree(db_name);
        if (de) dbData = dictGetVal(de);
        if (!dbData) continue;
        
        storageCtx *storage_ctx = dbData->module_instance->storage_ctx;
        filterCtx *filter_ctx = dbData->module_instance->filter_ctx;
        
        /* Check both storage and filter load states */
        int storage_status = atomic_load_explicit(&storage_ctx->ext_data_load_status, memory_order_acquire);
        int filter_status = atomic_load_explicit(&filter_ctx->ext_data_load_status, memory_order_acquire);
        
        if (storage_status == VMES_LOAD_STATE_IN_PROGRESS || filter_status == VMEF_LOAD_STATE_IN_PROGRESS) {
            return C_ERR; /* Still in progress */
        }
        
        if (storage_status != VMES_LOAD_STATE_SUCCESS || filter_status != VMEF_LOAD_STATE_SUCCESS) {
            serverLog(LL_WARNING, "External data load failed for database %d", dbid);
            return C_ERR;
        }
    }
    
    serverLog(LL_NOTICE, "External data load completed successfully for all databases");
    return C_OK;
}

/* Helper function to try auto-initialization from a module's state
 * Returns 1 if databases were initialized, 0 otherwise
 * This is extracted to be reusable from both module registration and deferred init */
static int tryAutoInitFromModuleState(externalDataModule *module) {
    serverLog(LL_NOTICE, "=== tryAutoInitFromModuleState START ===");
    
    if (!isExtDataOn()) {
        serverLog(LL_WARNING, "tryAutoInitFromModuleState: external data is OFF");
        return EXTERNAL_ERROR;
    }
    
    serverLog(LL_NOTICE, "DEBUG: tryAutoInitFromModuleState() - curr_external_data_ctx=%p before getCurrentExternalDataCtx()",
              (void*)curr_external_data_ctx);
    externalDataCtx *ctx = getCurrentExternalDataCtx();
    serverLog(LL_NOTICE, "DEBUG: tryAutoInitFromModuleState() - ctx=%p after getCurrentExternalDataCtx()", (void*)ctx);
    if (!ctx) {
        serverLog(LL_WARNING, "tryAutoInitFromModuleState: failed to get external data context");
        return EXTERNAL_ERROR;
    }
    serverLog(LL_NOTICE, "DEBUG: tryAutoInitFromModuleState() - ctx->dbdata=%p, dictSize=%lu",
              (void*)ctx->dbdata, (unsigned long)dictSize(ctx->dbdata));
    
    /* Check if module pointer is valid */
    if (!module) {
        serverLog(LL_WARNING, "tryAutoInitFromModuleState: NULL module pointer");
        return EXTERNAL_ERROR;
    }
    
    if (!module->module) {
        serverLog(LL_WARNING, "tryAutoInitFromModuleState: NULL module->module pointer");
        return EXTERNAL_ERROR;
    }
    
    const char *module_name = module->name;
    if (!module_name) {
        serverLog(LL_WARNING, "tryAutoInitFromModuleState: NULL module->name");
        return EXTERNAL_ERROR;
    }
    
    serverLog(LL_NOTICE, "tryAutoInitFromModuleState: module=%s, module->module=%p", module_name, (void*)module->module);
    
    /* Check if the module implements the get_state callback in filter methods */
    if (!module->filter_methods.get_state) {
        serverLog(LL_DEBUG, "Module %s does not implement filter get_state, skipping auto-init", module_name);
        return EXTERNAL_ERROR;
    }
    
    /* Create a temporary filter context for the callback */
    filterCtx temp_filter_ctx = {
        .state = VMEF_STATE_READY,
        .ext_data_timeout = server.ext_data_timeout,
        .ext_data_dump_status = VMEF_DUMP_STATE_NONE,
        .ext_data_dump_backup_id = NULL,
        .ext_data_load_status = VMEF_LOAD_STATE_NONE,
    };
    
    serverLog(LL_NOTICE, "tryAutoInitFromModuleState: created temp_filter_ctx at %p", (void*)&temp_filter_ctx);
    
    /* Create a temporary module context */
    ValkeyModuleCtx *temp_module_ctx = moduleAllocateContext();
    if (!temp_module_ctx) {
        serverLog(LL_WARNING, "tryAutoInitFromModuleState: failed to allocate temp_module_ctx");
        return EXTERNAL_ERROR;
    }
    
    serverLog(LL_NOTICE, "tryAutoInitFromModuleState: allocated temp_module_ctx at %p", (void*)temp_module_ctx);
    
    moduleExternalFilterInitContext(temp_module_ctx, module->module);
    serverLog(LL_NOTICE, "tryAutoInitFromModuleState: initialized temp_module_ctx");
    
    serverLog(LL_NOTICE, "tryAutoInitFromModuleState: initialized temp_module_ctx");
    
    /* Determine source for get_state operation:
     * - NULL if we're on primary/standalone (query local state)
     * - Primary address (host:port) if we're on replica (query primary's state) */
    ValkeyModuleString *source = NULL;
    int is_replica_node = is_replica();
    
    serverLog(LL_NOTICE, "tryAutoInitFromModuleState: is_replica_node=%d", is_replica_node);
    
    if (is_replica_node) {
        /* We're a replica - create source string with primary address */
        char *primary_addr = get_primary_address();
        serverLog(LL_NOTICE, "tryAutoInitFromModuleState: is_replica=true, primary_addr=%s",
                  primary_addr ? primary_addr : "NULL");
        
        if (primary_addr) {
            source = (ValkeyModuleString *)createStringObject(primary_addr, strlen(primary_addr));
            serverLog(LL_NOTICE, "tryAutoInitFromModuleState: created source string at %p, len=%zu",
                      (void*)source, source ? sdslen((sds)objectGetVal(source)) : 0);
        }
    }
    
    /* Ask the module to get its state and which databases to initialize */
    int *db_numbers = NULL;
    size_t num_dbs = 0;
    
    serverLog(LL_NOTICE, "Calling module %s filter get_state with source=%s", module_name,
              source ? (char*)(objectGetVal(source)) : "NULL");
    
    /* Validate all parameters before calling module */
    serverLog(LL_NOTICE, "tryAutoInitFromModuleState: calling get_state with - temp_module_ctx=%p, &temp_filter_ctx=%p, source=%p, &db_numbers=%p, &num_dbs=%p",
              (void*)temp_module_ctx, (void*)&temp_filter_ctx, (void*)source, (void*)&db_numbers, (void*)&num_dbs);
    
    int result = module->filter_methods.get_state(temp_module_ctx, &temp_filter_ctx, source, &db_numbers, &num_dbs);
    
    serverLog(LL_NOTICE, "Module %s filter get_state returned: result=%d, num_dbs=%zu, db_numbers=%p",
              module_name, result, num_dbs, (void*)db_numbers);
    
    /* Also call storage get_state if available */
    int *storage_db_numbers = NULL;
    size_t storage_num_dbs = 0;
    if (module->storage_methods.get_state) {
        serverLog(LL_NOTICE, "Calling module %s storage get_state with source=%s", module_name,
                  source ? (char*)(objectGetVal(source)) : "NULL");
        
        /* Create a temporary storage context for the storage get_state call */
        storageCtx temp_storage_ctx = {
            .state = VMES_STATE_READY,
            .ext_data_timeout = server.ext_data_timeout,
            .ext_data_dump_status = VMES_DUMP_STATE_NONE,
            .ext_data_dump_backup_id = NULL,
            .ext_data_load_status = VMES_LOAD_STATE_NONE,
        };
        
        serverLog(LL_NOTICE, "tryAutoInitFromModuleState: created temp_storage_ctx at %p", (void*)&temp_storage_ctx);
        
        int storage_result = module->storage_methods.get_state(temp_module_ctx, &temp_storage_ctx, source, &storage_db_numbers, &storage_num_dbs);
        
        serverLog(LL_NOTICE, "Module %s storage get_state returned: result=%d, num_dbs=%zu, db_numbers=%p",
                  module_name, storage_result, storage_num_dbs, (void*)storage_db_numbers);
        
        /* If storage get_state succeeded and returned databases, use those instead */
        if (storage_result == EXTERNAL_SUCCESS && storage_db_numbers != NULL && storage_num_dbs > 0) {
            /* Free filter db_numbers if they were allocated */
            if (db_numbers) {
                serverLog(LL_NOTICE, "tryAutoInitFromModuleState: freeing filter db_numbers=%p", (void*)db_numbers);
                zfree(db_numbers);
                db_numbers = NULL;
            }
            db_numbers = storage_db_numbers;
            num_dbs = storage_num_dbs;
            result = storage_result;
            serverLog(LL_NOTICE, "tryAutoInitFromModuleState: using storage results instead of filter");
        } else {
            /* Free storage db_numbers if they were allocated but result was not successful */
            if (storage_db_numbers) {
                serverLog(LL_NOTICE, "tryAutoInitFromModuleState: freeing storage db_numbers=%p", (void*)storage_db_numbers);
                zfree(storage_db_numbers);
                storage_db_numbers = NULL;
            }
            /* Keep filter results as fallback */
            serverLog(LL_NOTICE, "tryAutoInitFromModuleState: keeping filter results as fallback");
        }
    }
    
    /* Free source string if we created it */
    if (source) {
        serverLog(LL_NOTICE, "tryAutoInitFromModuleState: freeing source string %p", (void*)source);
        decrRefCount((robj *)source);
        source = NULL;
    }
    
    /* Clean up temporary context */
    serverLog(LL_NOTICE, "tryAutoInitFromModuleState: freeing temp_module_ctx %p", (void*)temp_module_ctx);
    moduleFreeContext(temp_module_ctx);
    zfree(temp_module_ctx);
    temp_module_ctx = NULL;
    
    if (result == EXTERNAL_SUCCESS && db_numbers != NULL && num_dbs > 0) {
        serverLog(LL_NOTICE, "Module %s returned state, auto-initializing %zu database(s)",
                 module_name, num_dbs);
        
        /* Log which databases will be initialized */
        for (size_t i = 0; i < num_dbs; i++) {
            serverLog(LL_NOTICE, "  - Will initialize db%d", db_numbers[i]);
        }
        
        /* Initialize all databases specified by the module */
        serverLog(LL_NOTICE, "DEBUG: About to call initializeDatabasesFromState - ctx=%p, ctx->dbdata=%p, dictSize=%lu",
                  (void*)ctx, (void*)ctx->dbdata, (unsigned long)dictSize(ctx->dbdata));
        int init_result = initializeDatabasesFromState(ctx, module, db_numbers, num_dbs);
        serverLog(LL_NOTICE, "initializeDatabasesFromState returned: %d", init_result);
        serverLog(LL_NOTICE, "DEBUG: After initializeDatabasesFromState - ctx=%p, ctx->dbdata=%p, dictSize=%lu",
                  (void*)ctx, (void*)ctx->dbdata, (unsigned long)dictSize(ctx->dbdata));
        
        /* Free the db_numbers array allocated by the module using ValkeyModule_Alloc
         * ValkeyModule_Alloc uses zmalloc internally, so we can use zfree */
        if (db_numbers) {
            serverLog(LL_NOTICE, "tryAutoInitFromModuleState: freeing db_numbers=%p", (void*)db_numbers);
            zfree(db_numbers);
            db_numbers = NULL;
        }
        
        serverLog(LL_NOTICE, "=== tryAutoInitFromModuleState SUCCESS ===");
        return EXTERNAL_SUCCESS; /* Databases were initialized */
    }
    
    return EXTERNAL_ERROR;
}

/* Auto-initialize external data from module state
 * This is called when a module is registered to check if it has state to restore */
void externalDataAutoInitFromModule(externalDataModule *module) {
    if (!isExtDataOn()) {
        serverLog(LL_NOTICE, "externalDataAutoInitFromModule: external data is OFF, skipping");
        return;
    }
    
    int is_repl = is_replica();
    serverLog(LL_NOTICE, "Checking module %s for state to auto-initialize (is_replica=%d)", module->name, is_repl);
    
    /* Try to auto-initialize from module state */
    int init_result = tryAutoInitFromModuleState(module);
    
    /* If auto-init failed, queue for deferred retry */
    if (init_result != EXTERNAL_SUCCESS && is_repl) {
        /* On failure, queue for deferred retry */
        serverLog(LL_NOTICE, "Auto-init failed for module %s, queueing for deferred retry", module->name);
        queueDeferredInit(module->name, "module not ready or missing state");
    }
    
    if (server.initial_memory_usage == 0) {
        serverLog(LL_NOTICE, "Initial start detected, skipping load");
        return;
    }

    if (init_result == EXTERNAL_SUCCESS && is_repl) {
        serverLog(LL_NOTICE, "Module %s initialized successfully on replica, triggering processExternalDataLoadForFullSync", module->name);
        /* For replica, trigger load after initialization */
        processExternalDataLoadForFullSync();
    } else if (is_repl && server.repl_state == REPL_STATE_CONNECTED) {
        /* Special case: replica with active connection but auto-init didn't find/create databases
         * This happens when module loads after initial sync completed.
         * Trigger load process anyway - processExternalDataLoadForFullSync will handle
         * auto-initialization from modules when db_count==0 */
        serverLog(LL_NOTICE, "Module %s loaded on connected replica (init_result=%d), triggering processExternalDataLoadForFullSync for late-load scenario",
                  module->name, init_result);
        processExternalDataLoadForFullSync();
    } else {
        serverLog(LL_NOTICE, "Module %s auto-init result: init_result=%d, is_replica=%d, repl_state=%d - NOT triggering load",
                  module->name, init_result, is_repl, server.repl_state);
    }
}

