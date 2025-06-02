#include "external_data.h"

#include "dict.h"
#include "sds.h"
#include "server.h"
#include "valkeymodule.h"
#include "zmalloc.h"
#include <assert.h>
#include <strings.h>
#include "module.h"
#include <stdatomic.h>

const char *extDataOffErrStr = "External data commands are unavailable with ext-data-mode off";

/* Forward declaration */
static void moduleStatsDispose(void *obj);

struct externalDataCtx {
    dict *storages;      /* Storage module name -> Storage module object */
    dict *filters;       /* Filter module name -> Filter module object */
    dict *dbdata;        /* Database name -> Database data */
    size_t cache_memory; /* Overhead memory (structs, dictionaries, ..) used by all the modules */
    dict *modules_stats; /* Per module statistics */
};

typedef struct externalStorage {
    sds name;               /* Name of the storage */
    ValkeyModule *module;   /* The module that implements the storage */
    atomic_int used_count;  /* Counter for the storage usage */
    storageMethods methods; /* Callback functions implemented by the external storage module */
} externalStorage;

typedef struct externalStorageInstance {
    externalStorage *storage;    /* Storage struct */
    storageCtx *storage_ctx;     /* Storage specific context */
    ValkeyModuleCtx *module_ctx; /* Cache of the module context object */
} externalStorageInstance;

typedef struct externalFilter {
    sds name;              /* Name of the filter */
    ValkeyModule *module;  /* The module that implements the filter */
    atomic_int used_count; /* Counter for the filter usage */
    filterMethods methods; /* Callback functions implemented by the external filter module */
} externalFilter;

typedef struct externalFilterInstance {
    externalFilter *filter;      /* Filter struct */
    filterCtx *filter_ctx;       /* Filter specific context */
    ValkeyModuleCtx *module_ctx; /* Cache of the module context object */
} externalFilterInstance;

typedef struct externalDbData {
    externalStorageInstance *storage_instance; /* Storage instance used for a certain db */
    externalFilterInstance *filter_instance;   /* Filter instance used for a certain db */
} externalDbData;

typedef struct moduleStats {
    size_t n_dbs;
} moduleStats;

/* External data Ctx. */
static externalDataCtx *curr_external_data_ctx = NULL;

static uint64_t dictStrCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char *)key, strlen((char *)key));
}

dictType storageDictType = {
    dictStrCaseHash,       /* hash function */
    NULL,                  /* key dup */
    dictSdsKeyCaseCompare, /* key compare */
    NULL,                  /* key destructor */
    NULL,                  /* val destructor */
    NULL                   /* allow to expand */
};

dictType filterDictType = {
    dictStrCaseHash,       /* hash function */
    NULL,                  /* key dup */
    dictSdsKeyCaseCompare, /* key compare */
    NULL,                  /* key destructor */
    NULL,                  /* val destructor */
    NULL};

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
    ret->storages = dictCreate(&storageDictType);
    ret->filters = dictCreate(&filterDictType);
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

/* Registers a new external storage.
 *
 * - `storage_name`: the name of the external storage.
 *
 * - `ctx`: engine specific context pointer.
 *
 * Returns C_ERR in case of an error during registration.
 */
int externalStorageRegister(const char *storage_name,
                            ValkeyModule *storage_module,
                            storageMethods *storage_methods) {
    sds storage_name_sds = sdsnew(storage_name);

    if (dictFind(curr_external_data_ctx->storages, storage_name_sds)) {
        serverLog(LL_WARNING, "External storage '%s' is already registered in the server", storage_name_sds);
        sdsfree(storage_name_sds);
        return C_ERR;
    }

    externalStorage *e = zmalloc(sizeof(*e));
    *e = (externalStorage){
        .name = storage_name_sds,
        .module = storage_module,
        .methods = *storage_methods,
    };

    dictAdd(curr_external_data_ctx->storages, storage_name_sds, e);

    return C_OK;
}

/* Removes an external storage.
 *
 * - `storage_name`: name of the storage to remove
 */
int externalStorageUnregister(const char *storage_name) {
    dictEntry *entry = dictFind(curr_external_data_ctx->storages, storage_name);
    if (entry == NULL) {
        serverLog(LL_WARNING, "There's no storage registered with name %s", storage_name);
        return C_ERR;
    }

    externalStorage *e = dictGetVal(entry);
    if (e->used_count > 0) {
        serverLog(LL_WARNING, "It's impossible to remove used storage %s, drop it from all dbs first: %d", storage_name, e->used_count);
        return C_ERR;
    }

    dictDelete(curr_external_data_ctx->storages, storage_name);
    sdsfree(e->name);
    zfree(e);

    return C_OK;
}

/* Registers a new external filter.
 *
 * - `filter_name`: the name of the external filter.
 *
 * - `ctx`: engine specific context pointer.
 *
 * Returns C_ERR in case of an error during registration.
 */
int externalFilterRegister(const char *filter_name,
                           ValkeyModule *filter_module,
                           filterMethods *filter_methods) {
    sds filter_name_sds = sdsnew(filter_name);

    if (dictFind(curr_external_data_ctx->filters, filter_name_sds)) {
        serverLog(LL_WARNING, "External filter '%s' is already registered in the server", filter_name_sds);
        sdsfree(filter_name_sds);
        return C_ERR;
    }

    externalFilter *e = zmalloc(sizeof(*e));
    *e = (externalFilter){
        .name = filter_name_sds,
        .module = filter_module,
        .methods = *filter_methods,
    };

    dictAdd(curr_external_data_ctx->filters, filter_name_sds, e);

    return C_OK;
}

/* Removes an external filter.
 *
 * - `filter_name`: name of the filter to remove
 */
int externalFilterUnregister(const char *filter_name) {
    dictEntry *entry = dictFind(curr_external_data_ctx->filters, filter_name);
    if (entry == NULL) {
        serverLog(LL_WARNING, "There's no filter registered with name %s", filter_name);
        return C_ERR;
    }

    externalFilter *e = dictGetVal(entry);
    if (e->used_count > 0) {
        serverLog(LL_WARNING, "It's impossible to remove used filter %s, drop it from all dbs first", filter_name);
        return C_ERR;
    }

    dictDelete(curr_external_data_ctx->filters, filter_name);
    sdsfree(e->name);
    zfree(e);

    return C_OK;
}

int qsortCompareNames(const void *n1, const void *n2) {
    return strcmp(*(char **)n1, (*(char **)n2));
}

/*
 * EXTERNAL_DATA LOADED [STORAGE | FILTER]
 *
 * Return general information about storage or filter loaded modules:
 * * Module name
 *
 */
void externalDataLoadedCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, extDataOffErrStr);
        return;
    }

    assert(curr_external_data_ctx != NULL);
    int j = 2;
    if (!strcasecmp(objectGetVal(c->argv[j]), "storage")) {
        int size = dictSize(curr_external_data_ctx->storages);
        addReplyArrayLen(c, size);
        if (size == 0) {
            return;
        }

        sds storage_names[size];
        int num_storages = 0;
        dictIterator *storages_iter = dictGetIterator(curr_external_data_ctx->storages);
        dictEntry *storage_entry = NULL;
        while ((storage_entry = dictNext(storages_iter))) {
            externalStorage *es = dictGetVal(storage_entry);
            storage_names[num_storages++] = es->name;
        }
        dictReleaseIterator(storages_iter);

        qsort(storage_names, num_storages, sizeof(sds), qsortCompareNames);
        for (int i = 0; i < num_storages; i++) {
            addReplyBulkCString(c, storage_names[i]);
        }
        zfree(storage_names);
    } else if (!strcasecmp(objectGetVal(c->argv[j]), "filter")) {
        int size = dictSize(curr_external_data_ctx->filters);
        addReplyArrayLen(c, size);
        if (size == 0) {
            return;
        }

        sds filter_names[size];
        int num_filters = 0;
        dictIterator *filters_iter = dictGetIterator(curr_external_data_ctx->filters);
        dictEntry *filter_entry = NULL;
        while ((filter_entry = dictNext(filters_iter))) {
            externalFilter *es = dictGetVal(filter_entry);
            filter_names[num_filters++] = es->name;
        }
        dictReleaseIterator(filters_iter);

        qsort(filter_names, num_filters, sizeof(sds), qsortCompareNames);
        for (int i = 0; i < num_filters; i++) {
            addReplyBulkCString(c, filter_names[i]);
        }
    } else {
        addReplyError(c, "Unknown module type (storage or filter expected)");
        return;
    }
}

/*
 * EXTERNAL_DATA STATS [STORAGE | FILTER]
 *
 * Return general information about storage or filter loaded modules:
 * * Module name
 * * Databases list
 *
 */
void externalDataStatsCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, extDataOffErrStr);
        return;
    }

    assert(curr_external_data_ctx != NULL);
    int j = 2;

    if (!strcasecmp(objectGetVal(c->argv[j]), "storage") && !strcasecmp(objectGetVal(c->argv[j]), "filter")) {
        addReplyError(c, "Unknown module type (storage or filter expected)");
        return;
    }

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
        externalDbData *es = dictGetVal(dbdata_entry);

        if (!strcasecmp(objectGetVal(c->argv[j]), "storage")) {
            char line[sizeof(name) + sizeof(es->storage_instance->storage->name) + 2];
            snprintf(line, sizeof(line), "%s:%s", name, es->storage_instance->storage->name);
            lines[num_lines++] = sdsnew(line);
        }
        if (!strcasecmp(objectGetVal(c->argv[j]), "filter")) {
            char line[sizeof(name) + sizeof(es->filter_instance->filter->name) + 2];
            snprintf(line, sizeof(line), "%s:%s", name, es->filter_instance->filter->name);
            lines[num_lines++] = sdsnew(line);
        }
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
 * EXTERNAL_DATA INIT db STORAGE s FILTER f
 *
 * Init storage and filter modules for a certain db
 *
 */
void externalDataInitCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, extDataOffErrStr);
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

    sds storage_name = NULL;
    sds filter_name = NULL;
    for (int j = 3; j < c->argc; j += 2) {
        if (!strcasecmp(objectGetVal(c->argv[j]), "storage")) {
            o = c->argv[j + 1];
            storage_name = objectGetVal(o);
        }
        if (!strcasecmp(objectGetVal(c->argv[j]), "filter")) {
            o = c->argv[j + 1];
            filter_name = objectGetVal(o);
        }
    }

    externalStorage *storage = dictFetchValue(curr_external_data_ctx->storages, storage_name);
    if (!storage) {
        addReplyErrorFormat(c, "storage module %s is not loaded", storage_name);
        return;
    }

    externalFilter *filter = dictFetchValue(curr_external_data_ctx->filters, filter_name);
    if (!filter) {
        addReplyErrorFormat(c, "filter module %s is not loaded", filter_name);
        return;
    }

    storage->used_count++;
    filter->used_count++;

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

    externalStorageInstance *storage_instance = zmalloc(sizeof(*storage_instance));
    *storage_instance = (externalStorageInstance){
        .storage = storage,
        .storage_ctx = storage_ctx,
        .module_ctx = moduleAllocateContext(),
    };
    externalFilterInstance *filter_instance = zmalloc(sizeof(*filter_instance));
    *filter_instance = (externalFilterInstance){
        .filter = filter,
        .filter_ctx = filter_ctx,
        .module_ctx = moduleAllocateContext(),
    };

    externalDbData *e = zmalloc(sizeof(*e));
    *e = (externalDbData){
        .storage_instance = storage_instance,
        .filter_instance = filter_instance,
    };

    dictAdd(curr_external_data_ctx->dbdata, db_name_sds, e);

    addReply(c, shared.ok);
    return;
}

/*
 * EXTERNAL_DATA DROP db
 *
 * Drops storage and filter data for a certain db
 *
 */
void externalDataDropCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, extDataOffErrStr);
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

    zfree(dbData->storage_instance->storage_ctx);
    dbData->storage_instance->storage->used_count--;
    zfree(dbData->storage_instance->module_ctx);
    zfree(dbData->storage_instance);

    zfree(dbData->filter_instance->filter_ctx);
    dbData->filter_instance->filter->used_count--;
    zfree(dbData->filter_instance->module_ctx);
    zfree(dbData->filter_instance);

    zfree(dbData);
    dictDelete(curr_external_data_ctx->dbdata, dictGetKey(dbEntry));

    addReply(c, shared.ok);
    return;
}

static void storageSetupModuleCtx(externalStorageInstance *si) {
    if (si->storage->module != NULL) {
        serverAssert(si->module_ctx != NULL);
        moduleExternalStorageInitContext(si->module_ctx, si->storage->module);
    }
}

static void storageTeardownModuleCtx(externalStorageInstance *si) {
    if (si->storage->module != NULL) {
        serverAssert(si->module_ctx != NULL);
        moduleFreeContext(si->module_ctx);
    }
}

int externalStorageCallSetFunc(externalStorageInstance *si, int dbid, robj *key, robj *value) {
    storageSetupModuleCtx(si);

    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int success = si->storage->methods.set(si->module_ctx, si->storage_ctx, &key_ctx, value);

    storageTeardownModuleCtx(si);
    return success;
}

int externalStorageCallGetFunc(externalStorageInstance *si, int dbid, robj *key, void **found) {
    storageSetupModuleCtx(si);

    serverAssert(si->storage != NULL && si->storage_ctx != NULL && si->module_ctx != NULL && key != NULL);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int exists = si->storage->methods.get(si->module_ctx, si->storage_ctx, &key_ctx, found);

    storageTeardownModuleCtx(si);
    return exists;
}

int externalStorageCallDelFunc(externalStorageInstance *si, int dbid, robj *key, robj **value) {
    storageSetupModuleCtx(si);

    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int exists = si->storage->methods.del(si->module_ctx, si->storage_ctx, &key_ctx, value);

    storageTeardownModuleCtx(si);
    return exists;
}

void externalStorageCallSetReadonlyFunc(externalStorageInstance *si) {
    storageSetupModuleCtx(si);

    si->storage->methods.set_readonly(si->module_ctx, si->storage_ctx);

    storageTeardownModuleCtx(si);
    return;
}

void externalStorageCallDropReadonlyFunc(externalStorageInstance *si) {
    storageSetupModuleCtx(si);

    si->storage->methods.drop_readonly(si->module_ctx, si->storage_ctx);

    storageTeardownModuleCtx(si);
    return;
}

static void filterSetupModuleCtx(externalFilterInstance *fi) {
    if (fi->filter->module != NULL) {
        serverAssert(fi->module_ctx != NULL);
        moduleExternalFilterInitContext(fi->module_ctx, fi->filter->module);
    }
}

static void filterTeardownModuleCtx(externalFilterInstance *fi) {
    if (fi->filter->module != NULL) {
        serverAssert(fi->module_ctx != NULL);
        moduleFreeContext(fi->module_ctx);
    }
}

int externalFilterCallSetFunc(externalFilterInstance *fi, int dbid, robj *key) {
    filterSetupModuleCtx(fi);

    serverAssert(fi->filter != NULL && fi->filter_ctx != NULL && fi->module_ctx != NULL && key != NULL);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int success = fi->filter->methods.set(fi->module_ctx, fi->filter_ctx, &key_ctx);

    filterTeardownModuleCtx(fi);
    return success;
}

int externalFilterCallGetFunc(externalFilterInstance *fi, int dbid, robj *key) {
    filterSetupModuleCtx(fi);

    serverAssert(fi->filter != NULL && fi->filter_ctx != NULL && fi->module_ctx != NULL && key != NULL);
    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int exists = fi->filter->methods.get(fi->module_ctx, fi->filter_ctx, &key_ctx);

    filterTeardownModuleCtx(fi);
    return exists;
}

int externalFilterCallDelFunc(externalFilterInstance *fi, int dbid, robj *key, robj **value) {
    filterSetupModuleCtx(fi);

    ValkeyModuleKeyOptCtx key_ctx = {key, NULL, dbid, -1};
    int exists = fi->filter->methods.del(fi->module_ctx, fi->filter_ctx, &key_ctx, value);

    filterTeardownModuleCtx(fi);
    return exists;
}

void externalFilterCallSetReadonlyFunc(externalFilterInstance *fi) {
    filterSetupModuleCtx(fi);

    fi->filter->methods.set_readonly(fi->module_ctx, fi->filter_ctx);

    filterTeardownModuleCtx(fi);
    return;
}

void externalFilterCallDropReadonlyFunc(externalFilterInstance *fi) {
    filterSetupModuleCtx(fi);

    fi->filter->methods.drop_readonly(fi->module_ctx, fi->filter_ctx);

    filterTeardownModuleCtx(fi);
    return;
}

/*
 * EXTERNAL_DATA DEBUG db STORAGE|FILTER set|del k [v]
 *
 * Manipulate storage and filter data directly to debug a certain db
 *
 */
void externalDataDebugCommand(client *c) {
    if (!isExtDataOn()) {
        addReplyError(c, extDataOffErrStr);
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

    int j = 3;
    if (!strcasecmp(objectGetVal(c->argv[j]), "storage")) {
        j++;
        if (!strcasecmp(objectGetVal(c->argv[j]), "set")) {
            robj *key = c->argv[++j];
            robj *value = c->argv[++j];
            if (!externalStorageCallSetFunc(dbData->storage_instance, c->db->id, key, value)) {
                addReplyErrorFormat(c, "%s set failed", (char *)objectGetVal(key));
                return;
            }
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "del")) {
            robj *key = c->argv[++j];
            robj *value = NULL;
            int exists = externalStorageCallDelFunc(dbData->storage_instance, c->db->id, key, &value);
            if (exists && value != NULL) {
                addReplyBulkCString(c, objectGetVal(value));
            } else {
                addReplyBulkCString(c, "");
            }
            if (value != NULL) {
                decrRefCount(value);
            }
            return;
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "setro")) {
            externalStorageCallSetReadonlyFunc(dbData->storage_instance);
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "dropro")) {
            externalStorageCallDropReadonlyFunc(dbData->storage_instance);
        } else {
            sds cmd = objectGetVal(c->argv[j]);
            addReplyErrorFormat(c, "unknown subcommand %s", cmd);
            return;
        }
    } else if (!strcasecmp(objectGetVal(c->argv[j]), "filter")) {
        j++;
        if (!strcasecmp(objectGetVal(c->argv[j]), "set")) {
            robj *key = c->argv[++j];
            if (!externalFilterCallSetFunc(dbData->filter_instance, c->db->id, key)) {
                addReplyErrorFormat(c, "%s set failed", (char *)objectGetVal(key));
                return;
            }
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "del")) {
            robj *key = c->argv[++j];
            robj *value = NULL;
            int exists = externalFilterCallDelFunc(dbData->filter_instance, c->db->id, key, &value);
            if (exists && value != NULL) {
                addReplyBulkCString(c, objectGetVal(value));
            } else {
                addReplyBulkCString(c, "0");
            }
            if (value != NULL) {
                decrRefCount(value);
            }
            return;
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "setro")) {
            externalFilterCallSetReadonlyFunc(dbData->filter_instance);
        } else if (!strcasecmp(objectGetVal(c->argv[j]), "dropro")) {
            externalFilterCallDropReadonlyFunc(dbData->filter_instance);
        } else {
            sds cmd = objectGetVal(c->argv[j]);
            addReplyErrorFormat(c, "unknown subcommand %s", cmd);
            return;
        }
    } else {
        sds cmd = objectGetVal(c->argv[3]);
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
    externalFilterInstance *fi = dbData->filter_instance;
    if (!fi) return 0;
    if (!externalFilterCallGetFunc(fi, id, key)) return 0;

    externalStorageInstance *si = dbData->storage_instance;
    if (!si) return 0;
    return externalStorageCallGetFunc(si, id, key, found);
}

/* Initialize external data structures.
 * Should be called once on server initialization */
int externalDataInit(void) {
    if (isExtDataOn()) {
        curr_external_data_ctx = externalDataCtxCreate();
    }

    return C_OK;
}
