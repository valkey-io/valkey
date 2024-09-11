/* Copyright (c) 2024-present, Valkey contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    * Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
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

#ifndef HASHTAB_H
#define HASHTAB_H

/* Hash table implementation.
 *
 * This is a cache-friendly hash table implementation. For details about the
 * implementation and documentation of functions, se comments in hashtab.c.
 *
 * The elements in a hashtab are of a user-defined type, but an element needs to
 * contain a key. It can represent a key-value entry, or it can be just a key,
 * if set semantics are desired.
 *
 * Terminology:
 *
 * hashtab
 *         An instance of the data structure.
 *
 * key
 *         A key used for looking up an element in the hashtab.
 *
 * element
 *         An element in the hashtab. This may be of the same type as the key,
 *         or a struct containing a key and other fields.
 *
 * type
 *         A struct containing callbacks, such as hash function, key comparison
 *         function and how to get the key in an element.
 */

#include "fmacros.h"
#include <stddef.h>
#include <stdint.h>

/* --- Opaque types --- */

typedef struct hashtab hashtab;

/* --- Non-opaque types --- */

/* The hashtabType is a set of callbacks for a hashtab. All callbacks are
 * optional. With all callbacks omitted, the hashtab is effectively a set of
 * pointer-sized integers. */
typedef struct {
    /* If the type of an element is not the same as the type of a key used for
     * lookup, this callback needs to return the key within an element. */
    const void *(*elementGetKey)(const void *element);
    /* Hash function. Defaults to hashing the bits in the pointer, effectively
     * treating the pointer as an integer. */
    uint64_t (*hashFunction)(const void *key);
    /* Compare function, returns 0 if the keys are equal. Defaults to just
     * comparing the pointers for equality. */
    int (*keyCompare)(hashtab *t, const void *key1, const void *key2);
    /* Callback to free an element when it's overwritten or deleted.
     * Optional. */
    void (*elementDestructor)(hashtab *t, void *elem);
    /* Optional callback to control when resizing should be allowed. (Not implemented) */
    int (*resizeAllowed)(size_t moreMem, double usedRatio);
    /* Invoked at the start of rehashing. Both tables are already created. */
    void (*rehashingStarted)(hashtab *t);
    /* Invoked at the end of rehashing. Both tables still exist and are cleaned
     * up after this callback. */
    void (*rehashingCompleted)(hashtab *t);
    /* Allow a hashtab to carry extra caller-defined metadata. The extra memory
     * is initialized to 0. */
    size_t (*getMetadataSize)(void);
    /* Flag to disable incremental rehashing */
    unsigned instant_rehashing : 1;
    /* Allow the caller to store some data here in the type. It's useful for the
     * rehashingStarted and rehashingCompleted callbacks. */
    void *userdata;
} hashtabType;

typedef enum {
    HASHTAB_RESIZE_ALLOW = 0,
    HASHTAB_RESIZE_AVOID,
    HASHTAB_RESIZE_FORBID,
} hashtabResizePolicy;

typedef void (*hashtabScanFunction)(void *privdata, void *element);

/* Scan flags */
#define HASHTAB_SCAN_EMIT_REF (1 << 0)
#define HASHTAB_SCAN_SINGLE_STEP (1 << 2)

typedef struct {
    hashtab *t;
    long index;
    int table;
    int posInBucket;
    /* unsafe iterator fingerprint for misuse detection. */
    uint64_t fingerprint;
    int safe;
} hashtabIterator;

typedef struct hashtabStats {
    int htidx;
    unsigned long buckets;       /* num buckets */
    unsigned long maxChainLen;   /* probing chain length */
    unsigned long totalChainLen; /* buckets with probing flag */
    unsigned long htSize;        /* buckets * positions-per-bucket */
    unsigned long htUsed;        /* num elements */
    unsigned long *clvector;
} hashtabStats;

/* --- Prototypes --- */

/* Hash function (global seed) */
void hashtabSetHashFunctionSeed(const uint8_t *seed);
uint8_t *hashtabGetHashFunctionSeed(void);
uint64_t hashtabGenHashFunction(const char *buf, size_t len);
uint64_t hashtabGenCaseHashFunction(const char *buf, size_t len);

/* Global resize policy */
void hashtabSetResizePolicy(hashtabResizePolicy policy);

/* Hashtab instance */
hashtab *hashtabCreate(hashtabType *type);
void hashtabRelease(hashtab *t);
void hashtabEmpty(hashtab *t, void(callback)(hashtab *));
hashtabType *hashtabGetType(hashtab *t);
void *hashtabMetadata(hashtab *t);
size_t hashtabSize(hashtab *t);
size_t hashtabMemUsage(hashtab *t);
void hashtabPauseAutoShrink(hashtab *t);
void hashtabResumeAutoShrink(hashtab *t);
int hashtabIsRehashing(hashtab *t);
void hashtabRehashingInfo(hashtab *t, size_t *from_size, size_t *to_size);
int hashtabExpand(hashtab *t, size_t size);
int hashtabTryExpand(hashtab *t, size_t size);
int hashtabExpandIfNeeded(hashtab *t);
int hashtabShrinkIfNeeded(hashtab *t);

/* Elements */
int hashtabFind(hashtab *t, const void *key, void **found);
int hashtabAdd(hashtab *t, void *elem);
int hashtabAddOrFind(hashtab *t, void *elem, void **existing);
void *hashtabFindPositionForInsert(hashtab *t, void *key, void **existing);
void hashtabInsertAtPosition(hashtab *t, void *elem, void *position);
int hashtabReplace(hashtab *t, void *elem);
int hashtabPop(hashtab *t, const void *key, void **popped);
int hashtabDelete(hashtab *t, const void *key);
int hashtabTwoPhasePopFind(hashtab *t, const void *key, void **found, void **position);
void hashtabTwoPhasePopDelete(hashtab *t, void *position);

/* Iteration & scan */
size_t hashtabScan(hashtab *t, size_t cursor, hashtabScanFunction fn, void *privdata, int emit_ref);
void hashtabInitIterator(hashtabIterator *iter, hashtab *t);
void hashtabInitSafeIterator(hashtabIterator *iter, hashtab *t);
void hashtabResetIterator(hashtabIterator *iter);
hashtabIterator *hashtabCreateIterator(hashtab *t);
hashtabIterator *hashtabCreateSafeIterator(hashtab *t);
void hashtabReleaseIterator(hashtabIterator *iter);
int hashtabNext(hashtabIterator *iter, void **elemptr);
#endif

/* Random elements */
int hashtabRandomElement(hashtab *t, void **found);
int hashtabFairRandomElement(hashtab *t, void **found);
unsigned hashtabSampleElements(hashtab *t, void **dst, unsigned count);

/* Debug & stats */

void hashtabFreeStats(hashtabStats *stats);
void hashtabCombineStats(hashtabStats *from, hashtabStats *into);
hashtabStats *hashtabGetStatsHt(hashtab *t, int htidx, int full);
size_t hashtabGetStatsMsg(char *buf, size_t bufsize, hashtabStats *stats, int full);
void hashtabGetStats(char *buf, size_t bufsize, hashtab *t, int full);
