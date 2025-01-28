#include "external_data.h"

#include "dict.h"
#include "sds.h"
#include "server.h"
#include "zmalloc.h"
#include <assert.h>
#include <strings.h>

const char *extDataOffErrStr = "External data commands are unavailable with ext-data-mode off";

/* Forward declaration */
static void filterDispose(void *obj);
static void storageDispose(void *obj);
static void moduleStatsDispose(void *obj);

struct externalDataCtx {
    dict *storages;     /* Storage module name -> Storage module object */
    dict *filters;     /* Filter module name -> Filter module object */
    size_t cache_memory; /* Overhead memory (structs, dictionaries, ..) used by all the modules */
    dict *modules_stats; /* Per module statistics */
};

typedef struct moduleStats {
    size_t n_dbs;
} moduleStats;

/* External data Ctx. */
static externalDataCtx *curr_external_data_ctx = NULL;

dictType storageDictType = {
    dictSdsHash,          /* hash function */
    dictSdsDup,           /* key dup */
    dictSdsKeyCompare,    /* key compare */
    dictSdsDestructor,    /* key destructor */
    storageDispose, /* val destructor */
    NULL                  /* allow to expand */
};

dictType filterDictType = {
    dictSdsHash,           /* hash function */
    dictSdsDup,            /* key dup */
    dictSdsKeyCompare,     /* key compare */
    dictSdsDestructor,     /* key destructor */
    filterDispose, /* val destructor */
    NULL                   /* allow to expand */
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

/* Dispose storage memory */
static void storageDispose(void *obj) {
    if (!obj) {
        return;
    }
}


/* Dispose filter memory */
static void filterDispose(void *obj) {
    if (!obj) {
        return;
    }
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
        addReplyArrayLen(c, dictSize(curr_external_data_ctx->storages));
    } else if (!strcasecmp(objectGetVal(c->argv[j]), "filter")) {
        addReplyArrayLen(c, dictSize(curr_external_data_ctx->filters));
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
