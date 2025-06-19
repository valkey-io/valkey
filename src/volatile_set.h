#ifndef VOLATILESET_H
#define VOLATILESET_H

#include <stddef.h>
#include "rax.h"
#include "sds.h"

typedef struct {
    sds (*entryGetKey)(const void *entry);

    long long (*getExpiry)(const void *entry);

    int (*expire)(void *entry);

} volatileEntryType;


typedef struct {
    volatileEntryType *etypr;
    rax *expiry_buckets;
} volatile_set;

typedef struct volatileSetIterator {
    raxIterator bucket;
} volatileSetIterator;


int volatileSetRemoveEntry(volatile_set *set, void *entry, long long expiry);
int volatileSetAddEntry(volatile_set *set, void *entry, long long expiry);
int volatileSetExpireEntry(volatile_set *set, void *entry);
int volatileSetUpdateEntry(volatile_set *set, void *old_entry, void *new_entry, long long old_expiry, long long new_expiry);
size_t volatileSetNumEntries(volatile_set *set);
void volatileSetStart(volatile_set *set, volatileSetIterator *it);
int volatileSetNext(volatileSetIterator *it, void **entryptr);
void volatileSetReset(volatileSetIterator *it);

void freeVolatileSet(volatile_set *b);
volatile_set *createVolatileSet(volatileEntryType *type);

#endif
