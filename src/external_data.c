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
    dict *filters;     /* Filter module name -> Filter module object */
    size_t cache_memory; /* Overhead memory (structs, dictionaries, ..) used by all the modules */
    dict *modules_stats; /* Per module statistics */
};

typedef struct externalStorage {
    sds name;                    /* Name of the storage */
    ValkeyModule *module;        /* the module that implements the external storage */
} externalStorage;

typedef struct externalFilter {
    sds name;                    /* Name of the filter */
    ValkeyModule *module;        /* the module that implements the external filter */
} externalFilter;


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

    if (dictFetchValue(curr_external_data_ctx->storages, storage_name_sds)) {
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

    if (dictFetchValue(curr_external_data_ctx->filters, filter_name_sds)) {
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

        externalStorage **storages = zmalloc(sizeof(externalStorage) * dictSize(curr_external_data_ctx->storages));
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

        externalFilter **filters = zmalloc(sizeof(externalFilter) * dictSize(curr_external_data_ctx->filters));
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
        zfree(filters);
    } else {
        addReplyError(c, "Unknown module type (storage or filter expected)");
        return;
    }
}

/* Initialize external data structures.
 * Should be called once on server initialization */
int externalDataInit(void) {
    if (isExtDataOn()) {
        curr_external_data_ctx = externalDataCtxCreate();
    }

    return C_OK;
}
