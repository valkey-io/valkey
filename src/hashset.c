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

/* Cache-line aware hash table. Each element contains the key and value. A
 * callback function is used to retrieve the key from within the element.
 *
 * It uses an open addressing scheme, with buckets of 64 bytes (one cache line).
 * Each bucket contains metadata and element slots for a fixed number of
 * elements.
 *
 * Functions and types are prefixed by "hashset", macros by "HASHSET".
 *
 * Bucket layout, 64-bit version
 * -----------------------------
 *
 *   1 bit  7 bits   [1 byte] x 7    [8 bytes] * 7
 * [ e      ppppppp  hash1...hash7   elem1...elem7  ] = 64 bytes
 *
 * - e = ever full
 * - p = presence for the pointer at the corresponding element
 * - elemN = elements of the bucket, 64 bits per element
 *
 * Bucket layout, 32-bit version (TBD)
 * -----------------------------------
 *
 *   1b   3b  12b      2B        1B * 12         4B * 12
 * [ e    xxx ppppppp  unused    hash1...hash12  elem1...elem12 ] = 64 bytes (2B unused)
 *   ------2B--------  ---2B---  ----12B-------  ---48B-------
 *
 *   1 bit 2bits  13 bits  3 bytes  [4 bits] x 13   4bits  [4 bytes] * 13
 * [ e     xx     ppppppp  xxxxxxx  hash1...hash13  xxxx   elem1...elem13 ] = 64 bytes (3B unused)
 *   ---------2B---------  ---3B--  ----7B--------------   ----52B-------
 * 
 * [ e        xx     ppppppp  x         hash1...hash13    elem1...elem13 ] = 64 bytes (4bits unused)
 *   1 bit    2bits  13 bits  2b       [6 bits] x 13     [4 bytes] * 13
 *  ------------------------           78 bits                  52
 *         2B                 -----10----------------
 *
 *   1 bit 1bit  14 bits  2b  [3 bits]*14   [4 bytes]*14
 * [ e     x     ppppppp  xx  hhhhhhhhhhh   elem1 elem2 elem3 elem4 elem5 elem6 elem7 ] = 64 bytes (3bits unused)
 *  {---------2--------}  ----6B---------   --------------------56--------------------  = 64
 * 
 *
 * - e = ever full
 * - p = presence for the pointer at the corresponding element
 * - elemN = elements of the bucket, 64 bits per element
 * - x = unused
 */

#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serverassert.h"
#include "zmalloc.h"
#include "hashset.h"

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

/* --- Constants --- */

/* Hash table parameters */

/* Dict: Shring when below 1/8 if resize is allowed.
 * Dict: Shrink when below 1/32 if resize is avoided. */

/* Hashset: The physical fill factor limit is 100% (probes the whole table) so
 * we need to resize even if a forked child is running. We can resize with
 * rehashing paused though and only add new elements in the new table (to avoid
 * affecting memory shared with forked processes which are using copy-on-write
 * per memory page). */

#define MAX_FILL_PERCENT_SOFT 80
#define MAX_FILL_PERCENT_HARD 92

#define ELEMENTS_PER_BUCKET 7

/* Choosing the number of buckets.
 *
 * The number of buckets we want is NUM_ELEMENTS / (ELEMENTS_PER_BUCKET * FILL_FACTOR),
 * rounded up. The fill is the number of elements we have, or want to put, in
 * the table.
 *
 * Division by a power of two is cheap, but any other division is expensive. We
 * pick a fill factor to make division cheap for ELEMENTS_PER_BUCKET = 7.
 *
 * Instead of the above fraction, we multiply by an integer BUCKET_FACTOR and
 * divide by a power-of-two BUCKET_DIVISOR.
 *
 *     BUCKET_FACTOR /   Average number of     Fill
 *     BUCKET_DIVISOR    elements per bucket   factor
 *     ---------------   -------------------   ------
 *      3 /  16          5.3333                0.7619
 *      5 /  32          6.4000                0.9142
 *     19 / 128          6.7368                0.9624
 *     21 / 128          6.0952                0.8708
 *     39 / 256          6.5641                0.9377
 *     37 / 256          6.9189                0.9884
 *      1 /   1          1.0000                0.1428 (for shrinking)
 *
 * NUM_BUCKETS = ceil(num_elements * BUCKET_FACTOR / BUCKET_DIVISOR)
 *
 * FILL_FACTOR = 1 / (BUCKET_FACTOR / BUCKET_DIVISOR) / ELEMENTS_PER_BUCKET
 *             = BUCKET_DIVISOR / BUCKET_FACTOR / ELEMENTS_PER_BUCKET
 *
 * For shrinking, we use a minimum fill factor of 1/7, meaning we shrink when we
 * have less than one element per bucket or 14.28% fill.
 */

#define BUCKET_FACTOR 3
#define BUCKET_DIVISOR 16
/* #define BUCKET_FACTOR 5 */
/* #define BUCKET_DIVISOR 32 */
/* #define BUCKET_FACTOR 37 */
/* #define BUCKET_DIVISOR 256 */

#ifndef static_assert
#define static_assert _Static_assert
#endif

static_assert(100 * BUCKET_DIVISOR / BUCKET_FACTOR / ELEMENTS_PER_BUCKET <= MAX_FILL_PERCENT_SOFT,
              "Expand must result in a fill below the soft max fill factor.");
static_assert(MAX_FILL_PERCENT_SOFT <= MAX_FILL_PERCENT_HARD, "");
static_assert(MAX_FILL_PERCENT_HARD <= 100, "");

/* Incremental rehashing
 * ---------------------
 *
 * TODO: Describe the approach of two tables.
 *
 * The incremental rehashing approach using two hashtables was designed by
 * Salvatore Sanfilippo, though for a chained hashtable.
 *
 * To avoid affecting CoW when there is a fork, the dict avoids resizing in this
 * case. With an open addressing scheme, it is impossible to add more elements
 * than the number of slots, so we need to allow resizing even in this case. To
 * avoid affecting CoW, we resize with incremental rehashing paused, so only new
 * elements are added to the new table until the fork is done.
 *
 * This also means that we need to allow resizing even if rehashing is already
 * in progress. In the worst case, we need to resizing multiple times while a
 * fork is running. We can to fast-forward the rehashing in this case.
 */

/* Scan
 * ----
 *
 * Cursor incremented as in dict scan, i.e. by setting the unmasked bits,
 * reversing, incrementing and reversing again. The original SCAN algorithm (for
 * the chained hashtable) was designed by Pieter Noordhuis.
 *
 * We need to use scan-increment-probing instead of linear probing. When we
 * scan, we need to continue scanning as long a bucket in either of the tables
 * is tombstoned (has ever been full). */

/* --- Types --- */

typedef struct {
    uint8_t everfull : 1;
    uint8_t presence : ELEMENTS_PER_BUCKET;
    uint8_t hashes[ELEMENTS_PER_BUCKET];
    void *elements[ELEMENTS_PER_BUCKET];
} hashsetBucket;

struct hashset {
    hashsetType *type;
    ssize_t rehashIdx;        /* -1 = rehashing not in progress. */
    hashsetBucket *tables[2]; /* 0 = main table, 1 = rehashing target.  */
    size_t used[2];           /* Number of elements in each table. */
    int8_t bucketExp[2];      /* Exponent for num buckets (num = 1 << exp). */
    int16_t pauseRehash;      /* Non-zero = rehashing is paused */
    int16_t pauseAutoResize;  /* Non-zero = automatic resizing disallowed. */
    void *metadata[];
};

/* --- Globals --- */

static uint8_t hash_function_seed[16];
static hashsetResizePolicy resize_policy = HASHSET_RESIZE_ALLOW;

/* --- Internal functions --- */

static hashsetBucket *hashsetFindBucketForInsert(hashset *s, uint64_t hash, int *pos_in_bucket);

static inline void freeElement(hashset *s, void *elem) {
    if (s->type->elementDestructor) s->type->elementDestructor(s, elem);
}

static inline int compareKeys(hashset *s, const void *key1, const void *key2) {
    return s->type->keyCompare ? s->type->keyCompare(s, key1, key2)
                               : key1 == key2;
}

static inline const void *elementGetKey(hashset *s, const void *elem) {
    return s->type->elementGetKey ? s->type->elementGetKey(elem) : elem;
}

static inline uint64_t hashKey(hashset *s, const void *key) {
    return s->type->hashFunction(key);
}

static inline uint64_t hashElement(hashset *s, const void *elem) {
    return hashKey(s, elementGetKey(s, elem));
}

static inline int bucketIsFull(hashsetBucket *b) {
    return b->presence == (1 << ELEMENTS_PER_BUCKET) - 1;
}

static void resetTable(hashset *s, int table_idx) {
    s->tables[table_idx] = NULL;
    s->used[table_idx] = 0;
    s->bucketExp[table_idx] = -1;
}

static inline size_t numBuckets(int exp) {
    return exp == -1 ? 0 : (size_t)1 << exp;
}

/* Bitmask for masking the hash value to get bucket index. */
static inline size_t expToMask(int exp) {
    return exp == -1 ? 0 : numBuckets(exp) - 1;
}

/* Returns the 'exp', where num_buckets = 1 << exp. The number of
 * buckets is a power of two. */
static signed char nextBucketExp(size_t min_capacity) {
    if (min_capacity == 0) return -1;
    /* ceil(x / y) = floor((x - 1) / y) + 1 */
    size_t min_buckets = (min_capacity * BUCKET_FACTOR - 1) / BUCKET_DIVISOR + 1;
    if (min_buckets >= SIZE_MAX / 2) return CHAR_BIT * sizeof(size_t) - 1;
    return CHAR_BIT * sizeof(size_t) - __builtin_clzl(min_buckets - 1);
}

/* Swaps the tables and frees the old table. */
static void rehashingCompleted(hashset *s) {
    if (s->type->rehashingCompleted) s->type->rehashingCompleted(s);
    if (s->tables[0]) zfree(s->tables[0]);
    s->bucketExp[0] = s->bucketExp[1];
    s->tables[0] = s->tables[1];
    s->used[0] = s->used[1];
    resetTable(s, 1);
    s->rehashIdx = -1;
}

/* Reverse bits, adapted to use bswap, from
 * https://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
size_t rev(size_t v) {
#if SIZE_MAX == UINT64_MAX
    /* Swap odd and even bits. */
    v = ((v >> 1) & 0x5555555555555555) | ((v & 0x5555555555555555) << 1);
    /* Swap consecutive pairs. */
    v = ((v >> 2) & 0x3333333333333333) | ((v & 0x3333333333333333) << 2);
    /* Swap nibbles. */
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0F) | ((v & 0x0F0F0F0F0F0F0F0F) << 4);
    /* Reverse bytes. */
    v = __builtin_bswap64(v);
#else
    /* 32-bit version. */
    v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);
    v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);
    v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4);
    v = __builtin_bswap32(v);
#endif
    return v;
}

/* This algorithm was invented by Pieter Noordhuis. It increments the reverse
 * bit representation of the masked bits of v. */
static inline size_t incrementCursor(size_t v, size_t mask) {
    /* Set unmasked bits so incrementing the reversed cursor operates on the masked bits. */
    v |= ~mask;

    /* Increment the reverse cursor. */
    v = rev(v);
    v++;
    v = rev(v);
    return v;
    //return rev(rev(v | ~mask) + 1);
}

static inline size_t decrementCursor(size_t v, size_t mask) {
    v = rev(v);
    v--;
    v = rev(v);
    v = v & mask;
    return v;
    // return rev(rev(v) - 1) & mask;
}

/* Rehashes one bucket. */
static void rehashStep(hashset *s) {
    assert(hashsetIsRehashing(s));
    size_t idx = s->rehashIdx;
    hashsetBucket *b = &s->tables[0][idx];
    int pos;
    for (pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
        if (!(b->presence & (1 << pos))) continue; /* empty */
        void *elem = b->elements[pos];
        uint8_t h2 = b->hashes[pos];
        /* Insert into table 1. */
        uint64_t hash;
        /* When shrinking, it's possible to avoid computing the hash. We can
         * just use idx has the hash, but only if we know that probing didn't
         * push this element away from its primary bucket, so only if the
         * bucket before the current one hasn't ever been full. */
        if (s->bucketExp[1] < s->bucketExp[0] &&
            !s->tables[0][decrementCursor(idx, expToMask(s->bucketExp[0]))].everfull) {
            hash = idx;
        } else {
            hash = hashElement(s, elem);
        }
        int pos_in_dst_bucket;
        hashsetBucket *dst = hashsetFindBucketForInsert(s, hash, &pos_in_dst_bucket);
        dst->elements[pos_in_dst_bucket] = elem;
        dst->hashes[pos_in_dst_bucket] = h2;
        dst->presence |= (1 << pos_in_dst_bucket);
        dst->everfull |= bucketIsFull(dst);
        s->used[0]--;
        s->used[1]++;
    }
    /* Mark the source bucket as empty. */
    b->presence = 0;
    /* Done. */
    s->rehashIdx++;
    if ((size_t)s->rehashIdx == numBuckets(s->bucketExp[0])) {
        rehashingCompleted(s);
    }
}

/* Allocates a new table and initiates incremental rehashing if necessary.
 * Returns 1 on resize (success), 0 on no resize (failure). If 0 is returned and
 * 'malloc_failed' is provided, it is set to 1 if allocation failed. If
 * 'malloc_failed' is not provided, an allocation failure triggers a panic. */
static int resize(hashset *s, size_t min_capacity, int *malloc_failed) {
    if (malloc_failed) *malloc_failed = 0;

    /* Size of new table. */
    signed char exp = nextBucketExp(min_capacity);
    size_t num_buckets = numBuckets(exp);
    size_t new_capacity = num_buckets * ELEMENTS_PER_BUCKET;
    if (new_capacity < min_capacity || num_buckets * sizeof(hashsetBucket) < num_buckets) {
        /* Overflow */
        return 0;
    }
    signed char old_exp = s->bucketExp[hashsetIsRehashing(s) ? 1 : 0];
    if (exp == old_exp) {
        /* Can't resize to the same size. */
        return 0;
    }

    /* Only shrink if we're below one element per bucket (1/7 fill) */
    size_t old_num_buckets = numBuckets(s->bucketExp[hashsetIsRehashing(s) ? 1 : 0]);
    size_t old_capacity = old_num_buckets * ELEMENTS_PER_BUCKET;
    if (new_capacity < old_capacity && new_capacity > old_num_buckets) {
        return 0;
    }

    /* We can't resize if rehashing is already ongoing. Finish ongoing rehashing
     * before we continue. */
    while (hashsetIsRehashing(s)) {
        rehashStep(s);
    }

    /* Allocate the new hash table. */
    hashsetBucket *new_table;
    if (malloc_failed) {
        new_table = ztrycalloc(num_buckets * sizeof(hashsetBucket));
        if (new_table == NULL) {
            *malloc_failed = 1;
            return 0;
        }
    } else {
        new_table = zcalloc(num_buckets * sizeof(hashsetBucket));
    }
    s->bucketExp[1] = exp;
    s->tables[1] = new_table;
    s->used[1] = 0;
    s->rehashIdx = 0;
    if (s->type->rehashingStarted) s->type->rehashingStarted(s);

    /* If the old table was empty, the rehashing is completed immediately. */
    if (s->tables[0] == NULL || s->used[0] == 0) {
        rehashingCompleted(s);
    }
    return 1;
}

/* Returns 1 if expanding, 0 if not expanding. */
static int expandIfNeeded(hashset *s) {
    size_t min_capacity = s->used[0] + s->used[1] + 1;
    size_t num_buckets = numBuckets(s->bucketExp[hashsetIsRehashing(s) ? 1 : 0]);
    size_t current_capacity = num_buckets * ELEMENTS_PER_BUCKET;

    /* Expand if (min_capacity / current_capacity > max_fill_percent / 100). */
    unsigned max_fill_percent = resize_policy == HASHSET_RESIZE_ALLOW ? MAX_FILL_PERCENT_SOFT : MAX_FILL_PERCENT_HARD;
    if (min_capacity * 100 > current_capacity * max_fill_percent) {
        return resize(s, min_capacity, NULL);
    }
    return 0;
}

/* Finds an element matching the key. If a match is found, returns a pointer to
 * the bucket containing the matching element and points 'pos_in_bucket' to the
 * index within the bucket. Returns NULL if no matching element was found. */
static hashsetBucket *hashsetFindBucket(hashset *s, uint64_t hash, const void *key, int *pos_in_bucket) {
    if (hashsetCount(s) == 0) return 0;
    uint8_t h2 = hash >> (CHAR_BIT * 7); /* The high byte. */
    int table;

    /* Do some incremental rehashing. */
    if (hashsetIsRehashing(s) && resize_policy != HASHSET_RESIZE_FORBID) rehashStep(s);

    /* Check rehashing destination table first, since it is newer and typically
     * has less 'everfull' flagged buckets. Therefore it needs less probing for
     * lookup. */
    for (table = 1; table >= 0; table--) {
        if (s->used[table] == 0) continue;
        size_t mask = expToMask(s->bucketExp[table]);
        size_t bucket_idx = hash & mask;
        while (1) {
            hashsetBucket *b = &s->tables[table][bucket_idx];
            /* Find candidate elements with presence flag set and matching h2 hash. */
            for (int pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
                if ((b->presence & (1 << pos)) && b->hashes[pos] == h2) {
                    /* It's a candidate. */
                    void *elem = b->elements[pos];
                    const void *elem_key = elementGetKey(s, elem);
                    if (compareKeys(s, key, elem_key) == 0) {
                        /* It's a match. */
                        if (pos_in_bucket) *pos_in_bucket = pos;
                        return b;
                    }
                }
            }

            /* Probe the next bucket? */
            if (!b->everfull) break;
            bucket_idx = incrementCursor(bucket_idx, mask);
        }
    }
    return NULL;
}

/* Find an empty position in the table for inserting an element with the given hash. */
static hashsetBucket *hashsetFindBucketForInsert(hashset *s, uint64_t hash, int *pos_in_bucket) {
    int table = hashsetIsRehashing(s) ? 1 : 0;
    assert(s->tables[table]);
    size_t mask = expToMask(s->bucketExp[table]);
    size_t bucket_idx = hash & mask;
    while (1) {
        hashsetBucket *b = &s->tables[table][bucket_idx];
        for (int pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
            if (b->presence & (1 << pos)) continue; /* busy */
            if (pos_in_bucket) *pos_in_bucket = pos;
            return b;
        }
        bucket_idx = incrementCursor(bucket_idx, mask);
    }
}

/* Helper to insert an element. Doesn't check if an element with a matching key
 * already exists. This must be ensured by the caller. */
static void hashsetInsert(hashset *s, uint64_t hash, void *elem) {
    expandIfNeeded(s);
    uint8_t h2 = hash >> (CHAR_BIT * 7); /* The high byte. */
    int i;
    hashsetBucket *b = hashsetFindBucketForInsert(s, hash, &i);
    b->elements[i] = elem;
    b->presence |= (1 << i);
    b->hashes[i] = h2;
    b->everfull |= bucketIsFull(b);
    s->used[hashsetIsRehashing(s) ? 1 : 0]++;
}

/* --- Hash function API --- */

/* The seed needs to be 16 bytes. */
void hashsetSetHashFunctionSeed(uint8_t *seed) {
    memcpy(hash_function_seed, seed, sizeof(hash_function_seed));
}

uint8_t *hashsetGetHashFunctionSeed(void) {
    return hash_function_seed;
}

uint64_t hashsetGenHashFunction(const char *buf, size_t len) {
    return siphash((const uint8_t *)buf, len, hash_function_seed);
}

uint64_t hashsetGenCaseHashFunction(const char *buf, size_t len) {
    return siphash_nocase((const uint8_t *)buf, len, hash_function_seed);
}

/* --- Global resize policy API --- */

/* The global resize policy is one of
 *
 *   - HASHSET_RESIZE_ALLOW: Rehash as required for optimal performance.
 *   - HASHSET_RESIZE_AVOID: Don't rehash and move memory if it can be avoided;
 *     used when there is a fork running and we want to avoid affecting CoW
 *     memory.
 *   - HASHSET_RESIZE_FORBID: Don't rehash at all. Used in a child process which
 *     doesn't add any keys. */
void hashsetSetResizePolicy(hashsetResizePolicy policy) {
    resize_policy = policy;
}

/* --- API functions --- */

hashset *hashsetCreate(hashsetType *type) {
    size_t metasize = type->getMetadataSize ? type->getMetadataSize() : 0;
    hashset *s = zmalloc(sizeof(*s) + metasize);
    if (metasize > 0) {
        memset(&s->metadata, 0, metasize);
    }
    s->type = type;
    s->rehashIdx = -1;
    s->pauseRehash = 0;
    s->pauseAutoResize = 0;
    resetTable(s, 0);
    resetTable(s, 1);
    return s;
}

hashsetType *hashsetGetType(hashset *s) {
    return s->type;
}

void *hashsetMetadata(hashset *s) {
    return &s->metadata;
}

size_t hashsetNumBuckets(hashset *s) {
    return numBuckets(s->bucketExp[0]) + numBuckets(s->bucketExp[1]);
}

size_t hashsetCount(hashset *s) {
    return s->used[0] + s->used[1];
}

void hashtablePauseAutoResize(hashset *s) {
    s->pauseAutoResize++;
}

void hashsetResumeAutoResize(hashset *s) {
    s->pauseAutoResize--;
}

int hashsetIsRehashing(hashset *s) {
    return s->rehashIdx != -1;
}

/* Returns 1 if an element was found matching the key. Also points *found to it,
 * if found is provided. Returns 0 if no matching element was found. */
int hashsetFind(hashset *s, const void *key, void **found) {
    if (hashsetCount(s) == 0) return 0;
    uint64_t hash = hashKey(s, key);
    int pos_in_bucket = 0;
    hashsetBucket *b = hashsetFindBucket(s, hash, key, &pos_in_bucket);
    if (b) {
        if (found) *found = b->elements[pos_in_bucket];
        return 1;
    } else {
        return 0;
    }
}

/* Adds an element. Returns 1 on success. Returns 0 if there was already an element
 * with the same key. */
int hashsetAdd(hashset *s, void *elem) {
    return hashsetAddRaw(s, elem, NULL);
}

/* Adds an element and returns 1 on success. Returns 0 if there was already an
 * element with the same key and, if an 'existing' pointer is provided, it is
 * pointed to the existing element. */
int hashsetAddRaw(hashset *s, void *elem, void **existing) {
    const void *key = elementGetKey(s, elem);
    uint64_t hash = hashKey(s, key);
    int pos_in_bucket = 0;
    hashsetBucket *b = hashsetFindBucket(s, hash, key, &pos_in_bucket);
    if (b != NULL) {
        if (existing) *existing = b->elements[pos_in_bucket];
        return 0;
    } else {
        hashsetInsert(s, hash, elem);
        return 1;
    }
}

/* Add or overwrite. Returns 1 if an new element was inserted, 0 if an existing
 * element was overwritten. */
int hashsetReplace(hashset *s, void *elem) {
    const void *key = elementGetKey(s, elem);
    int pos_in_bucket = 0;
    uint64_t hash = hashKey(s, key);
    hashsetBucket *b = hashsetFindBucket(s, hash, key, &pos_in_bucket);
    if (b != NULL) {
        if (s->type->elementDestructor) {
            s->type->elementDestructor(s, b->elements[pos_in_bucket]);
        }
        b->elements[pos_in_bucket] = elem;
        return 0;
    } else {
        hashsetInsert(s, hash, elem);
        return 1;
    }
}

/* --- DEBUG --- */
void hashsetDump(hashset *s) {
    for (int table = 0; table <= 1; table++) {
        printf("Table %d, used %lu, exp %d\n", table, s->used[table], s->bucketExp[table]);
        for (size_t idx = 0; idx < numBuckets(s->bucketExp[table]); idx++) {
            hashsetBucket *b = &s->tables[table][idx];
            printf("Bucket %d:%lu everfull:%d\n", table, idx, b->everfull);
            for (int pos = 0; pos < ELEMENTS_PER_BUCKET; pos++) {
                printf("  %d ", pos);
                if (b->presence & (1 << pos)) {
                    printf("h2 %02x, key \"%s\"\n", b->hashes[pos], (const char *)elementGetKey(s, b->elements[pos]));
                } else {
                    printf("(empty)\n");
                }
            }
        }
    }
}

void hashsetHistogram(hashset *s) {
    //const char *symb = ".:-+x*=#";
    //const char *symb = ".123456#";
    for (int table = 0; table <= 1; table++) {
        //printf("Table %d elements per bucket:", table);
        for (size_t idx = 0; idx < numBuckets(s->bucketExp[table]); idx++) {
            hashsetBucket *b = &s->tables[table][idx];
            char c = b->presence == 0 && b->everfull ? 'X' : '0' + __builtin_popcount(b->presence);
            printf("%c", c);
        }
        if (table == 0) printf(" ");
    }
    printf("\n");
}
