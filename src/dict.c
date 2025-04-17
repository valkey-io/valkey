/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
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
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "fmacros.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#include "serverassert.h"
#include "monotonic.h"
#include "config.h"

#ifndef static_assert
#define static_assert(expr, lit) _Static_assert(expr, lit)
#endif

#define UNUSED(V) ((void)V)

/* Using dictSetResizeEnabled() we make possible to disable
 * resizing and rehashing of the hash table as needed. This is very important
 * for us, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to DICT_RESIZE_AVOID, not all
 * resizes are prevented:
 *  - A hash table is still allowed to expand if the ratio between the number
 *    of elements and the buckets >= dict_force_resize_ratio.
 *  - A hash table is still allowed to shrink if the ratio between the number
 *    of elements and the buckets <= 1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio). */
static dictResizeEnable dict_can_resize = DICT_RESIZE_ENABLE;
static unsigned int dict_force_resize_ratio = 4;

/* -------------------------- types ----------------------------------------- */
typedef struct {
    void *key;
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next; /* Next entry in the same hash bucket. */
} dictEntryNormal;

typedef struct {
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next;  /* Next entry in the same hash bucket. */
    uint8_t key_header_size; /* offset into key_buf where the key is located at. */
    unsigned char key_buf[]; /* buffer with embedded key. */
} dictEntryEmbedded;

/* Validation and helper for `dictEntryEmbedded` */

static_assert(offsetof(dictEntryEmbedded, v) == 0, "unexpected field offset");
static_assert(offsetof(dictEntryEmbedded, next) == sizeof(double), "unexpected field offset");
static_assert(offsetof(dictEntryEmbedded, key_header_size) == sizeof(double) + sizeof(void *),
              "unexpected field offset");
/* key_buf is located after a union with a double value  `v.d`, a pointer `next` and uint8_t field `key_header_size` */
static_assert(offsetof(dictEntryEmbedded, key_buf) == sizeof(double) + sizeof(void *) + sizeof(uint8_t),
              "unexpected field offset");

/* The minimum amount of bytes required for embedded dict entry. */
static inline size_t compactSizeEmbeddedDictEntry(void) {
    return offsetof(dictEntryEmbedded, key_buf);
}

typedef struct {
    void *key;
    dictEntry *next;
} dictEntryNoValue;

/* -------------------------- private prototypes ---------------------------- */
static dictEntry **dictGetNextRef(dictEntry *de);
static void dictSetNext(dictEntry *de, dictEntry *next);

/* -------------------------- Utility functions -------------------------------- */
static void dictShrinkIfAutoResizeAllowed(dict *d) {
    /* Automatic resizing is disallowed. Return */
    if (d->pauseAutoResize > 0) return;

    dictShrinkIfNeeded(d);
}

/* Expand the hash table if needed */
static void dictExpandIfAutoResizeAllowed(dict *d) {
    /* Automatic resizing is disallowed. Return */
    if (d->pauseAutoResize > 0) return;

    dictExpandIfNeeded(d);
}

/* Our hash table capability is a power of two */
static signed char dictNextExp(unsigned long size) {
    if (size <= DICT_HT_INITIAL_SIZE) return DICT_HT_INITIAL_EXP;
    if (size >= LONG_MAX) return (8 * sizeof(long) - 1);

    return 8 * sizeof(long) - __builtin_clzl(size - 1);
}

/* This function performs just a step of rehashing, and only if hashing has
 * not been paused for our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some elements can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
static void dictRehashStep(dict *d) {
    if (d->pauserehash == 0) dictRehash(d, 1);
}

/* Validates dict type members dependencies. */
static inline void validateDictType(dictType *type) {
    if (type->embedded_entry) {
        assert(type->embedKey);
        assert(!type->keyDup);
        assert(!type->keyDestructor);
    } else {
        assert(!type->embedKey);
    }
}

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed, seed, sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, size_t len) {
    return siphash(key, len, dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len) {
    return siphash_nocase(buf, len, dict_hash_function_seed);
}

/* --------------------- dictEntry pointer bit tricks ----------------------  */

/* The 3 least significant bits in a pointer to a dictEntry determines what the
 * pointer actually points to. If the least bit is set, it's a key. Otherwise,
 * the bit pattern of the least 3 significant bits mark the kind of entry. */

#define ENTRY_PTR_MASK 7     /* 111 */
#define ENTRY_PTR_NORMAL 0   /* 000 */
#define ENTRY_PTR_NO_VALUE 2 /* 010 */
#define ENTRY_PTR_EMBEDDED 4 /* 100 */
#define ENTRY_PTR_IS_KEY 1   /* XX1 */

/* Returns 1 if the entry pointer is a pointer to a key, rather than to an
 * allocated entry. Returns 0 otherwise. */
static inline int entryIsKey(const void *de) {
    return (uintptr_t)(void *)de & ENTRY_PTR_IS_KEY;
}

/* Returns 1 if the pointer is actually a pointer to a dictEntry struct. Returns
 * 0 otherwise. */
static inline int entryIsNormal(const void *de) {
    return ((uintptr_t)(void *)de & ENTRY_PTR_MASK) == ENTRY_PTR_NORMAL;
}

/* Returns 1 if the entry is a special entry with key and next, but without
 * value. Returns 0 otherwise. */
static inline int entryIsNoValue(const void *de) {
    return ((uintptr_t)(void *)de & ENTRY_PTR_MASK) == ENTRY_PTR_NO_VALUE;
}

static inline int entryIsEmbedded(const void *de) {
    return ((uintptr_t)(void *)de & ENTRY_PTR_MASK) == ENTRY_PTR_EMBEDDED;
}

static inline dictEntry *encodeMaskedPtr(const void *ptr, unsigned int bits) {
    return (dictEntry *)(void *)((uintptr_t)ptr | bits);
}

static inline void *decodeMaskedPtr(const dictEntry *de) {
    return (void *)((uintptr_t)(void *)de & ~ENTRY_PTR_MASK);
}

static inline dictEntry *createEntryNormal(void *key, dictEntry *next) {
    dictEntryNormal *entry = zmalloc(sizeof(dictEntryNormal));
    entry->key = key;
    entry->next = next;
    return encodeMaskedPtr(entry, ENTRY_PTR_NORMAL);
}

/* Creates an entry without a value field. */
static inline dictEntry *createEntryNoValue(void *key, dictEntry *next) {
    dictEntryNoValue *entry = zmalloc(sizeof(*entry));
    entry->key = key;
    entry->next = next;
    return encodeMaskedPtr(entry, ENTRY_PTR_NO_VALUE);
}

static inline dictEntry *createEmbeddedEntry(void *key, dictEntry *next, dictType *dt) {
    size_t key_len = dt->embedKey(NULL, 0, key, NULL);
    dictEntryEmbedded *entry = zmalloc(compactSizeEmbeddedDictEntry() + key_len);
    dt->embedKey(entry->key_buf, key_len, key, &entry->key_header_size);
    entry->next = next;
    return encodeMaskedPtr(entry, ENTRY_PTR_EMBEDDED);
}

static inline void *getEmbeddedKey(const dictEntry *de) {
    dictEntryEmbedded *entry = (dictEntryEmbedded *)decodeMaskedPtr(de);
    return &entry->key_buf[entry->key_header_size];
}

/* Decodes the pointer to an entry without value, when you know it is an entry
 * without value. Hint: Use entryIsNoValue to check. */
static inline dictEntryNoValue *decodeEntryNoValue(const dictEntry *de) {
    return decodeMaskedPtr(de);
}

static inline dictEntryEmbedded *decodeEntryEmbedded(const dictEntry *de) {
    return decodeMaskedPtr(de);
}

static inline dictEntryNormal *decodeEntryNormal(const dictEntry *de) {
    return decodeMaskedPtr(de);
}

/* ----------------------------- API implementation ------------------------- */

/* Reset hash table parameters already initialized with dictInit()*/
static void dictReset(dict *d, int htidx) {
    d->ht_table[htidx] = NULL;
    d->ht_size_exp[htidx] = -1;
    d->ht_used[htidx] = 0;
}

/* Initialize the hash table */
static int dictInit(dict *d, dictType *type) {
    dictReset(d, 0);
    dictReset(d, 1);
    d->type = type;
    d->rehashidx = -1;
    d->pauserehash = 0;
    d->pauseAutoResize = 0;
    return DICT_OK;
}

/* Create a new hash table */
dict *dictCreate(dictType *type) {
    validateDictType(type);
    size_t metasize = type->dictMetadataBytes ? type->dictMetadataBytes(NULL) : 0;
    dict *d = zmalloc(sizeof(*d) + metasize);
    if (metasize > 0) {
        memset(dictMetadata(d), 0, metasize);
    }
    dictInit(d, type);
    return d;
}

/* Resize or create the hash table,
 * when malloc_failed is non-NULL, it'll avoid panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if resize was performed, and DICT_ERR if skipped. */
static int dictResizeWithOptionalCheck(dict *d, unsigned long size, int *malloc_failed) {
    if (malloc_failed) *malloc_failed = 0;

    /* We can't rehash twice if rehashing is ongoing. */
    assert(!dictIsRehashing(d));

    /* the new hash table */
    dictEntry **new_ht_table;
    unsigned long new_ht_used;
    signed char new_ht_size_exp = dictNextExp(size);

    /* Detect overflows */
    size_t newsize = DICTHT_SIZE(new_ht_size_exp);
    if (newsize < size || newsize * sizeof(dictEntry *) < newsize) return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
    if (new_ht_size_exp == d->ht_size_exp[0]) return DICT_ERR;

    /* Allocate the new hash table and initialize all pointers to NULL */
    if (malloc_failed) {
        new_ht_table = ztrycalloc(newsize * sizeof(dictEntry *));
        *malloc_failed = new_ht_table == NULL;
        if (*malloc_failed) return DICT_ERR;
    } else {
        new_ht_table = zcalloc(newsize * sizeof(dictEntry *));
    }

    new_ht_used = 0;

    /* Prepare a second hash table for incremental rehashing.
     * We do this even for the first initialization, so that we can trigger the
     * rehashingStarted more conveniently, we will clean it up right after. */
    d->ht_size_exp[1] = new_ht_size_exp;
    d->ht_used[1] = new_ht_used;
    d->ht_table[1] = new_ht_table;
    d->rehashidx = 0;
    if (d->type->rehashingStarted) d->type->rehashingStarted(d);

    /* Is this the first initialization or is the first hash table empty? If so
     * it's not really a rehashing, we can just set the first hash table so that
     * it can accept keys. */
    if (d->ht_table[0] == NULL || d->ht_used[0] == 0) {
        if (d->type->rehashingCompleted) d->type->rehashingCompleted(d);
        if (d->ht_table[0]) zfree(d->ht_table[0]);
        d->ht_size_exp[0] = new_ht_size_exp;
        d->ht_used[0] = new_ht_used;
        d->ht_table[0] = new_ht_table;
        dictReset(d, 1);
        d->rehashidx = -1;
        return DICT_OK;
    }

    if (d->type->no_incremental_rehash) {
        /* If the dict type does not support incremental rehashing, we need to
         * rehash the whole table immediately. */
        while (dictRehash(d, 1000));
    }

    return DICT_OK;
}

static int dictExpandWithOptionalCheck(dict *d, unsigned long size, int *malloc_failed) {
    /* the size is invalid if it is smaller than the size of the hash table
     * or smaller than the number of elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht_used[0] > size || DICTHT_SIZE(d->ht_size_exp[0]) >= size) return DICT_ERR;
    return dictResizeWithOptionalCheck(d, size, malloc_failed);
}

/* return DICT_ERR if expand was not performed */
int dictExpand(dict *d, unsigned long size) {
    return dictExpandWithOptionalCheck(d, size, NULL);
}

/* return DICT_ERR if expand failed due to memory allocation failure */
int dictTryExpand(dict *d, unsigned long size) {
    int malloc_failed = 0;
    dictExpandWithOptionalCheck(d, size, &malloc_failed);
    return malloc_failed ? DICT_ERR : DICT_OK;
}

/* return DICT_ERR if shrink was not performed */
int dictShrink(dict *d, unsigned long size) {
    /* the size is invalid if it is bigger than the size of the hash table
     * or smaller than the number of elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht_used[0] > size || DICTHT_SIZE(d->ht_size_exp[0]) <= size) return DICT_ERR;
    return dictResizeWithOptionalCheck(d, size, NULL);
}

/* Helper function for `dictRehash` and `dictBucketRehash` which rehashes all the keys
 * in a bucket at index `idx` from the old to the new hash HT. */
static void rehashEntriesInBucketAtIndex(dict *d, uint64_t idx) {
    dictEntry *de = d->ht_table[0][idx];
    uint64_t h;
    dictEntry *nextde;
    while (de) {
        nextde = dictGetNext(de);
        void *key = dictGetKey(de);
        /* Get the index in the new hash table */
        if (d->ht_size_exp[1] > d->ht_size_exp[0]) {
            h = dictHashKey(d, key) & DICTHT_SIZE_MASK(d->ht_size_exp[1]);
        } else {
            /* We're shrinking the table. The tables sizes are powers of
             * two, so we simply mask the bucket index in the larger table
             * to get the bucket index in the smaller table. */
            h = idx & DICTHT_SIZE_MASK(d->ht_size_exp[1]);
        }
        if (d->type->no_value) {
            if (d->type->keys_are_odd && !d->ht_table[1][h]) {
                /* Destination bucket is empty and we can store the key
                 * directly without an allocated entry. Free the old entry
                 * if it's an allocated entry. */
                assert(entryIsKey(key));
                if (!entryIsKey(de)) zfree(decodeMaskedPtr(de));
                de = key;
            } else if (entryIsKey(de)) {
                /* We don't have an allocated entry but we need one. */
                de = createEntryNoValue(key, d->ht_table[1][h]);
            } else {
                /* Just move the existing entry to the destination table and
                 * update the 'next' field. */
                assert(entryIsNoValue(de));
                dictSetNext(de, d->ht_table[1][h]);
            }
        } else {
            dictSetNext(de, d->ht_table[1][h]);
        }
        d->ht_table[1][h] = de;
        d->ht_used[0]--;
        d->ht_used[1]++;
        de = nextde;
    }
    d->ht_table[0][idx] = NULL;
}

/* This checks if we already rehashed the whole table and if more rehashing is required */
static int dictCheckRehashingCompleted(dict *d) {
    if (d->ht_used[0] != 0) return 0;

    if (d->type->rehashingCompleted) d->type->rehashingCompleted(d);
    zfree(d->ht_table[0]);
    /* Copy the new ht onto the old one */
    d->ht_table[0] = d->ht_table[1];
    d->ht_used[0] = d->ht_used[1];
    d->ht_size_exp[0] = d->ht_size_exp[1];
    dictReset(d, 1);
    d->rehashidx = -1;
    return 1;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
int dictRehash(dict *d, int n) {
    int empty_visits = n * 10; /* Max number of empty buckets to visit. */
    unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
    unsigned long s1 = DICTHT_SIZE(d->ht_size_exp[1]);
    if (dict_can_resize == DICT_RESIZE_FORBID || !dictIsRehashing(d)) return 0;
    /* If dict_can_resize is DICT_RESIZE_AVOID, we want to avoid rehashing.
     * - If expanding, the threshold is dict_force_resize_ratio which is 4.
     * - If shrinking, the threshold is 1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) which is 1/32. */
    if (dict_can_resize == DICT_RESIZE_AVOID && ((s1 > s0 && s1 < dict_force_resize_ratio * s0) ||
                                                 (s1 < s0 && s0 < HASHTABLE_MIN_FILL * dict_force_resize_ratio * s1))) {
        return 0;
    }

    while (n-- && d->ht_used[0] != 0) {
        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        assert(DICTHT_SIZE(d->ht_size_exp[0]) > (unsigned long)d->rehashidx);
        while (d->ht_table[0][d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        /* Move all the keys in this bucket from the old to the new hash HT */
        rehashEntriesInBucketAtIndex(d, d->rehashidx);
        d->rehashidx++;
    }

    return !dictCheckRehashingCompleted(d);
}

long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
}

/* Rehash in us+"delta" microseconds. The value of "delta" is larger
 * than 0, and is smaller than 1000 in most cases. The exact upper bound
 * depends on the running time of dictRehash(d,100).*/
int dictRehashMicroseconds(dict *d, uint64_t us) {
    if (d->pauserehash > 0) return 0;

    monotime timer;
    elapsedStart(&timer);
    int rehashes = 0;

    while (dictRehash(d, 100)) {
        rehashes += 100;
        if (elapsedUs(timer) >= us) break;
    }
    return rehashes;
}

/* Performs rehashing on a single bucket. */
static int dictBucketRehash(dict *d, uint64_t idx) {
    if (d->pauserehash != 0) return 0;
    unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
    unsigned long s1 = DICTHT_SIZE(d->ht_size_exp[1]);
    if (dict_can_resize == DICT_RESIZE_FORBID || !dictIsRehashing(d)) return 0;
    /* If dict_can_resize is DICT_RESIZE_AVOID, we want to avoid rehashing.
     * - If expanding, the threshold is dict_force_resize_ratio which is 4.
     * - If shrinking, the threshold is 1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) which is 1/32. */
    if (dict_can_resize == DICT_RESIZE_AVOID && ((s1 > s0 && s1 < dict_force_resize_ratio * s0) ||
                                                 (s1 < s0 && s0 < HASHTABLE_MIN_FILL * dict_force_resize_ratio * s1))) {
        return 0;
    }
    rehashEntriesInBucketAtIndex(d, idx);
    dictCheckRehashingCompleted(d);
    return 1;
}

/* Add an element to the target hash table */
int dictAdd(dict *d, void *key, void *val) {
    dictEntry *entry = dictAddRaw(d, key, NULL);

    if (!entry) return DICT_ERR;
    if (!d->type->no_value) dictSetVal(d, entry, val);
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as they wish.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 *
 * The dict handles `key` based on `dictType` during initialization:
 * - If `dictType.embedded-entry` is 1, it clones the `key`.
 * - Otherwise, it assumes ownership of the `key`.
 */
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing) {
    /* Get the position for the new key or NULL if the key already exists. */
    void *position = dictFindPositionForInsert(d, key, existing);
    if (!position) return NULL;

    /* Dup the key if necessary. */
    if (d->type->keyDup) key = d->type->keyDup(key);

    return dictInsertAtPosition(d, key, position);
}

/* Adds a key in the dict's hashtable at the position returned by a preceding
 * call to dictFindPositionForInsert. This is a low level function which allows
 * splitting dictAddRaw in two parts. Normally, dictAddRaw or dictAdd should be
 * used instead. */
dictEntry *dictInsertAtPosition(dict *d, void *key, void *position) {
    dictEntry **bucket = position; /* It's a bucket, but the API hides that. */
    dictEntry *entry;
    /* If rehashing is ongoing, we insert in table 1, otherwise in table 0.
     * Assert that the provided bucket is the right table. */
    int htidx = dictIsRehashing(d) ? 1 : 0;
    assert(bucket >= &d->ht_table[htidx][0] && bucket <= &d->ht_table[htidx][DICTHT_SIZE_MASK(d->ht_size_exp[htidx])]);
    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    if (d->type->no_value) {
        if (d->type->keys_are_odd && !*bucket) {
            /* We can store the key directly in the destination bucket without the
             * allocated entry. */
            entry = key;
            assert(entryIsKey(entry));
        } else {
            /* Allocate an entry without value. */
            entry = createEntryNoValue(key, *bucket);
        }
    } else if (d->type->embedded_entry) {
        entry = createEmbeddedEntry(key, *bucket, d->type);
    } else {
        entry = createEntryNormal(key, *bucket);
    }
    *bucket = entry;
    d->ht_used[htidx]++;

    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
int dictReplace(dict *d, void *key, void *val) {
    dictEntry *entry, *existing;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    entry = dictAddRaw(d, key, &existing);
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    void *oldval = dictGetVal(existing);
    dictSetVal(d, existing, val);
    if (d->type->valDestructor) d->type->valDestructor(oldval);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    entry = dictAddRaw(d, key, &existing);
    return entry ? entry : existing;
}

/* Search and remove an element. This is a helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    /* dict is empty */
    if (dictSize(d) == 0) return NULL;

    h = dictHashKey(d, key);
    idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[0]);

    if (dictIsRehashing(d)) {
        if ((long)idx >= d->rehashidx && d->ht_table[0][idx]) {
            /* If we have a valid hash entry at `idx` in ht0, we perform
             * rehash on the bucket at `idx` (being more CPU cache friendly) */
            dictBucketRehash(d, idx);
        } else {
            /* If the hash entry is not in ht0, we rehash the buckets based
             * on the rehashidx (not CPU cache friendly). */
            dictRehashStep(d);
        }
    }

    for (table = 0; table <= 1; table++) {
        if (table == 0 && (long)idx < d->rehashidx) continue;
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        he = d->ht_table[table][idx];
        prevHe = NULL;
        while (he) {
            void *he_key = dictGetKey(he);
            if (key == he_key || dictCompareKeys(d, key, he_key)) {
                /* Unlink the element from the list */
                if (prevHe)
                    dictSetNext(prevHe, dictGetNext(he));
                else
                    d->ht_table[table][idx] = dictGetNext(he);
                if (!nofree) {
                    dictFreeUnlinkedEntry(d, he);
                }
                d->ht_used[table]--;
                dictShrinkIfAutoResizeAllowed(d);
                return he;
            }
            prevHe = he;
            he = dictGetNext(he);
        }
        if (!dictIsRehashing(d)) break;
    }
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht, key, 0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
dictEntry *dictUnlink(dict *d, const void *key) {
    return dictGenericDelete(d, key, 1);
}

inline static void dictFreeKey(dict *d, dictEntry *entry) {
    if (d->type->keyDestructor) {
        d->type->keyDestructor(dictGetKey(entry));
    }
}

inline static void dictFreeVal(dict *d, dictEntry *entry) {
    if (d->type->valDestructor) {
        d->type->valDestructor(dictGetVal(entry));
    }
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    /* Clear the dictEntry */
    if (!entryIsKey(he)) zfree(decodeMaskedPtr(he));
}

/* Destroy an entire dictionary */
static int dictClear(dict *d, int htidx, void(callback)(dict *)) {
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]) && d->ht_used[htidx] > 0; i++) {
        dictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0) callback(d);

        if ((he = d->ht_table[htidx][i]) == NULL) continue;
        while (he) {
            nextHe = dictGetNext(he);
            dictFreeKey(d, he);
            dictFreeVal(d, he);
            if (!entryIsKey(he)) zfree(decodeMaskedPtr(he));
            d->ht_used[htidx]--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    zfree(d->ht_table[htidx]);
    /* Re-initialize the table */
    dictReset(d, htidx);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
void dictRelease(dict *d) {
    /* Someone may be monitoring a dict that started rehashing, before
     * destroying the dict fake completion. */
    if (dictIsRehashing(d) && d->type->rehashingCompleted) d->type->rehashingCompleted(d);
    dictClear(d, 0, NULL);
    dictClear(d, 1, NULL);
    zfree(d);
}

dictEntry *dictFind(dict *d, const void *key) {
    dictEntry *he;
    uint64_t h, idx, table;

    if (dictSize(d) == 0) return NULL; /* dict is empty */

    h = dictHashKey(d, key);
    idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[0]);

    if (dictIsRehashing(d)) {
        if ((long)idx >= d->rehashidx && d->ht_table[0][idx]) {
            /* If we have a valid hash entry at `idx` in ht0, we perform
             * rehash on the bucket at `idx` (being more CPU cache friendly) */
            dictBucketRehash(d, idx);
        } else {
            /* If the hash entry is not in ht0, we rehash the buckets based
             * on the rehashidx (not CPU cache friendly). */
            dictRehashStep(d);
        }
    }

    for (table = 0; table <= 1; table++) {
        if (table == 0 && (long)idx < d->rehashidx) continue;
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        he = d->ht_table[table][idx];
        while (he) {
            void *he_key = dictGetKey(he);
            if (key == he_key || dictCompareKeys(d, key, he_key)) return he;
            he = dictGetNext(he);
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;

    he = dictFind(d, key);
    return he ? dictGetVal(he) : NULL;
}

/* Find an element from the table, also get the plink of the entry. The entry
 * is returned if the element is found, and the user should later call
 * `dictTwoPhaseUnlinkFree` with it in order to unlink and release it. Otherwise if
 * the key is not found, NULL is returned. These two functions should be used in pair.
 * `dictTwoPhaseUnlinkFind` pauses rehash and `dictTwoPhaseUnlinkFree` resumes rehash.
 *
 * We can use like this:
 *
 * dictEntry *de = dictTwoPhaseUnlinkFind(db->dict,key->ptr,&plink, &table);
 * // Do something, but we can't modify the dict
 * dictTwoPhaseUnlinkFree(db->dict,de,plink,table); // We don't need to lookup again
 *
 * If we want to find an entry before delete this entry, this an optimization to avoid
 * dictFind followed by dictDelete. i.e. the first API is a find, and it gives some info
 * to the second one to avoid repeating the lookup
 */
dictEntry *dictTwoPhaseUnlinkFind(dict *d, const void *key, dictEntry ***plink, int *table_index) {
    uint64_t h, idx, table;

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    if (dictIsRehashing(d)) dictRehashStep(d);
    h = dictHashKey(d, key);

    for (table = 0; table <= 1; table++) {
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        if (table == 0 && (long)idx < d->rehashidx) continue;
        dictEntry **ref = &d->ht_table[table][idx];
        while (ref && *ref) {
            void *de_key = dictGetKey(*ref);
            if (key == de_key || dictCompareKeys(d, key, de_key)) {
                *table_index = table;
                *plink = ref;
                dictPauseRehashing(d);
                return *ref;
            }
            ref = dictGetNextRef(*ref);
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

void dictTwoPhaseUnlinkFree(dict *d, dictEntry *he, dictEntry **plink, int table_index) {
    if (he == NULL) return;
    d->ht_used[table_index]--;
    *plink = dictGetNext(he);
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    if (!entryIsKey(he)) zfree(decodeMaskedPtr(he));
    dictShrinkIfAutoResizeAllowed(d);
    dictResumeRehashing(d);
}

/* In the macros below, `de` stands for dict entry. */
#define DICT_SET_VALUE(de, field, val)                        \
    {                                                         \
        if (entryIsNormal(de)) {                              \
            dictEntryNormal *_de = decodeEntryNormal(de);     \
            _de->field = val;                                 \
        } else if (entryIsEmbedded(de)) {                     \
            dictEntryEmbedded *_de = decodeEntryEmbedded(de); \
            _de->field = val;                                 \
        } else {                                              \
            panic("Entry type not supported");                \
        }                                                     \
    }
#define DICT_INCR_VALUE(de, field, val)                       \
    {                                                         \
        if (entryIsNormal(de)) {                              \
            dictEntryNormal *_de = decodeEntryNormal(de);     \
            _de->field += val;                                \
        } else if (entryIsEmbedded(de)) {                     \
            dictEntryEmbedded *_de = decodeEntryEmbedded(de); \
            _de->field += val;                                \
        } else {                                              \
            panic("Entry type not supported");                \
        }                                                     \
    }
#define DICT_GET_VALUE(de, field)                                              \
    (entryIsNormal(de) ? decodeEntryNormal(de)->field                          \
                       : (entryIsEmbedded(de) ? decodeEntryEmbedded(de)->field \
                                              : (panic("Entry type not supported"), ((dictEntryNormal *)de)->field)))
#define DICT_GET_VALUE_PTR(de, field)    \
    (entryIsNormal(de)                   \
         ? &decodeEntryNormal(de)->field \
         : (entryIsEmbedded(de) ? &decodeEntryEmbedded(de)->field : (panic("Entry type not supported"), NULL)))

void dictSetKey(dict *d, dictEntry *de, void *key) {
    void *k = d->type->keyDup ? d->type->keyDup(key) : key;
    if (entryIsNormal(de)) {
        dictEntryNormal *_de = decodeEntryNormal(de);
        _de->key = k;
    } else if (entryIsNoValue(de)) {
        dictEntryNoValue *_de = decodeEntryNoValue(de);
        _de->key = k;
    } else {
        panic("Entry type not supported");
    }
}

void dictSetVal(dict *d, dictEntry *de, void *val) {
    UNUSED(d);
    DICT_SET_VALUE(de, v.val, val);
}

void dictSetSignedIntegerVal(dictEntry *de, int64_t val) {
    DICT_SET_VALUE(de, v.s64, val);
}

void dictSetUnsignedIntegerVal(dictEntry *de, uint64_t val) {
    DICT_SET_VALUE(de, v.u64, val);
}

void dictSetDoubleVal(dictEntry *de, double val) {
    DICT_SET_VALUE(de, v.d, val);
}

int64_t dictIncrSignedIntegerVal(dictEntry *de, int64_t val) {
    DICT_INCR_VALUE(de, v.s64, val);
    return DICT_GET_VALUE(de, v.s64);
}

uint64_t dictIncrUnsignedIntegerVal(dictEntry *de, uint64_t val) {
    DICT_INCR_VALUE(de, v.u64, val);
    return DICT_GET_VALUE(de, v.u64);
}

double dictIncrDoubleVal(dictEntry *de, double val) {
    DICT_INCR_VALUE(de, v.d, val);
    return DICT_GET_VALUE(de, v.d);
}

void *dictGetKey(const dictEntry *de) {
    if (entryIsKey(de)) return (void *)de;
    if (entryIsNoValue(de)) return decodeEntryNoValue(de)->key;
    if (entryIsEmbedded(de)) return getEmbeddedKey(de);
    return decodeEntryNormal(de)->key;
}

void *dictGetVal(const dictEntry *de) {
    return DICT_GET_VALUE(de, v.val);
}

int64_t dictGetSignedIntegerVal(const dictEntry *de) {
    return DICT_GET_VALUE(de, v.s64);
}

uint64_t dictGetUnsignedIntegerVal(const dictEntry *de) {
    return DICT_GET_VALUE(de, v.u64);
}

double dictGetDoubleVal(const dictEntry *de) {
    return DICT_GET_VALUE(de, v.d);
}

/* Returns a mutable reference to the value as a double within the entry. */
double *dictGetDoubleValPtr(dictEntry *de) {
    return DICT_GET_VALUE_PTR(de, v.d);
}

/* Returns the 'next' field of the entry or NULL if the entry doesn't have a
 * 'next' field. */
dictEntry *dictGetNext(const dictEntry *de) {
    if (entryIsKey(de)) return NULL; /* there's no next */
    if (entryIsNoValue(de)) return decodeEntryNoValue(de)->next;
    if (entryIsEmbedded(de)) return decodeEntryEmbedded(de)->next;
    return decodeEntryNormal(de)->next;
}

/* Returns a pointer to the 'next' field in the entry or NULL if the entry
 * doesn't have a next field. */
static dictEntry **dictGetNextRef(dictEntry *de) {
    if (entryIsKey(de)) return NULL;
    if (entryIsNoValue(de)) return &decodeEntryNoValue(de)->next;
    if (entryIsEmbedded(de)) return &decodeEntryEmbedded(de)->next;
    return &decodeEntryNormal(de)->next;
}

static void dictSetNext(dictEntry *de, dictEntry *next) {
    if (entryIsNoValue(de)) {
        decodeEntryNoValue(de)->next = next;
    } else if (entryIsEmbedded(de)) {
        decodeEntryEmbedded(de)->next = next;
    } else {
        assert(entryIsNormal(de));
        decodeEntryNormal(de)->next = next;
    }
}

/* Returns the memory usage in bytes of the dict, excluding the size of the keys
 * and values. */
size_t dictMemUsage(const dict *d) {
    return dictSize(d) * sizeof(dictEntryNormal) + dictBuckets(d) * sizeof(dictEntry *);
}

/* Returns the memory usage in bytes of dictEntry based on the type. if `de` is NULL, return the size of
 * regular dict entry else return based on the type. */
size_t dictEntryMemUsage(dictEntry *de) {
    if (de == NULL || entryIsNormal(de))
        return sizeof(dictEntryNormal);
    else if (entryIsKey(de))
        return 0;
    else if (entryIsNoValue(de))
        return sizeof(dictEntryNoValue);
    else if (entryIsEmbedded(de))
        return zmalloc_size(decodeEntryEmbedded(de));
    else
        panic("Entry type not supported");
    return 0;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
unsigned long long dictFingerprint(dict *d) {
    unsigned long long integers[6], hash = 0;
    int j;

    integers[0] = (long)d->ht_table[0];
    integers[1] = d->ht_size_exp[0];
    integers[2] = d->ht_used[0];
    integers[3] = (long)d->ht_table[1];
    integers[4] = d->ht_size_exp[1];
    integers[5] = d->ht_used[1];

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

/* Initiaize a normal iterator. This function should be called when initializing
 * an iterator on the stack. */
void dictInitIterator(dictIterator *iter, dict *d) {
    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
}

/* Initialize a safe iterator, which is allowed to modify the dictionary while iterating.
 * You must call dictResetIterator when you are done with a safe iterator. */
void dictInitSafeIterator(dictIterator *iter, dict *d) {
    dictInitIterator(iter, d);
    iter->safe = 1;
}

void dictResetIterator(dictIterator *iter) {
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe) {
            dictResumeRehashing(iter->d);
            assert(iter->d->pauserehash >= 0);
        } else
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
}

dictIterator *dictGetIterator(dict *d) {
    dictIterator *iter = zmalloc(sizeof(*iter));
    dictInitIterator(iter, d);
    return iter;
}

dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

dictEntry *dictNext(dictIterator *iter) {
    while (1) {
        if (iter->entry == NULL) {
            if (iter->index == -1 && iter->table == 0) {
                if (iter->safe)
                    dictPauseRehashing(iter->d);
                else
                    iter->fingerprint = dictFingerprint(iter->d);

                /* skip the rehashed slots in table[0] */
                if (dictIsRehashing(iter->d)) {
                    iter->index = iter->d->rehashidx - 1;
                }
            }
            iter->index++;
            if (iter->index >= (long)DICTHT_SIZE(iter->d->ht_size_exp[iter->table])) {
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                } else {
                    break;
                }
            }
            iter->entry = iter->d->ht_table[iter->table][iter->index];
        } else {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = dictGetNext(iter->entry);
            return iter->entry;
        }
    }
    return NULL;
}

void dictReleaseIterator(dictIterator *iter) {
    dictResetIterator(iter);
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
dictEntry *dictGetRandomKey(dict *d) {
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL;
    if (dictIsRehashing(d)) dictRehashStep(d);
    if (dictIsRehashing(d)) {
        unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            h = d->rehashidx + (randomULong() % (dictBuckets(d) - d->rehashidx));
            he = (h >= s0) ? d->ht_table[1][h - s0] : d->ht_table[0][h];
        } while (he == NULL);
    } else {
        unsigned long m = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
        do {
            h = randomULong() & m;
            he = d->ht_table[0][h];
        } while (he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    listlen = 0;
    orighe = he;
    while (he) {
        he = dictGetNext(he);
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while (listele--) he = dictGetNext(he);
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j;      /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    if (dictSize(d) < count) count = dictSize(d);
    maxsteps = count * 10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            dictRehashStep(d);
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
    if (tables > 1 && maxsizemask < DICTHT_SIZE_MASK(d->ht_size_exp[1]))
        maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[1]);

    /* Pick a random point inside the larger table. */
    unsigned long i = randomULong() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    while (stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < (unsigned long)d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= DICTHT_SIZE(d->ht_size_exp[1]))
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= DICTHT_SIZE(d->ht_size_exp[j])) continue; /* Out of range for this table. */
            dictEntry *he = d->ht_table[j][i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = randomULong() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non empty while iterating.
                     * To avoid the issue of being unable to sample the end of a long chain,
                     * we utilize the Reservoir Sampling algorithm to optimize the sampling process.
                     * This means that even when the maximum number of samples has been reached,
                     * we continue sampling until we reach the end of the chain.
                     * See https://en.wikipedia.org/wiki/Reservoir_sampling. */
                    if (stored < count) {
                        des[stored] = he;
                    } else {
                        unsigned long r = randomULong() % (stored + 1);
                        if (r < count) des[r] = he;
                    }

                    he = dictGetNext(he);
                    stored++;
                }
                if (stored >= count) goto end;
            }
        }
        i = (i + 1) & maxsizemask;
    }

end:
    return stored > count ? count : stored;
}


/* Reallocate the dictEntry, key and value allocations in a bucket using the
 * provided allocation functions in order to defrag them. */
static void dictDefragBucket(dictEntry **bucketref, const dictDefragFunctions *defragfns, void *privdata) {
    dictDefragAllocFunction *defragalloc = defragfns->defragAlloc;
    dictDefragAllocFunction *defragkey = defragfns->defragKey;
    dictDefragAllocFunction *defragval = defragfns->defragVal;
    while (bucketref && *bucketref) {
        dictEntry *de = *bucketref, *newde = NULL;
        void *newkey = defragkey ? defragkey(dictGetKey(de)) : NULL;
        void *newval = defragval ? defragval(dictGetVal(de)) : NULL;
        if (entryIsKey(de)) {
            if (newkey) *bucketref = newkey;
            assert(entryIsKey(*bucketref));
        } else if (entryIsNoValue(de)) {
            dictEntryNoValue *entry = decodeEntryNoValue(de), *newentry;
            if ((newentry = defragalloc(entry))) {
                newde = encodeMaskedPtr(newentry, ENTRY_PTR_NO_VALUE);
                entry = newentry;
            }
            if (newkey) entry->key = newkey;
        } else if (entryIsEmbedded(de)) {
            defragfns->defragEntryStartCb(privdata, de);
            dictEntryEmbedded *entry = decodeEntryEmbedded(de), *newentry;
            if ((newentry = defragalloc(entry))) {
                newde = encodeMaskedPtr(newentry, ENTRY_PTR_EMBEDDED);
                entry = newentry;
                defragfns->defragEntryFinishCb(privdata, newde);
            } else {
                defragfns->defragEntryFinishCb(privdata, NULL);
            }
            if (newval) entry->v.val = newval;
        } else {
            assert(entryIsNormal(de));
            dictEntryNormal *entry = decodeEntryNormal(de), *newentry;
            newentry = defragalloc(entry);
            newde = encodeMaskedPtr(newentry, ENTRY_PTR_NORMAL);
            if (newde) entry = newentry;
            if (newkey) entry->key = newkey;
            if (newval) entry->v.val = newval;
        }
        if (newde) {
            *bucketref = newde;
        }
        bucketref = dictGetNextRef(*bucketref);
    }
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lengths. */
#define GETFAIR_NUM_ENTRIES 15
dictEntry *dictGetFairRandomKey(dict *d) {
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    unsigned int count = dictGetSomeKeys(d, entries, GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yield the element if the hash table has at least one. */
    if (count == 0) return dictGetRandomKey(d);
    unsigned int idx = rand() % count;
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata) {
    return dictScanDefrag(d, v, fn, NULL, privdata);
}

/* Like dictScan, but additionally reallocates the memory used by the dict
 * entries using the provided allocation function. This feature was added for
 * the active defrag feature.
 *
 * The 'defragfns' callbacks are called with a pointer to memory that callback
 * can reallocate. The callbacks should return a new memory address or NULL,
 * where NULL means that no reallocation happened and the old memory is still
 * valid. */
unsigned long
dictScanDefrag(dict *d, unsigned long v, dictScanFunction *fn, const dictDefragFunctions *defragfns, void *privdata) {
    int htidx0, htidx1;
    const dictEntry *de, *next;
    unsigned long m0, m1;

    if (dictSize(d) == 0) return 0;

    /* This is needed in case the scan callback tries to do dictFind or alike. */
    dictPauseRehashing(d);

    if (!dictIsRehashing(d)) {
        htidx0 = 0;
        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);

        /* Emit entries at cursor */
        if (defragfns) {
            dictDefragBucket(&d->ht_table[htidx0][v & m0], defragfns, privdata);
        }
        de = d->ht_table[htidx0][v & m0];
        while (de) {
            next = dictGetNext(de);
            fn(privdata, de);
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        v |= ~m0;

        /* Increment the reverse cursor */
        v = rev(v);
        v++;
        v = rev(v);

    } else {
        htidx0 = 0;
        htidx1 = 1;

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (DICTHT_SIZE(d->ht_size_exp[htidx0]) > DICTHT_SIZE(d->ht_size_exp[htidx1])) {
            htidx0 = 1;
            htidx1 = 0;
        }

        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);
        m1 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx1]);

        /* Emit entries at cursor */
        if (defragfns) {
            dictDefragBucket(&d->ht_table[htidx0][v & m0], defragfns, privdata);
        }
        de = d->ht_table[htidx0][v & m0];
        while (de) {
            next = dictGetNext(de);
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            if (defragfns) {
                dictDefragBucket(&d->ht_table[htidx1][v & m1], defragfns, privdata);
            }
            de = d->ht_table[htidx1][v & m1];
            while (de) {
                next = dictGetNext(de);
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    dictResumeRehashing(d);

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Because we may need to allocate huge memory chunk at once when dict
 * resizes, we will check this allocation is allowed or not if the dict
 * type has resizeAllowed member function. */
static int dictTypeResizeAllowed(dict *d, size_t size) {
    if (d->type->resizeAllowed == NULL) return 1;
    return d->type->resizeAllowed(DICTHT_SIZE(dictNextExp(size)) * sizeof(dictEntry *),
                                  (double)d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]));
}

/* Returning DICT_OK indicates a successful expand or the dictionary is undergoing rehashing,
 * and there is nothing else we need to do about this dictionary currently. While DICT_ERR indicates
 * that expand has not been triggered (may be try shrinking?)*/
int dictExpandIfNeeded(dict *d) {
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    if (DICTHT_SIZE(d->ht_size_exp[0]) == 0) {
        dictExpand(d, DICT_HT_INITIAL_SIZE);
        return DICT_OK;
    }

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    if ((dict_can_resize == DICT_RESIZE_ENABLE && d->ht_used[0] >= DICTHT_SIZE(d->ht_size_exp[0])) ||
        (dict_can_resize != DICT_RESIZE_FORBID &&
         d->ht_used[0] >= dict_force_resize_ratio * DICTHT_SIZE(d->ht_size_exp[0]))) {
        if (dictTypeResizeAllowed(d, d->ht_used[0] + 1)) dictExpand(d, d->ht_used[0] + 1);
        return DICT_OK;
    }
    return DICT_ERR;
}

/* Returning DICT_OK indicates a successful shrinking or the dictionary is undergoing rehashing,
 * and there is nothing else we need to do about this dictionary currently. While DICT_ERR indicates
 * that shrinking has not been triggered (may be try expanding?)*/
int dictShrinkIfNeeded(dict *d) {
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the size of hash table is DICT_HT_INITIAL_SIZE, don't shrink it. */
    if (DICTHT_SIZE(d->ht_size_exp[0]) <= DICT_HT_INITIAL_SIZE) return DICT_OK;

    /* If we reached below 1:8 elements/buckets ratio, and we are allowed to resize
     * the hash table (global setting) or we should avoid it but the ratio is below 1:32,
     * we'll trigger a resize of the hash table. */
    if ((dict_can_resize == DICT_RESIZE_ENABLE &&
         d->ht_used[0] * HASHTABLE_MIN_FILL <= DICTHT_SIZE(d->ht_size_exp[0])) ||
        (dict_can_resize != DICT_RESIZE_FORBID &&
         d->ht_used[0] * HASHTABLE_MIN_FILL * dict_force_resize_ratio <= DICTHT_SIZE(d->ht_size_exp[0]))) {
        if (dictTypeResizeAllowed(d, d->ht_used[0])) dictShrink(d, d->ht_used[0]);
        return DICT_OK;
    }
    return DICT_ERR;
}

/* Finds and returns the position within the dict where the provided key should
 * be inserted using dictInsertAtPosition if the key does not already exist in
 * the dict. If the key exists in the dict, NULL is returned and the optional
 * 'existing' entry pointer is populated, if provided. */
void *dictFindPositionForInsert(dict *d, const void *key, dictEntry **existing) {
    unsigned long idx, table;
    dictEntry *he;
    if (existing) *existing = NULL;
    uint64_t hash = dictHashKey(d, key);
    idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[0]);

    if (dictIsRehashing(d)) {
        if ((long)idx >= d->rehashidx && d->ht_table[0][idx]) {
            /* If we have a valid hash entry at `idx` in ht0, we perform
             * rehash on the bucket at `idx` (being more CPU cache friendly) */
            dictBucketRehash(d, idx);
        } else {
            /* If the hash entry is not in ht0, we rehash the buckets based
             * on the rehashidx (not CPU cache friendly). */
            dictRehashStep(d);
        }
    }

    /* Expand the hash table if needed */
    dictExpandIfAutoResizeAllowed(d);
    for (table = 0; table <= 1; table++) {
        if (table == 0 && (long)idx < d->rehashidx) continue;
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        /* Search if this slot does not already contain the given key */
        he = d->ht_table[table][idx];
        while (he) {
            void *he_key = dictGetKey(he);
            if (key == he_key || dictCompareKeys(d, key, he_key)) {
                if (existing) *existing = he;
                return NULL;
            }
            he = dictGetNext(he);
        }
        if (!dictIsRehashing(d)) break;
    }

    /* If we are in the process of rehashing the hash table, the bucket is
     * always returned in the context of the second (new) hash table. */
    dictEntry **bucket = &d->ht_table[dictIsRehashing(d) ? 1 : 0][idx];
    return bucket;
}

void dictEmpty(dict *d, void(callback)(dict *)) {
    /* Someone may be monitoring a dict that started rehashing, before
     * destroying the dict fake completion. */
    if (dictIsRehashing(d) && d->type->rehashingCompleted) d->type->rehashingCompleted(d);
    dictClear(d, 0, callback);
    dictClear(d, 1, callback);
    d->rehashidx = -1;
    d->pauserehash = 0;
    d->pauseAutoResize = 0;
}

void dictSetResizeEnabled(dictResizeEnable enable) {
    dict_can_resize = enable;
}

uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Provides the old and new ht size for a given dictionary during rehashing. This method
 * should only be invoked during initialization/rehashing. */
void dictRehashingInfo(dict *d, unsigned long long *from_size, unsigned long long *to_size) {
    /* Invalid method usage if rehashing isn't ongoing. */
    assert(dictIsRehashing(d));
    *from_size = DICTHT_SIZE(d->ht_size_exp[0]);
    *to_size = DICTHT_SIZE(d->ht_size_exp[1]);
}

/* ------------------------------- Debugging ---------------------------------*/
#define DICT_STATS_VECTLEN 50
void dictFreeStats(dictStats *stats) {
    zfree(stats->clvector);
    zfree(stats);
}

void dictCombineStats(dictStats *from, dictStats *into) {
    into->buckets += from->buckets;
    into->maxChainLen = (from->maxChainLen > into->maxChainLen) ? from->maxChainLen : into->maxChainLen;
    into->totalChainLen += from->totalChainLen;
    into->htSize += from->htSize;
    into->htUsed += from->htUsed;
    for (int i = 0; i < DICT_STATS_VECTLEN; i++) {
        into->clvector[i] += from->clvector[i];
    }
}

dictStats *dictGetStatsHt(dict *d, int htidx, int full) {
    unsigned long *clvector = zcalloc(sizeof(unsigned long) * DICT_STATS_VECTLEN);
    dictStats *stats = zcalloc(sizeof(dictStats));
    stats->htidx = htidx;
    stats->clvector = clvector;
    stats->htSize = DICTHT_SIZE(d->ht_size_exp[htidx]);
    stats->htUsed = d->ht_used[htidx];
    if (!full) return stats;
    /* Compute stats. */
    for (unsigned long i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]); i++) {
        dictEntry *he;

        if (d->ht_table[htidx][i] == NULL) {
            clvector[0]++;
            continue;
        }
        stats->buckets++;
        /* For each hash entry on this slot... */
        unsigned long chainlen = 0;
        he = d->ht_table[htidx][i];
        while (he) {
            chainlen++;
            he = dictGetNext(he);
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN - 1)]++;
        if (chainlen > stats->maxChainLen) stats->maxChainLen = chainlen;
        stats->totalChainLen += chainlen;
    }

    return stats;
}

/* Generates human readable stats. */
size_t dictGetStatsMsg(char *buf, size_t bufsize, dictStats *stats, int full) {
    if (stats->htUsed == 0) {
        return snprintf(buf, bufsize,
                        "Hash table %d stats (%s):\n"
                        "No stats available for empty dictionaries\n",
                        stats->htidx, (stats->htidx == 0) ? "main hash table" : "rehashing target");
    }
    size_t l = 0;
    l += snprintf(buf + l, bufsize - l,
                  "Hash table %d stats (%s):\n"
                  " table size: %lu\n"
                  " number of elements: %lu\n",
                  stats->htidx, (stats->htidx == 0) ? "main hash table" : "rehashing target", stats->htSize,
                  stats->htUsed);
    if (full) {
        l += snprintf(buf + l, bufsize - l,
                      " different slots: %lu\n"
                      " max chain length: %lu\n"
                      " avg chain length (counted): %.02f\n"
                      " avg chain length (computed): %.02f\n"
                      " Chain length distribution:\n",
                      stats->buckets, stats->maxChainLen, (float)stats->totalChainLen / stats->buckets,
                      (float)stats->htUsed / stats->buckets);

        for (unsigned long i = 0; i < DICT_STATS_VECTLEN - 1; i++) {
            if (stats->clvector[i] == 0) continue;
            if (l >= bufsize) break;
            l += snprintf(buf + l, bufsize - l, "   %ld: %ld (%.02f%%)\n", i, stats->clvector[i],
                          ((float)stats->clvector[i] / stats->htSize) * 100);
        }
    }

    /* Make sure there is a NULL term at the end. */
    buf[bufsize - 1] = '\0';
    /* Unlike snprintf(), return the number of characters actually written. */
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d, int full) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    dictStats *mainHtStats = dictGetStatsHt(d, 0, full);
    l = dictGetStatsMsg(buf, bufsize, mainHtStats, full);
    dictFreeStats(mainHtStats);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        dictStats *rehashHtStats = dictGetStatsHt(d, 1, full);
        dictGetStatsMsg(buf, bufsize, rehashHtStats, full);
        dictFreeStats(rehashHtStats);
    }
    /* Make sure there is a NULL term at the end. */
    orig_buf[orig_bufsize - 1] = '\0';
}
