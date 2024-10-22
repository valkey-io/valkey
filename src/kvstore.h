#ifndef KVSTORE_H
#define KVSTORE_H

#include "hashset.h"
#include "adlist.h"

typedef struct _kvstore kvstore;
typedef struct _kvstoreIterator kvstoreIterator;
typedef struct _kvstoreHashsetIterator kvstoreHashsetIterator;

typedef int(kvstoreScanShouldSkipHashset)(hashset *d);
typedef int(kvstoreExpandShouldSkipHashsetIndex)(int didx);

#define KVSTORE_ALLOCATE_HASHSETS_ON_DEMAND (1 << 0)
#define KVSTORE_FREE_EMPTY_HASHSETS (1 << 1)
kvstore *kvstoreCreate(hashsetType *type, int num_hashsets_bits, int flags);
void kvstoreEmpty(kvstore *kvs, void(callback)(hashset *));
void kvstoreRelease(kvstore *kvs);
unsigned long long kvstoreSize(kvstore *kvs);
unsigned long kvstoreBuckets(kvstore *kvs);
size_t kvstoreMemUsage(kvstore *kvs);
unsigned long long kvstoreScan(kvstore *kvs,
                               unsigned long long cursor,
                               int onlydidx,
                               hashsetScanFunction scan_cb,
                               kvstoreScanShouldSkipHashset *skip_cb,
                               void *privdata,
                               int flags);
int kvstoreExpand(kvstore *kvs, uint64_t newsize, int try_expand, kvstoreExpandShouldSkipHashsetIndex *skip_cb);
int kvstoreGetFairRandomHashsetIndex(kvstore *kvs);
void kvstoreGetStats(kvstore *kvs, char *buf, size_t bufsize, int full);

int kvstoreFindHashsetIndexByKeyIndex(kvstore *kvs, unsigned long target);
int kvstoreGetFirstNonEmptyHashsetIndex(kvstore *kvs);
int kvstoreGetNextNonEmptyHashsetIndex(kvstore *kvs, int didx);
int kvstoreNumNonEmptyHashsets(kvstore *kvs);
int kvstoreNumAllocatedHashsets(kvstore *kvs);
int kvstoreNumHashsets(kvstore *kvs);
uint64_t kvstoreGetHash(kvstore *kvs, const void *key);

void kvstoreHashsetRehashingStarted(hashset *d);
void kvstoreHashsetRehashingCompleted(hashset *d);
size_t kvstoreHashsetMetadataSize(void);

/* kvstore iterator specific functions */
kvstoreIterator *kvstoreIteratorInit(kvstore *kvs);
void kvstoreIteratorRelease(kvstoreIterator *kvs_it);
int kvstoreIteratorGetCurrentHashsetIndex(kvstoreIterator *kvs_it);
int kvstoreIteratorNext(kvstoreIterator *kvs_it, void **next);

/* Rehashing */
void kvstoreTryResizeHashsets(kvstore *kvs, int limit);
uint64_t kvstoreIncrementallyRehash(kvstore *kvs, uint64_t threshold_us);
size_t kvstoreOverheadHashtableLut(kvstore *kvs);
size_t kvstoreOverheadHashtableRehashing(kvstore *kvs);
unsigned long kvstoreHashsetRehashingCount(kvstore *kvs);

/* Specific hashset access by hashset-index */
unsigned long kvstoreHashsetSize(kvstore *kvs, int didx);
kvstoreHashsetIterator *kvstoreGetHashsetIterator(kvstore *kvs, int didx);
kvstoreHashsetIterator *kvstoreGetHashsetSafeIterator(kvstore *kvs, int didx);
void kvstoreReleaseHashsetIterator(kvstoreHashsetIterator *kvs_id);
int kvstoreHashsetIteratorNext(kvstoreHashsetIterator *kvs_di, void **next);
int kvstoreHashsetRandomElement(kvstore *kvs, int didx, void **found);
int kvstoreHashsetFairRandomElement(kvstore *kvs, int didx, void **found);
unsigned int kvstoreHashsetSampleElements(kvstore *kvs, int didx, void **dst, unsigned int count);
int kvstoreHashsetExpand(kvstore *kvs, int didx, unsigned long size);
unsigned long kvstoreHashsetScan(kvstore *kvs,
                                 int didx,
                                 unsigned long v,
                                 hashsetScanFunction fn,
                                 void *privdata,
                                 int flags);
void kvstoreHashsetDefragInternals(kvstore *kvs, void *(*defragfn)(void *));
/* void *kvstoreHashsetFetchElement(kvstore *kvs, int didx, const void *key); */
int kvstoreHashsetFind(kvstore *kvs, int didx, void *key, void **found);
void **kvstoreHashsetFindRef(kvstore *kvs, int didx, const void *key);
int kvstoreHashsetAddOrFind(kvstore *kvs, int didx, void *key, void **existing);
int kvstoreHashsetAdd(kvstore *kvs, int didx, void *element);

void *kvstoreHashsetFindPositionForInsert(kvstore *kvs, int didx, void *key, void **existing);
void kvstoreHashsetInsertAtPosition(kvstore *kvs, int didx, void *elem, void *position);

void **kvstoreHashsetTwoPhasePopFindRef(kvstore *kvs, int didx, const void *key, void **position);
void kvstoreHashsetTwoPhasePopDelete(kvstore *kvs, int didx, void *position);
int kvstoreHashsetPop(kvstore *kvs, int didx, const void *key, void **popped);
int kvstoreHashsetDelete(kvstore *kvs, int didx, const void *key);
hashset *kvstoreGetHashset(kvstore *kvs, int didx);

#endif /* KVSTORE_H */
