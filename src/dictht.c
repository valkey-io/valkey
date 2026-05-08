/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

/* Dict, a key-value hashtable API. */

#include "dict.h"
#include "zmalloc.h"

/* dictEntry represents a key-value pair for use with hashtable */
struct dictEntry {
    void *key;
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
};

#define UNUSED(V) ((void)V)

/* Callback for dictType.entryGetKey, which expects void pointers. */
static const void *dicthtEntryGetKey(const void *entry) {
    return dictGetKey((const dictEntry *)entry);
}


dict *dicthtCreate(dictType *type) {
    type->entryGetKey = dicthtEntryGetKey;
    return hashtableCreate(type);
}

/* Expand the hash table if needed. Returns DICT_OK if expand was performed
 * or if the dictionary is already large enough, DICT_ERR if expand was not
 * performed. */
int dicthtExpand(dict *d, unsigned long size) {
    return hashtableExpand(d, size) ? DICT_OK : DICT_ERR;
}

/* Entry accessor functions */
void dicthtSetKey(dict *d, dictEntry *de, void *key) {
    UNUSED(d);
    de->key = key;
}

void dicthtSetVal(dict *d, dictEntry *de, void *val) {
    UNUSED(d);
    de->v.val = val;
}

void dicthtSetSignedIntegerVal(dictEntry *de, int64_t val) {
    de->v.s64 = val;
}

void dicthtSetUnsignedIntegerVal(dictEntry *de, uint64_t val) {
    de->v.u64 = val;
}

void dicthtSetDoubleVal(dictEntry *de, double val) {
    de->v.d = val;
}

int64_t dicthtIncrSignedIntegerVal(dictEntry *de, int64_t val) {
    de->v.s64 += val;
    return de->v.s64;
}

uint64_t dicthtIncrUnsignedIntegerVal(dictEntry *de, uint64_t val) {
    de->v.u64 += val;
    return de->v.u64;
}

double dicthtIncrDoubleVal(dictEntry *de, double val) {
    de->v.d += val;
    return de->v.d;
}

void *dicthtGetKey(const dictEntry *de) {
    return de->key;
}

void *dicthtGetVal(const dictEntry *de) {
    return de->v.val;
}

int64_t dicthtGetSignedIntegerVal(const dictEntry *de) {
    return de->v.s64;
}

uint64_t dicthtGetUnsignedIntegerVal(const dictEntry *de) {
    return de->v.u64;
}

double dicthtGetDoubleVal(const dictEntry *de) {
    return de->v.d;
}

double *dicthtGetDoubleValPtr(dictEntry *de) {
    return &de->v.d;
}

size_t dicthtEntryMemUsage(dictEntry *de) {
    return sizeof(*de);
}

size_t dicthtMemUsage(const dict *d) {
    return hashtableMemUsage(d) + hashtableSize(d) * sizeof(dictEntry);
}

/* Search for a key in the dictionary. Returns the dictEntry if found,
 * or NULL if the key doesn't exist. */
dictEntry *dicthtFind(dict *d, const void *key) {
    void *found = NULL;
    return hashtableFind(d, key, &found) ? (dictEntry *)found : NULL;
}

/* Fetch the value associated with a key. Returns the value if the key exists,
 * or NULL if the key doesn't exist. */
void *dicthtFetchValue(dict *d, const void *key) {
    dictEntry *de = dictFind(d, key);
    return de ? de->v.val : NULL;
}

/* Remove a key from the dictionary. Returns DICT_OK if the key was found
 * and removed, DICT_ERR if the key was not found. */
int dicthtDelete(dict *d, const void *key) {
    return hashtableDelete(d, key) ? DICT_OK : DICT_ERR;
}

/* Free an entry that was previously unlinked with dictUnlink().
 * It's safe to call this function with de = NULL. */
void dicthtFreeUnlinkedEntry(dict *d, dictEntry *de) {
    if (de == NULL) return;
    hashtableType *type = hashtableGetType(d);
    type->entryDestructor(de);
}

/* Return a random entry from the hash table. */
dictEntry *dicthtGetRandomKey(dict *d) {
    void *entry = NULL;
    return hashtableRandomEntry(d, &entry) ? (dictEntry *)entry : NULL;
}

/* A more fair random entry selection that considers chain lengths.
 * This provides better distribution than dictGetRandomKey(). */
dictEntry *dicthtGetFairRandomKey(dict *d) {
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
dictEntry *dicthtUnlink(dict *d, const void *key) {
    void *entry = NULL;
    return hashtablePop(d, key, &entry) ? (dictEntry *)entry : NULL;
}

/* Add an entry to the dictionary. */
int dicthtAdd(dict *d, void *key, void *val) {
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
dictEntry *dicthtAddRaw(dict *d, void *key, dictEntry **existing) {
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
dictEntry *dicthtAddOrFind(dict *d, void *key) {
    dictEntry *existing = NULL;
    dictEntry *entry = dictAddRaw(d, key, &existing);
    return entry ? entry : existing;
}

/* Adds an element to the dictionary. If the key already exists, the old
 * value is replaced with the new one.
 *
 * Always returns 1 to indicate the key was consumed (either added or used
 * to replace). The caller should not free the key after calling this. */
int dicthtReplace(dict *d, void *key, void *val) {
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
dictEntry *dicthtNext(dictIterator *iter) {
    void *entry = NULL;
    if (hashtableNext(iter, &entry)) {
        return (dictEntry *)entry;
    }
    return NULL;
}

void dicthtDefragEntry(dictEntry **de_ref, dictDefragFunctions *defragfns) {
    dictEntry *de = *de_ref;

    /* Defrag the entry itself */
    dictEntry *newentry = defragfns->defragAlloc(de);
    if (newentry) {
        de = newentry;
        *de_ref = newentry;
    }
    /* Defrag the key */
    if (defragfns->defragKey) {
        void *newkey = defragfns->defragKey(de->key);
        if (newkey) de->key = newkey;
    }
    /* Defrag the value */
    if (defragfns->defragVal) {
        void *newval = defragfns->defragVal(de->v.val);
        if (newval) de->v.val = newval;
    }
}
