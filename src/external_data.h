#ifndef __EXTERNAL_DATA_H_
#define __EXTERNAL_DATA_H_

#include "server.h"
#include "valkeymodule.h"

// Forward declaration of the external storage structure.
typedef struct externalStorage externalStorage;
typedef struct externalFilter externalFilter;

/* ValkeyModule type aliases for external storage structs and types. */
typedef struct ValkeyModule ValkeyModule;

int externalStorageRegister(const char *storage_name, ValkeyModule *storage_module);
int externalStorageUnregister(const char *storage_name);
int externalFilterRegister(const char *filter_name, ValkeyModule *filter_module);
int externalFilterUnregister(const char *filter_name);
int externalDataInit(void);

#endif /* __EXTERNAL_DATA_H_ */