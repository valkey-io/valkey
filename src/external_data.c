#include "external_data.h"

#include "bio.h"
#include "dict.h"
#include "sds.h"
#include "server.h"
#include "valkeymodule.h"
#include "zmalloc.h"
#include <assert.h>
#include <stdlib.h>
#include <strings.h>
#include "module.h"
#include <stdatomic.h>
#include <time.h>

/* Forward declaration */
static void moduleStatsDispose(void *obj);

struct externalDataCtx {
    dict *modules;       /* Module name -> Module object */
    dict *dbdata;        /* Database name -> Database data */
    size_t cache_memory; /* Overhead memory (structs, dictionaries, ..) used by all the modules */
    dict *modules_stats; /* Per module statistics */
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
    return ret;
}

/* Dispose stats memory */
static void moduleStatsDispose(void *obj) {
    moduleStats *stats = obj;
    zfree(stats);
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
    sds name_sds = sdsnew(name);

    if (dictFind(curr_external_data_ctx->modules, name_sds)) {
        serverLog(LL_WARNING, "External module '%s' is already registered in the server", name_sds);
        sdsfree(name_sds);
        return C_ERR;
    }

    externalDataModule *m = zmalloc(sizeof(*m));
    *m = (externalDataModule){
        .name = name_sds,
        .module = module,
        .storage_methods = *storage_methods,
        .filter_methods = *filter_methods,
    };

    dictAdd(curr_external_data_ctx->modules, name_sds, m);

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

    assert(curr_external_data_ctx != NULL);

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

    assert(curr_external_data_ctx != NULL);

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
    char db_name[14];
    snprintf(db_name, sizeof(db_name), "db%d", db_num);
    return sdsnew(db_name);
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
    assert(curr_external_data_ctx != NULL);

    robj *o = c->argv[2];
    sds db_name = objectGetVal(o);
    if (checkDbNum(c, db_name) < 0) {
        return;
    }

    if (dictFind(curr_external_data_ctx->dbdata, db_name)) {
        addReplyErrorFormat(c, "%s is already initialized", db_name);
        return;
    }

    sds module_name = objectGetVal(c->argv[3]);
    externalDataModule *module = dictFetchValue(curr_external_data_ctx->modules, module_name);
    if (!module) {
        addReplyErrorFormat(c, "module %s is not loaded", module_name);
        return;
    }
    module->used_count++;

    sds db_name_sds = sdsnew(db_name);

    storageCtx *storage_ctx = zmalloc(sizeof(*storage_ctx));
    *storage_ctx = (storageCtx){
        .state = VMES_STATE_READY,
        .ext_data_timeout = server.ext_data_timeout,
    };
    filterCtx *filter_ctx = zmalloc(sizeof(*filter_ctx));
    *filter_ctx = (filterCtx){
        .state = VMEF_STATE_READY,
        .ext_data_timeout = server.ext_data_timeout,
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

    dictAdd(curr_external_data_ctx->dbdata, db_name_sds, e);

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
    assert(curr_external_data_ctx != NULL);

    robj *o = c->argv[2];
    sds db_name = objectGetVal(o);
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

    zfree(dbData->module_instance->storage_ctx);
    zfree(dbData->module_instance->filter_ctx);
    dbData->module_instance->external_module->used_count--;
    /* Free the module context - this will flush any pending logs */
    if (dbData->module_instance->module_ctx != NULL) {
        moduleFreeContext(dbData->module_instance->module_ctx);
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

    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int success = mi->external_module->storage_methods.set(mi->module_ctx, mi->storage_ctx, &key_ctx, value);

    teardownModuleCtx(mi);
    return success;
}

int externalStorageCallGetFunc(externalDataModuleInstance *si, int dbid, robj *key, void **found) {
    setupModuleCtx(si);

    serverAssert(si->external_module != NULL && si->storage_ctx != NULL && si->module_ctx != NULL && key != NULL);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int exists = si->external_module->storage_methods.get(si->module_ctx, si->storage_ctx, &key_ctx, found);

    teardownModuleCtx(si);
    return exists;
}

int externalStorageCallDelFunc(externalDataModuleInstance *mi, int dbid, robj *key, robj **value) {
    setupModuleCtx(mi);

    serverAssert(mi->external_module != NULL && mi->storage_ctx != NULL && mi->module_ctx != NULL && key != NULL);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int result = mi->external_module->storage_methods.del(mi->module_ctx, mi->storage_ctx, &key_ctx, value);

    teardownModuleCtx(mi);
    return result == EXTERNAL_SUCCESS;
}

int externalFilterCallSetFunc(externalDataModuleInstance *fi, int dbid, robj *key) {
    setupModuleCtx(fi);

    serverAssert(fi->external_module != NULL && fi->filter_ctx != NULL && fi->module_ctx != NULL && key != NULL);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int success = fi->external_module->filter_methods.set(fi->module_ctx, fi->filter_ctx, &key_ctx);

    teardownModuleCtx(fi);
    return success;
}

int externalFilterCallGetFunc(externalDataModuleInstance *fi, int dbid, robj *key) {
    setupModuleCtx(fi);

    serverAssert(fi->external_module != NULL && fi->filter_ctx != NULL && fi->module_ctx != NULL && key != NULL);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int exists = fi->external_module->filter_methods.get(fi->module_ctx, fi->filter_ctx, &key_ctx);

    teardownModuleCtx(fi);
    return exists;
}

int externalFilterCallDelFunc(externalDataModuleInstance *fi, int dbid, robj *key, robj **value) {
    setupModuleCtx(fi);

    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
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

struct extStorageInstanceIterator {
    externalDbData *dbdata;
    ValkeyModuleString *match;
    long long *type;
    int dbid;
    ValkeyModuleDictIter *iter;
};

externalStorageInstanceIterator *externalStorageInstanceIteratorInit(int dbid, robj *match, long long *type) {
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
    assert(curr_external_data_ctx != NULL);

    robj *o = c->argv[2];
    sds db_name = objectGetVal(o);
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
            } else {
                addReplyBulkCString(c, "");
            }
            if (value != NULL) {
                decrRefCount(value);
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
            } else {
                addReplyBulkCString(c, "0");
            }
            if (value != NULL) {
                decrRefCount(value);
            }
            return;
        } else {
            sds cmd = objectGetVal(c->argv[j]);
            addReplyErrorFormat(c, "unknown subcommand %s", cmd);
            return;
        }
    } else if (!strcasecmp(c->argv[j]->ptr, "setro")) {
        int result = externalDataCallSetReadonlyFunc(dbData->module_instance);
        if (result != EXTERNAL_SUCCESS) {
            addReplyErrorFormat(c, "error code setting readonly: %d", result);
            return;
        }
    } else if (!strcasecmp(c->argv[j]->ptr, "dropro")) {
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

int externalDataFind(int id, void *key, void **found) {
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
    sds db_name = getDBName(id);
    dictEntry *db = dictFind(curr_external_data_ctx->dbdata, db_name);
    sdsfree(db_name);
    if (!db) return EXTERNAL_ERROR;

    externalDbData *dbData = dictGetVal(db);
    externalDataModuleInstance *mi = dbData->module_instance;
    if (!mi) return EXTERNAL_ERROR;

    // Check if both storage and filter are in readonly state
    ValkeyModuleExternalStorageState storage_state = mi->storage_ctx->state;
    ValkeyModuleExternalFilterState filter_state = mi->filter_ctx->state;
    
    // If both are readonly, return 2 to signal the client should be blocked
    // The client will be retried later when the state changes
    if (storage_state == VMES_STATE_READONLY || filter_state == VMEF_STATE_READONLY) {
        return EXTERNAL_READONLY;  // Signal to block the client
    }

    if (externalStorageCallSetFunc(mi, id, key, value) != EXTERNAL_SUCCESS) return EXTERNAL_ERROR;
    return externalFilterCallSetFunc(mi, id, key);
}

/* Initialize external data structures.
 * Should be called once on server initialization */
int externalDataInit(void) {
    if (isExtDataOn()) {
        curr_external_data_ctx = externalDataCtxCreate();
    }

    return C_OK;
}

/* Get current external data context */
externalDataCtx *getCurrentExternalDataCtx(void) {
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
