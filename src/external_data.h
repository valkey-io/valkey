#ifndef __EXTERNAL_DATA_H_
#define __EXTERNAL_DATA_H_

#include "server.h"
#include "valkeymodule.h"

#define EXTDATAOFFERRMSG "External data commands are unavailable with ext-data-mode off"

// Forward declaration of the external storage structures.
typedef struct externalStorage externalStorage;
typedef struct externalStorageInstance externalStorageInstance;
// Forward declaration of the external filter structures.
typedef struct externalFilter externalFilter;
typedef struct externalFilterInstance externalFilterInstance;

typedef struct ValkeyModule ValkeyModule;
/* ValkeyModule type aliases for external storage structs and types. */
typedef ValkeyModuleExternalStorageCtx storageCtx;
typedef ValkeyModuleExternalStorageMethods storageMethods;
typedef ValkeyModuleExternalStorageState storageState;
/* ValkeyModule type aliases for external storage structs and types. */
typedef ValkeyModuleExternalFilterCtx filterCtx;
typedef ValkeyModuleExternalFilterMethods filterMethods;
typedef ValkeyModuleExternalFilterState filterState;

/* Functions */
int externalStorageCallSetFunc(externalStorageInstance *si, int dbid, robj *key, robj *value);
int externalStorageCallGetFunc(externalStorageInstance *fi, int dbid, robj *key, void **found);
int externalStorageCallDelFunc(externalStorageInstance *si, int dbid, robj *key, robj **value);
void externalStorageCallSetReadonlyFunc(externalStorageInstance *si);
void externalStorageCallDropReadonlyFunc(externalStorageInstance *si);
int externalStorageRegister(const char *storage_name, ValkeyModule *storage_module, storageMethods *storage_methods);
int externalStorageUnregister(const char *storage_name);

externalStorageInstanceIterator *externalStorageInstanceIteratorInit(int dbid, robj *match, long long *type);
int externalStorageInstanceIteratorNext(externalStorageInstanceIterator *esi_it, robj **next);
void externalStorageInstanceIteratorRelease(externalStorageInstanceIterator *esi_it);

int externalFilterCallSetFunc(externalFilterInstance *fi, int dbid, robj *key);
int externalFilterCallGetFunc(externalFilterInstance *fi, int dbid, robj *key);
int externalFilterCallDelFunc(externalFilterInstance *fi, int dbid, robj *key, robj **value);
void externalFilterCallSetReadonlyFunc(externalFilterInstance *fi);
void externalFilterCallDropReadonlyFunc(externalFilterInstance *fi);
int externalFilterRegister(const char *filter_name, ValkeyModule *filter_module, filterMethods *filter_methods);
int externalFilterUnregister(const char *filter_name);

int externalFilterIsIn(int id, void *key);

/* Core used methods */
int externalDataInit(void);
int externalDataFind(int id, void *key, void **found);

#endif /* __EXTERNAL_DATA_H_ */
