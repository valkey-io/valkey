/* Dict, a key-value hashtable API.
 *
 * This file implements the dict API as a thin wrapper of the newer hashtable
 * API. The dictEntry struct is used as the entry type in underlying hashtable.
 *
 * Copyright (c) 2006-2012, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

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
 * the "htdict" prefix is used to avoid colliding with the "dict" in libvalkey */
#define dictCreate(type) htdictCreate(type)
#define dictExpand(d, size) htdictExpand(d, size)
#define dictSetKey(d, de, key) htdictSetKey(d, de, key)
#define dictSetVal(d, de, val) htdictSetVal(d, de, val)
#define dictSetSignedIntegerVal(de, val) htdictSetSignedIntegerVal(de, val)
#define dictSetUnsignedIntegerVal(de, val) htdictSetUnsignedIntegerVal(de, val)
#define dictSetDoubleVal(de, val) htdictSetDoubleVal(de, val)
#define dictIncrSignedIntegerVal(de, val) htdictIncrSignedIntegerVal(de, val)
#define dictIncrUnsignedIntegerVal(de, val) htdictIncrUnsignedIntegerVal(de, val)
#define dictIncrDoubleVal(de, val) htdictIncrDoubleVal(de, val)

#define dictGetKey(de) htdictGetKey(de)
#define dictGetVal(de) htdictGetVal(de)

#define dictGetSignedIntegerVal(de) htdictGetSignedIntegerVal(de)
#define dictGetUnsignedIntegerVal(de) htdictGetUnsignedIntegerVal(de)
#define dictGetDoubleVal(de) htdictGetDoubleVal(de)
#define dictGetDoubleValPtr(de) htdictGetDoubleValPtr(de)

#define dictEntryMemUsage(de) htdictEntryMemUsage(de)
#define dictMemUsage(d) htdictMemUsage(d)

#define dictFind(d, key) htdictFind(d, key)
#define dictFetchValue(d, key) htdictFetchValue(d, key)
#define dictDelete(d, key) htdictDelete(d, key)

#define dictFreeUnlinkedEntry(d, de) htdictFreeUnlinkedEntry(d, de)
#define dictGetRandomKey(d) htdictGetRandomKey(d)
#define dictGetFairRandomKey(d) htdictGetFairRandomKey(d)

#define dictUnlink(d, key) htdictUnlink(d, key)
#define dictAdd(d, key, val) htdictAdd(d, key, val)
#define dictAddRaw(d, key, existing) htdictAddRaw(d, key, existing)
#define dictAddOrFind(d, key) htdictAddOrFind(d, key)
#define dictReplace(d, key, val) htdictReplace(d, key, val)

#define dictNext(iter) htdictNext(iter)

dict *htdictCreate(dictType *type);
int htdictExpand(dict *d, unsigned long size);
void htdictSetKey(dict *d, dictEntry *de, void *key);
void htdictSetVal(dict *d, dictEntry *de, void *val);
void htdictSetSignedIntegerVal(dictEntry *de, int64_t val);
void htdictSetUnsignedIntegerVal(dictEntry *de, uint64_t val);
void htdictSetDoubleVal(dictEntry *de, double val);
int64_t htdictIncrSignedIntegerVal(dictEntry *de, int64_t val);
uint64_t htdictIncrUnsignedIntegerVal(dictEntry *de, uint64_t val);
double htdictIncrDoubleVal(dictEntry *de, double val);
void *htdictGetKey(const dictEntry *de);
void *htdictGetVal(const dictEntry *de);
int64_t htdictGetSignedIntegerVal(const dictEntry *de);
uint64_t htdictGetUnsignedIntegerVal(const dictEntry *de);
double htdictGetDoubleVal(const dictEntry *de);
double *htdictGetDoubleValPtr(dictEntry *de);
size_t htdictEntryMemUsage(dictEntry *de);
size_t htdictMemUsage(const dict *d);
dictEntry *htdictFind(dict *d, const void *key);
void *htdictFetchValue(dict *d, const void *key);
int htdictDelete(dict *d, const void *key);
void htdictFreeUnlinkedEntry(dict *d, dictEntry *de);
dictEntry *htdictGetRandomKey(dict *d);
dictEntry *htdictGetFairRandomKey(dict *d);
dictEntry *htdictUnlink(dict *d, const void *key);
int htdictAdd(dict *d, void *key, void *val);
dictEntry *htdictAddRaw(dict *d, void *key, dictEntry **existing);
dictEntry *htdictAddOrFind(dict *d, void *key);
int htdictReplace(dict *d, void *key, void *val);
dictEntry *htdictNext(dictIterator *iter);

typedef void *(dictDefragAllocFunction)(void *ptr);
typedef struct {
    dictDefragAllocFunction *defragAlloc;
    dictDefragAllocFunction *defragKey;
    dictDefragAllocFunction *defragVal;
} dictDefragFunctions;
void htdictDefragEntry(dictEntry **de_ref, dictDefragFunctions *defragfns);

#endif /* __DICT_H */
