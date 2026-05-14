/* Dict — convenience helpers for using hashtable as a key-value store.
 *
 * dict is a typedef for hashtable. dictEntry is a transparent struct that
 * callers allocate and access directly (de->key, de->v.val, de->v.u64, etc.).
 *
 * The helpers in this file handle dictEntry allocation and hashtable
 * insertion/lookup/deletion so callers don't need to manage the two-step
 * FindPositionForInsert + InsertAtPosition dance themselves.
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
#include "zmalloc.h"
#include <stdint.h>

#define DICT_OK 0
#define DICT_ERR 1

/* dict, dictType, and dictIterator are aliases for their hashtable equivalents. */
typedef hashtable dict;
typedef hashtableType dictType;
typedef hashtableIterator dictIterator;

/* dictEntry is a transparent key-value pair. Access fields directly:
 *   de->key       - the key pointer
 *   de->v.val     - value as void*
 *   de->v.u64     - value as uint64_t
 *   de->v.s64     - value as int64_t
 *   de->v.d       - value as double
 */
typedef struct dictEntry {
    void *key;
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
} dictEntry;

#define UNUSED(V) ((void)V)

#define dictSize(d) hashtableSize(d)
#define dictIsEmpty(d) (hashtableSize(d) == 0)
#define dictIsRehashing(d) hashtableIsRehashing(d)
#define dictCreate(type) hashtableCreate(type)
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

/* Expand the hash table if needed. Returns DICT_OK if expand was performed
 * or if the dictionary is already large enough, DICT_ERR if expand was not
 * performed. */
static inline int dictExpand(dict *d, unsigned long size) {
    return hashtableExpand(d, size) ? DICT_OK : DICT_ERR;
}

/* Callback for dictType.entryGetKey, which expects void pointers. */
static inline const void *dictEntryGetKey(const void *entry) {
    return ((const dictEntry *)entry)->key;
}

static inline size_t dictEntryMemUsage(dictEntry *de) {
    return sizeof(*de);
}

static inline size_t dictMemUsage(const dict *d) {
    return hashtableMemUsage(d) + hashtableSize(d) * sizeof(dictEntry);
}

/* Search for a key in the dictionary. Returns the dictEntry if found,
 * or NULL if the key doesn't exist. */
static inline dictEntry *dictFind(dict *d, const void *key) {
    void *found = NULL;
    return hashtableFind(d, key, &found) ? (dictEntry *)found : NULL;
}

/* Fetch the value associated with a key. Returns the value if the key exists,
 * or NULL if the key doesn't exist. */
static inline void *dictFetchValue(dict *d, const void *key) {
    dictEntry *de = dictFind(d, key);
    return de ? de->v.val : NULL;
}

/* Remove a key from the dictionary. Returns DICT_OK if the key was found
 * and removed, DICT_ERR if the key was not found. */
static inline int dictDelete(dict *d, const void *key) {
    return hashtableDelete(d, key) ? DICT_OK : DICT_ERR;
}

/* Free an entry that was previously unlinked with dictUnlink().
 * It's safe to call this function with de = NULL. */
static inline void dictFreeUnlinkedEntry(dict *d, dictEntry *de) {
    if (de == NULL) return;
    hashtableType *type = hashtableGetType(d);
    type->entryDestructor(de);
}

/* Return a random entry from the hash table. */
static inline dictEntry *dictGetRandomKey(dict *d) {
    void *entry = NULL;
    return hashtableRandomEntry(d, &entry) ? (dictEntry *)entry : NULL;
}

/* A more fair random entry selection that considers chain lengths.
 * This provides better distribution than dictGetRandomKey(). */
static inline dictEntry *dictGetFairRandomKey(dict *d) {
    void *entry = NULL;
    return hashtableFairRandomEntry(d, &entry) ? (dictEntry *)entry : NULL;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release
 * it. Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups. */
static inline dictEntry *dictUnlink(dict *d, const void *key) {
    void *entry = NULL;
    return hashtablePop(d, key, &entry) ? (dictEntry *)entry : NULL;
}

/* Add an entry to the dictionary. */
static inline int dictAdd(dict *d, void *key, void *val) {
    hashtablePosition pos;
    void *existing = NULL;

    if (!hashtableFindPositionForInsert(d, key, &pos, &existing)) {
        return DICT_ERR; /* Key already exists */
    }

    dictEntry *entry = (dictEntry *)zmalloc(sizeof(*entry));
    entry->key = key;
    entry->v.val = val;
    hashtableInsertAtPosition(d, entry, &pos);
    return DICT_OK;
}

/* Adds a key to the dictionary without setting a value.
 *
 * If key already exists, NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the dictEntry is returned to be manipulated by the
 * caller. */
static inline dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing) {
    hashtablePosition pos;
    void *existing_entry = NULL;

    if (!hashtableFindPositionForInsert(d, key, &pos, &existing_entry)) {
        if (existing) *existing = (dictEntry *)existing_entry;
        return NULL;
    }

    dictEntry *entry = (dictEntry *)zmalloc(sizeof(*entry));
    entry->key = key;
    hashtableInsertAtPosition(d, entry, &pos);
    if (existing) *existing = NULL;
    return entry;
}

/* Adds a key to the dictionary if it doesn't already exists. Returns the
 * dictEntry of the key, whether it was just added or not. */
static inline dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *existing = NULL;
    dictEntry *entry = dictAddRaw(d, key, &existing);
    return entry ? entry : existing;
}

/* Adds an element to the dictionary. If the key already exists, the old
 * value is replaced with the new one.
 *
 * Always returns 1 to indicate the key was consumed (either added or used
 * to replace). The caller should not free the key after calling this. */
static inline int dictReplace(dict *d, void *key, void *val) {
    dictEntry *entry = (dictEntry *)zmalloc(sizeof(*entry));
    entry->key = key;
    entry->v.val = val;

    void *existing = NULL;
    if (hashtableAddOrFind(d, entry, &existing)) {
        return 1; /* Entry was added */
    }

    /* Entry already exists. Put the old value in our new entry and free it. */
    dictEntry *existing_entry = (dictEntry *)existing;
    entry->v.val = existing_entry->v.val;
    hashtableType *type = hashtableGetType(d);
    type->entryDestructor(entry);

    /* Update the existing entry with the new value. */
    existing_entry->v.val = val;
    return 1;
}

/* Iterator operations */
static inline dictEntry *dictNext(dictIterator *iter) {
    void *entry = NULL;
    if (hashtableNext(iter, &entry)) {
        return (dictEntry *)entry;
    }
    return NULL;
}

#endif /* __DICT_H */
