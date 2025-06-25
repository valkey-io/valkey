#ifndef _ENTRY_H_
#define _ENTRY_H_

#include "sds.h"
#include <stdbool.h>

typedef void entry;

sds entryGetField(const entry *entry);
sds entryGetValue(const entry *entry);
entry *entrySetValue(entry *entry, sds value);
long long entryGetExpiry(const entry *entry);
bool entryHasExpiry(const entry *entry);
entry *entrySetExpiry(entry *entry, long long expiry);
bool entryIsExpired(entry *entry);

void entryFree(entry *entry);
entry *entryCreate(const_sds field, sds value, long long expiry);
entry *entryUpdate(entry *entry, sds value, long long expiry);
size_t entryMemUsage(entry *entry);
entry *entryDefrag(entry *entry, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds));
void entryDismissMemory(entry *entry);

/* Internal used for debug. No need to use this function except in tests */
bool entryHasEmbeddedValue(entry *entry);

#endif
