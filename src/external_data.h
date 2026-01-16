#ifndef __EXTERNAL_DATA_H_
#define __EXTERNAL_DATA_H_

#include "server.h"
#include "valkeymodule.h"

#define EXTDATAOFFERRMSG "External data commands are unavailable with ext-data-mode off"

// Forward declaration of the new module-based structures
typedef struct externalDataModule externalDataModule;
typedef struct externalDataModuleInstance externalDataModuleInstance;
typedef struct externalDbData externalDbData;
typedef struct externalDataCtx externalDataCtx;
typedef struct extStorageInstanceIterator externalStorageInstanceIterator;

typedef struct ValkeyModule ValkeyModule;
/* ValkeyModule type aliases for external storage structs and types */
typedef ValkeyModuleExternalStorageCtx storageCtx;
typedef ValkeyModuleExternalStorageMethods storageMethods;
typedef ValkeyModuleExternalStorageState storageState;
/* ValkeyModule type aliases for external filter structs and types */
typedef ValkeyModuleExternalFilterCtx filterCtx;
typedef ValkeyModuleExternalFilterMethodsV2 filterMethods;
typedef ValkeyModuleExternalFilterState filterState;

/* New module registration functions */
int externalDataModuleRegister(const char *name, ValkeyModule *module, storageMethods *storage_methods, filterMethods *filter_methods);
int externalDataModuleUnregister(const char *name);

/* Functions */
int externalStorageCallSetFunc(externalDataModuleInstance *si, int dbid, robj *key, robj *value);
int externalStorageCallGetFunc(externalDataModuleInstance *fi, int dbid, robj *key, void **found);
int externalStorageCallDelFunc(externalDataModuleInstance *si, int dbid, robj *key, robj **value);

externalStorageInstanceIterator *externalStorageInstanceIteratorInit(int dbid, robj *match, long long *type);
int externalStorageInstanceIteratorNext(externalStorageInstanceIterator *esi_it, robj **next);
void externalStorageInstanceIteratorRelease(externalStorageInstanceIterator *esi_it);

int externalFilterCallSetFunc(externalDataModuleInstance *fi, int dbid, robj *key);
int externalFilterCallGetFunc(externalDataModuleInstance *fi, int dbid, robj *key);
int externalFilterCallDelFunc(externalDataModuleInstance *fi, int dbid, robj *key, robj **value);
int externalDataCallSetReadonlyFunc(externalDataModuleInstance *mi);
int externalDataCallDropReadonlyFunc(externalDataModuleInstance *mi);
int externalDataCallDumpFunc(externalDataModuleInstance *mi, int slot, long long timestamp, ValkeyModuleString *target, ValkeyModuleString **backup_id);
int externalDataCallLoadFunc(externalDataModuleInstance *mi, ValkeyModuleString *backup_id, ValkeyModuleString *source);
int externalDataCallGetStateFunc(externalDataModuleInstance *mi, ValkeyModuleString *source, int **db_numbers, size_t *num_dbs);
int externalDataDumpForFullSync(void);
int externalDataDumpCheckComplete(ValkeyModuleString **backup_id);
int externalDataLoadForFullSync(void);
int externalDataLoadCheckComplete(void);
void bioExternalDataDumpForFullSync(void);
void bioExternalDataLoadForFullSync(void);
void processExternalDataLoadForFullSync(void);

int externalFilterIsIn(int id, void *key);

/* Core used methods */
int externalDataFind(int id, void *key, void **found);
int externalDataWrite(int id, void *key, void *value);

/* Access to external data context */
externalDataCtx *getCurrentExternalDataCtx(void);

/* Delete a key from external storage for a specific database */
int externalStorageDeleteKey(int dbid, robj *key, robj **value);

/* Delete a key from external filter for a specific database */
int externalFilterDeleteKey(int dbid, robj *key, robj **value);

/* Flush external data for a specific database */
void externalDataFlushDb(int dbid);

/* Flush external data for all databases */
void externalDataFlushAll(void);

/* Swap external data between two databases */
void externalDataSwapDb(int id1, int id2);

/* Utility functions */
sds getDBName(int db_num);

/* Count external data keys for a specific database */
unsigned long long externalDataCountKeys(int dbid);

#endif /* __EXTERNAL_DATA_H_ */
