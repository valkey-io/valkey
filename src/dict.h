/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

/* Dict, a key-value hashtable API. */

#ifndef __DICT_H
#define __DICT_H

#include "hashtable.h"
#include <stdint.h>

#define DICT_OK 0
#define DICT_ERR 1

/* dict is now an alias for hashtable */
typedef hashtable dict;
typedef hashtableType dictType;
typedef hashtableIterator dictIterator;

typedef struct dictEntry dictEntry; // opaque

/* functions replaced by hashtable equivalents */
#define dictSize(d) hashtableSize(d)
#define dictIsEmpty(d) (hashtableSize(d) == 0)
#define dictIsRehashing(d) hashtableIsRehashing(d)
#define dictRelease(d) hashtableRelease(d)
#define dictEmpty(d, callback) hashtableEmpty(d, callback)
#define dictGetSomeKeys(d, dst, count) hashtableSampleEntries(d, dst, count)
#define dictGenHashFunction(key, len) hashtableGenHashFunction(key, len)
#define dictGenCaseHashFunction(buf, len) hashtableGenCaseHashFunction(buf, len)
#define dictRehashMicroseconds(d, us) hashtableRehashMicroseconds(d, us)
#define dictGetIterator(d) hashtableCreateIterator(d, 0)
#define dictGetSafeIterator(d) hashtableCreateIterator(d, HASHTABLE_ITER_SAFE)
#define dictReleaseIterator(iter) hashtableReleaseIterator(iter)
#define dictInitIterator(iter, d) hashtableInitIterator(iter, d, 0)
#define dictInitSafeIterator(iter, d) hashtableInitIterator(iter, d, HASHTABLE_ITER_SAFE)
#define dictResetIterator(iter) hashtableCleanupIterator(iter)

/* functions with variation for hashtable implementation
 * the "dictht" prefix is used to avoid colliding with the "dict" in libvalkey */
#define dictCreate(type) dicthtCreate(type)
#define dictExpand(d, size) dicthtExpand(d, size)
#define dictSetKey(d, de, key) dicthtSetKey(d, de, key)
#define dictSetVal(d, de, val) dicthtSetVal(d, de, val)
#define dictSetSignedIntegerVal(de, val) dicthtSetSignedIntegerVal(de, val)
#define dictSetUnsignedIntegerVal(de, val) dicthtSetUnsignedIntegerVal(de, val)
#define dictSetDoubleVal(de, val) dicthtSetDoubleVal(de, val)
#define dictIncrSignedIntegerVal(de, val) dicthtIncrSignedIntegerVal(de, val)
#define dictIncrUnsignedIntegerVal(de, val) dicthtIncrUnsignedIntegerVal(de, val)
#define dictIncrDoubleVal(de, val) dicthtIncrDoubleVal(de, val)

#define dictGetKey(de) dicthtGetKey(de)
#define dictGetVal(de) dicthtGetVal(de)

#define dictGetSignedIntegerVal(de) dicthtGetSignedIntegerVal(de)
#define dictGetUnsignedIntegerVal(de) dicthtGetUnsignedIntegerVal(de)
#define dictGetDoubleVal(de) dicthtGetDoubleVal(de)
#define dictGetDoubleValPtr(de) dicthtGetDoubleValPtr(de)

#define dictEntryMemUsage(de) dicthtEntryMemUsage(de)
#define dictMemUsage(d) dicthtMemUsage(d)

#define dictFind(d, key) dicthtFind(d, key)
#define dictFetchValue(d, key) dicthtFetchValue(d, key)
#define dictDelete(d, key) dicthtDelete(d, key)

#define dictFreeUnlinkedEntry(d, de) dicthtFreeUnlinkedEntry(d, de)
#define dictGetRandomKey(d) dicthtGetRandomKey(d)
#define dictGetFairRandomKey(d) dicthtGetFairRandomKey(d)

#define dictUnlink(d, key) dicthtUnlink(d, key)
#define dictAdd(d, key, val) dicthtAdd(d, key, val)
#define dictAddRaw(d, key, existing) dicthtAddRaw(d, key, existing)
#define dictAddOrFind(d, key) dicthtAddOrFind(d, key)
#define dictReplace(d, key, val) dicthtReplace(d, key, val)

#define dictNext(iter) dicthtNext(iter)

dict *dicthtCreate(dictType *type);
int dicthtExpand(dict *d, unsigned long size);
void dicthtSetKey(dict *d, dictEntry *de, void *key);
void dicthtSetVal(dict *d, dictEntry *de, void *val);
void dicthtSetSignedIntegerVal(dictEntry *de, int64_t val);
void dicthtSetUnsignedIntegerVal(dictEntry *de, uint64_t val);
void dicthtSetDoubleVal(dictEntry *de, double val);
int64_t dicthtIncrSignedIntegerVal(dictEntry *de, int64_t val);
uint64_t dicthtIncrUnsignedIntegerVal(dictEntry *de, uint64_t val);
double dicthtIncrDoubleVal(dictEntry *de, double val);
void *dicthtGetKey(const dictEntry *de);
void *dicthtGetVal(const dictEntry *de);
int64_t dicthtGetSignedIntegerVal(const dictEntry *de);
uint64_t dicthtGetUnsignedIntegerVal(const dictEntry *de);
double dicthtGetDoubleVal(const dictEntry *de);
double *dicthtGetDoubleValPtr(dictEntry *de);
size_t dicthtEntryMemUsage(dictEntry *de);
size_t dicthtMemUsage(const dict *d);
dictEntry *dicthtFind(dict *d, const void *key);
void *dicthtFetchValue(dict *d, const void *key);
int dicthtDelete(dict *d, const void *key);
void dicthtFreeUnlinkedEntry(dict *d, dictEntry *de);
dictEntry *dicthtGetRandomKey(dict *d);
dictEntry *dicthtGetFairRandomKey(dict *d);
dictEntry *dicthtUnlink(dict *d, const void *key);
int dicthtAdd(dict *d, void *key, void *val);
dictEntry *dicthtAddRaw(dict *d, void *key, dictEntry **existing);
dictEntry *dicthtAddOrFind(dict *d, void *key);
int dicthtReplace(dict *d, void *key, void *val);
dictEntry *dicthtNext(dictIterator *iter);

typedef void *(dictDefragAllocFunction)(void *ptr);
typedef struct {
    dictDefragAllocFunction *defragAlloc;
    dictDefragAllocFunction *defragKey;
    dictDefragAllocFunction *defragVal;
} dictDefragFunctions;
void dicthtDefragEntry(dictEntry **de_ref, dictDefragFunctions *defragfns);

#endif /* __DICT_H */
