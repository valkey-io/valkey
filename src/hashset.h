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

#ifndef HASHSET_H
#define HASHSET_H

/* Hash table implementation.
 *
 * This is a cache-friendly hash table implementation. For details about the
 * implementation and documentation of functions, se comments in hashset.c.
 *
 * The elements in a hashset are of a user-defined type, but an element needs to
 * contain a key. It can represent a key-value entry, or it can be just a key,
 * if set semantics are desired.
 *
 * Terminology:
 *
 * hashset
 *         An instance of the data structure.
 *
 * key
 *         A key used for looking up an element in the hashset.
 *
 * element
 *         An element in the hashset. This may be of the same type as the key,
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

typedef struct hashset hashset;

/* --- Non-opaque types --- */

/* The hashsetType is a set of callbacks for a hashset. All callbacks are
 * optional. With all callbacks omitted, the hashset is effectively a set of
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
    int (*keyCompare)(hashset *t, const void *key1, const void *key2);
    /* Callback to free an element when it's overwritten or deleted.
     * Optional. */
    void (*elementDestructor)(hashset *t, void *elem);
    /* Optional callback to control when resizing should be allowed. */
    int (*resizeAllowed)(size_t moreMem, double usedRatio);
    /* Invoked at the start of rehashing. Both tables are already created. */
    void (*rehashingStarted)(hashset *t);
    /* Invoked at the end of rehashing. Both tables still exist and are cleaned
     * up after this callback. */
    void (*rehashingCompleted)(hashset *t);
    /* Allow a hashset to carry extra caller-defined metadata. The extra memory
     * is initialized to 0. */
    size_t (*getMetadataSize)(void);
    /* Flag to disable incremental rehashing */
    unsigned instant_rehashing : 1;
    /* Allow the caller to store some data here in the type. It's useful for the
     * rehashingStarted and rehashingCompleted callbacks. */
    void *userdata;
} hashsetType;

typedef enum {
    HASHSET_RESIZE_ALLOW = 0,
    HASHSET_RESIZE_AVOID,
    HASHSET_RESIZE_FORBID,
} hashsetResizePolicy;

typedef void (*hashsetScanFunction)(void *privdata, void *element);

/* Constants */
#define HASHSET_BUCKET_SIZE 64 /* bytes */

/* Scan flags */
#define HASHSET_SCAN_EMIT_REF (1 << 0)
#define HASHSET_SCAN_SINGLE_STEP (1 << 2)

typedef struct {
    hashset *hashset;
    long index;
    int table;
    int posInBucket;
    /* unsafe iterator fingerprint for misuse detection. */
    uint64_t fingerprint;
    int safe;
} hashsetIterator;

typedef struct hashsetStats {
    int htidx;
    unsigned long buckets;       /* num buckets */
    unsigned long maxChainLen;   /* probing chain length */
    unsigned long totalChainLen; /* buckets with probing flag */
    unsigned long htSize;        /* buckets * positions-per-bucket */
    unsigned long htUsed;        /* num elements */
    unsigned long *clvector;
} hashsetStats;

/* --- Prototypes --- */

/* Hash function (global seed) */
void hashsetSetHashFunctionSeed(const uint8_t *seed);
uint8_t *hashsetGetHashFunctionSeed(void);
uint64_t hashsetGenHashFunction(const char *buf, size_t len);
uint64_t hashsetGenCaseHashFunction(const char *buf, size_t len);

/* Global resize policy */
void hashsetSetResizePolicy(hashsetResizePolicy policy);

/* Hashset instance */
hashset *hashsetCreate(hashsetType *type);
void hashsetRelease(hashset *t);
void hashsetEmpty(hashset *t, void(callback)(hashset *));
hashsetType *hashsetGetType(hashset *t);
void *hashsetMetadata(hashset *t);
size_t hashsetSize(hashset *t);
size_t hashsetBuckets(hashset *t);
size_t hashsetProbeCounter(hashset *t, int table);
size_t hashsetMemUsage(hashset *t);
void hashsetPauseAutoShrink(hashset *t);
void hashsetResumeAutoShrink(hashset *t);
int hashsetIsRehashing(hashset *t);
int hashsetIsRehashingPaused(hashset *t);
void hashsetRehashingInfo(hashset *t, size_t *from_size, size_t *to_size);
int hashsetRehashMicroseconds(hashset *s, uint64_t us);
int hashsetExpand(hashset *t, size_t size);
int hashsetTryExpand(hashset *t, size_t size);
int hashsetExpandIfNeeded(hashset *t);
int hashsetShrinkIfNeeded(hashset *t);
hashset *hashsetDefragInternals(hashset *t, void *(*defragfn)(void *));

/* Elements */
int hashsetFind(hashset *t, const void *key, void **found);
void **hashsetFindRef(hashset *t, const void *key);
/* void *hashsetFetchElement(hashset *t, const void *key); */
int hashsetAdd(hashset *t, void *elem);
int hashsetAddOrFind(hashset *t, void *elem, void **existing);
void *hashsetFindPositionForInsert(hashset *t, void *key, void **existing);
void hashsetInsertAtPosition(hashset *t, void *elem, void *position);
int hashsetReplace(hashset *t, void *elem);
int hashsetPop(hashset *t, const void *key, void **popped);
int hashsetDelete(hashset *t, const void *key);
void **hashsetTwoPhasePopFindRef(hashset *t, const void *key, void **position);
void hashsetTwoPhasePopDelete(hashset *t, void *position);

/* Iteration & scan */
size_t hashsetScan(hashset *t, size_t cursor, hashsetScanFunction fn, void *privdata, int flags);
void hashsetInitIterator(hashsetIterator *iter, hashset *t);
void hashsetInitSafeIterator(hashsetIterator *iter, hashset *t);
void hashsetResetIterator(hashsetIterator *iter);
hashsetIterator *hashsetCreateIterator(hashset *t);
hashsetIterator *hashsetCreateSafeIterator(hashset *t);
void hashsetReleaseIterator(hashsetIterator *iter);
int hashsetNext(hashsetIterator *iter, void **elemptr);
#endif

/* Random elements */
int hashsetRandomElement(hashset *t, void **found);
int hashsetFairRandomElement(hashset *t, void **found);
unsigned hashsetSampleElements(hashset *t, void **dst, unsigned count);

/* Debug & stats */

void hashsetFreeStats(hashsetStats *stats);
void hashsetCombineStats(hashsetStats *from, hashsetStats *into);
hashsetStats *hashsetGetStatsHt(hashset *t, int htidx, int full);
size_t hashsetGetStatsMsg(char *buf, size_t bufsize, hashsetStats *stats, int full);
void hashsetGetStats(char *buf, size_t bufsize, hashset *t, int full);
