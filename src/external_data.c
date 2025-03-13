#include "external_data.h"

#include "dict.h"
#include "sds.h"
#include "server.h"
#include "zmalloc.h"
#include <assert.h>
#include <strings.h>
#include "module.h"

const char *extDataOffErrStr = "External data commands are unavailable with ext-data-mode off";

/* Forward declaration */
static void moduleStatsDispose(void *obj);

struct externalDataCtx {
    dict *storages;     /* Storage module name -> Storage module object */
    dict *filters;      /* Filter module name -> Filter module object */
    dict *dbdata;       /* Database name -> Database data */
    size_t cache_memory; /* Overhead memory (structs, dictionaries, ..) used by all the modules */
    dict *modules_stats; /* Per module statistics */
};

typedef struct externalStorage {
    sds name;                    /* Name of the storage */
    ValkeyModule *module;        /* the module that implements the external storage */
    int used_count;
} externalStorage;

typedef struct externalFilter {
    sds name;                    /* Name of the filter */
    ValkeyModule *module;        /* the module that implements the external filter */
    int used_count;
} externalFilter;

typedef struct externalDbData {
    sds storage_name;            /* Name of the storage used for a certain db */
    sds filter_name;             /* Name of the filter used for a certain db */
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
    NULL  
};

dictType dbdataDictType = {
    dictStrCaseHash,       /* hash function */
    NULL,                  /* key dup */
    dictSdsKeyCaseCompare, /* key compare */
    NULL,                  /* key destructor */
    NULL,                  /* val destructor */
    NULL  
};

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
    ValkeyModule *storage_module) {
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
    };

    dictAdd(curr_external_data_ctx->storages, storage_name_sds, e);

    return C_OK;
}

/* Removes an external storage.
 *
 * - `storage_name`: name of the storage to remove
 */
 int externalStorageUnregister(const char *storage_name) {
    dictEntry *entry = dictUnlink(curr_external_data_ctx->storages, storage_name);
    if (entry == NULL) {
        serverLog(LL_WARNING, "There's no storage registered with name %s", storage_name);
        return C_ERR;
    }

    externalStorage *e = dictGetVal(entry);
    if (e->used_count > 0) {
        serverLog(LL_WARNING, "It's impossible to remove used storage %s, drop it from all dbs first", storage_name);
        return C_ERR;
    }

    sdsfree(e->name);
    zfree(e);

    dictFreeUnlinkedEntry(curr_external_data_ctx->storages, entry);

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
    ValkeyModule *filter_module) {
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
    };

    dictAdd(curr_external_data_ctx->filters, filter_name_sds, e);

    return C_OK;
}

/* Removes an external filter.
 *
 * - `filter_name`: name of the filter to remove
 */
 int externalFilterUnregister(const char *filter_name) {
    dictEntry *entry = dictUnlink(curr_external_data_ctx->filters, filter_name);
    if (entry == NULL) {
        serverLog(LL_WARNING, "There's no filter registered with name %s", filter_name);
        return C_ERR;
    }

    externalFilter *e = dictGetVal(entry);
    if (e->used_count > 0) {
        serverLog(LL_WARNING, "It's impossible to remove used filter %s, drop it from all dbs first", filter_name);
        return C_ERR;
    }

    sdsfree(e->name);
    zfree(e);

    dictFreeUnlinkedEntry(curr_external_data_ctx->filters, entry);

    return C_OK;
}

int qsortCompareNames(const void *n1, const void *n2) {
    // return *(char **)n1 > (*(char **)n2);
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

    assert(curr_external_data_ctx!=NULL);
    int j = 2;
    if (!strcasecmp(objectGetVal(c->argv[j]), "storage")) {
        int size = dictSize(curr_external_data_ctx->storages);
        addReplyArrayLen(c, size);
        if (size == 0) {
            return;
        }

        externalStorage *storages[size];
        int num_storages = 0;
        dictIterator *storages_iter = dictGetIterator(curr_external_data_ctx->storages);
        dictEntry *storage_entry = NULL;
        while ((storage_entry = dictNext(storages_iter))) {
            externalStorage *es = dictGetVal(storage_entry);
            storages[num_storages++] = es;
        }
        dictReleaseIterator(storages_iter);

        qsort(storages, num_storages, sizeof(externalStorage *), qsortCompareNames);
        for (int i = 0; i < num_storages; i++) {
            addReplyBulkCString(c, storages[i]->name);
        }
        zfree(storages);
    } else if (!strcasecmp(objectGetVal(c->argv[j]), "filter")) {
        int size = dictSize(curr_external_data_ctx->filters);
        addReplyArrayLen(c, size);
        if (size == 0) {
            return;
        }

        externalFilter *filters[size];
        int num_filters = 0;
        dictIterator *filters_iter = dictGetIterator(curr_external_data_ctx->filters);
        dictEntry *filter_entry = NULL;
        while ((filter_entry = dictNext(filters_iter))) {
            externalFilter *es = dictGetVal(filter_entry);
            filters[num_filters++] = es;
        }
        dictReleaseIterator(filters_iter);

        qsort(filters, num_filters, sizeof(externalFilter *), qsortCompareNames);
        for (int i = 0; i < num_filters; i++) {
            addReplyBulkCString(c, filters[i]->name);
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

    assert(curr_external_data_ctx!=NULL);
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
            char line[sizeof(name)+sizeof(es->storage_name)+2];
            snprintf(line, sizeof(line), "%s:%s", name, es->storage_name);
            lines[num_lines++] = sdsnew(line);
        }
        if (!strcasecmp(objectGetVal(c->argv[j]), "filter")) {
            char line[sizeof(name)+sizeof(es->filter_name)+2];
            snprintf(line, sizeof(line), "%s:%s", name, es->filter_name);
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
    assert(curr_external_data_ctx!=NULL);

    robj *o = c->argv[2];
    sds db_name = objectGetVal(o);
    int db_num;
    if (sscanf(db_name,"db%d",&db_num) != 1) {
        addReplyErrorFormat(c, "failed to parse db number from %s, expect db0, db10, etc.", db_name);
        return;
    }
    if (db_num >= server.dbnum) {
        addReplyErrorFormat(c, "db number %d exceeds used on server 0-%d", db_num, server.dbnum-1);
        return;
    }

    if (dictFind(curr_external_data_ctx->dbdata, db_name)) {
        addReplyErrorFormat(c, "%s is already initialized", db_name);
        return;
    }

    sds storage_name = NULL;
    sds filter_name = NULL;
    for (int j = 3; j < c->argc; j+=2) {
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
    storage->used_count++;

    externalFilter *filter = dictFetchValue(curr_external_data_ctx->filters, filter_name);
    if (!filter) {
        addReplyErrorFormat(c, "filter module %s is not loaded", filter_name);
        return;
    }
    filter->used_count++;

    sds db_name_sds = sdsnew(db_name);
    sds storage_name_sds = sdsnew(storage_name);
    sds filter_name_sds = sdsnew(filter_name);

    externalDbData *e = zmalloc(sizeof(*e));
    *e = (externalDbData){
        .storage_name = storage_name_sds,
        .filter_name = filter_name_sds,
    };

    dictAdd(curr_external_data_ctx->dbdata, db_name_sds, e);

    addReply(c, shared.ok);
    return;
}

/* Initialize external data structures.
 * Should be called once on server initialization */
int externalDataInit(void) {
    if (isExtDataOn()) {
        curr_external_data_ctx = externalDataCtxCreate();
    }

    return C_OK;
}
