#ifndef KVSTORE_H
#define KVSTORE_H

#include "hashtable.h"
#include "adlist.h"

typedef struct _kvstore kvstore;
typedef struct _kvstoreIterator kvstoreIterator;
typedef struct _kvstoreHashtableIterator kvstoreHashtableIterator;

/* Return 1 if we should skip the hashtable in scan. */
typedef int(kvstoreScanShouldSkipHashtable)(hashtable *d);
/* Return 1 if we should skip the hashtable in expand. */
typedef int(kvstoreExpandShouldSkipHashtableIndex)(int didx);
typedef void (*kvstoreScanFunction)(void *privdata, void *entry, int didx);

#define KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND (1 << 0)
#define KVSTORE_FREE_EMPTY_HASHTABLES (1 << 1)

#define KVSTORE_INDEX_NOT_FOUND (-1)

kvstore *kvstoreCreate(hashtableType *type, int num_hashtables_bits, int flags);
void kvstoreEmpty(kvstore *kvs, void(callback)(hashtable *));
void kvstoreRelease(kvstore *kvs);
unsigned long long kvstoreSize(kvstore *kvs);
unsigned long long kvstoreImportingSize(kvstore *kvs);
unsigned long kvstoreBuckets(kvstore *kvs);
size_t kvstoreMemUsage(kvstore *kvs);
unsigned long long kvstoreScan(kvstore *kvs,
                               unsigned long long cursor,
                               int onlydidx,
                               kvstoreScanFunction scan_cb,
                               kvstoreScanShouldSkipHashtable *skip_cb,
                               void *privdata);
bool kvstoreExpand(kvstore *kvs, uint64_t newsize, int try_expand, kvstoreExpandShouldSkipHashtableIndex *skip_cb);
int kvstoreGetFairRandomHashtableIndex(kvstore *kvs);
void kvstoreGetStats(kvstore *kvs, char *buf, size_t bufsize, int full);

int kvstoreFindHashtableIndexByKeyIndex(kvstore *kvs, unsigned long target);
int kvstoreGetFirstNonEmptyHashtableIndex(kvstore *kvs);
int kvstoreGetNextNonEmptyHashtableIndex(kvstore *kvs, int didx);
int kvstoreNumNonEmptyHashtables(kvstore *kvs);
int kvstoreNumAllocatedHashtables(kvstore *kvs);
int kvstoreNumHashtables(kvstore *kvs);
uint64_t kvstoreGetHash(kvstore *kvs, const void *key);

void kvstoreHashtableRehashingStarted(hashtable *d);
void kvstoreHashtableRehashingCompleted(hashtable *d);
void kvstoreHashtableTrackMemUsage(hashtable *s, ssize_t delta);
size_t kvstoreHashtableMetadataSize(void);

/* kvstore iterator specific functions */
kvstoreIterator *kvstoreIteratorInit(kvstore *kvs, uint8_t flags);
void kvstoreIteratorRelease(kvstoreIterator *kvs_it);
int kvstoreIteratorGetCurrentHashtableIndex(kvstoreIterator *kvs_it);
bool kvstoreIteratorNext(kvstoreIterator *kvs_it, void **next);

/* Rehashing */
void kvstoreTryResizeHashtables(kvstore *kvs, int limit);
uint64_t kvstoreIncrementallyRehash(kvstore *kvs, uint64_t threshold_us);
size_t kvstoreOverheadHashtableLut(kvstore *kvs);
size_t kvstoreOverheadHashtableRehashing(kvstore *kvs);
unsigned long kvstoreHashtableRehashingCount(kvstore *kvs);

/* Specific hashtable access by hashtable-index */
unsigned long kvstoreHashtableSize(kvstore *kvs, int didx);
kvstoreHashtableIterator *kvstoreGetHashtableIterator(kvstore *kvs, int didx, uint8_t flags);
void kvstoreReleaseHashtableIterator(kvstoreHashtableIterator *kvs_id);
bool kvstoreHashtableIteratorNext(kvstoreHashtableIterator *kvs_di, void **next);
bool kvstoreHashtableRandomEntry(kvstore *kvs, int didx, void **found);
bool kvstoreHashtableFairRandomEntry(kvstore *kvs, int didx, void **found);
unsigned int kvstoreHashtableSampleEntries(kvstore *kvs, int didx, void **dst, unsigned int count);
bool kvstoreHashtableExpand(kvstore *kvs, int didx, unsigned long size);
void kvstoreSetIsImporting(kvstore *kvs, int didx, int is_importing);
unsigned long kvstoreHashtableScanDefrag(kvstore *kvs,
                                         int didx,
                                         unsigned long v,
                                         hashtableScanFunction fn,
                                         void *privdata,
                                         void *(*defragfn)(void *),
                                         int flags);
unsigned long kvstoreHashtableDefragTables(kvstore *kvs, unsigned long cursor, void *(*defragfn)(void *));
bool kvstoreHashtableFind(kvstore *kvs, int didx, void *key, void **found);
void **kvstoreHashtableFindRef(kvstore *kvs, int didx, const void *key);
bool kvstoreHashtableAdd(kvstore *kvs, int didx, void *entry);

bool kvstoreHashtableFindPositionForInsert(kvstore *kvs, int didx, void *key, hashtablePosition *position, void **existing);
void kvstoreHashtableInsertAtPosition(kvstore *kvs, int didx, void *entry, void *position);

void **kvstoreHashtableTwoPhasePopFindRef(kvstore *kvs, int didx, const void *key, void *position);
void kvstoreHashtableTwoPhasePopDelete(kvstore *kvs, int didx, void *position);
bool kvstoreHashtablePop(kvstore *kvs, int didx, const void *key, void **popped);
bool kvstoreHashtableDelete(kvstore *kvs, int didx, const void *key);
hashtable *kvstoreGetHashtable(kvstore *kvs, int didx);

#endif /* KVSTORE_H */
