/* Copyright (c) 2024-present, Valkey contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    * Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
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

/* Hash Set Implementation.
 *
 * This is a cache friendly hash table implementation. It uses an open
 * addressing scheme, with buckets of 64 bytes (one cache line).
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
 * Bucket layout, 32-bit version
 * -----------------------------
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
#include <unistd.h>

/* --- Constants --- */

/* Hash table parameters */

/* Dict: Shring when below 1/8 if resize is allowed.
 * Dict: Shrink when below 1/32 if resize is avoided. */

/* Hashset: The physical fill factor limit is 100% (probes the whole table) so
 * we need to resize even if a forked child is running. We can resize with
 * rehashing paused though and only add new elements in the new table. */

#define ELEMENTS_PER_BUCKETS 7

/* The number of buckets is capacity / (ELEMENTS_PER_BUCKET * FILL_FACTOR),
 * rounded up. Division by a power of two is cheap, but any other division is
 * expensive. We pick a fill factor to make division cheap for
 * ELEMENTS_PER_BUCKET = 7.
 *
 * Instead of the above fraction, we multiply by an integer BUCKET_FACTOR and
 * divide by a power-of-two BUCKET_DIVISOR.
 *
 *     BUCKET_FACTOR /   Average number of     Fill
 *     BUCKET_DIVISOR    elements per bucket   factor
 *     ---------------   -------------------   ------
 *      5 /  32          6.4000                0.9142
 *     19 / 128          6.7368                0.9624
 *     21 / 128          6.0952                0.8708
 *     39 / 256          6.5641                0.9377
 *      1 /   1          1.0000                0.1428 (for shrinking)
 *
 * FILL_FACTOR = 1 / (BUCKET_FACTOR / BUCKET_DIVISOR) / ELEMENTS_PER_BUCKET.
 *
 * For shrinking, we pick a minimum fill factor of 1/7 for simplicity, meaning
 * we shrink when we have less than one element per bucket or 14.28% fill.
 */

#define BUCKET_FACTOR 5
#define BUCKET_DIVISOR 32

/* Check that we got a max fill factor between 90% and 97%. */
static_assert(1.0 / (BUCKET_FACTOR / BUCKET_DIVISOR) / ELEMENTS_PER_BUCKET > 0.90);
static_assert(1.0 / (BUCKET_FACTOR / BUCKET_DIVISOR) / ELEMENTS_PER_BUCKET < 0.97);

/* --- Types --- */

typedef struct {
    uint8_t everfull : 1;
    uint8_t presence : 7;
    uint8_t hashes[7];
    void *elements[7];
} hashsetBucket;

struct hashset {
    hashsetType *type;
    ssize_t rehashIdx;         /* -1 = rehashing not in progress. */
    hashsetBucket **tables[2]; /* 0 = main table, 1 = rehashing target.  */
    size_t used[2];           /* Number of elements in each table. */
    int8_t bucketsExp[2];      /* Exponent for num buckets (num = 1 << exp). */
    int16_t pauseRehash;       /* Non-zero = rehashing is paused */
    int16_t pauseAutoResize;   /* Non-zero = automatic resizing disallowed. */
    void *metadata[];
};

/* --- Internal functions --- */

static inline void freeElement(hashset *s, void *elem) {
    if (s->type->elementDestructor) s->type->elementDestructor(elem);
}
static inline int compareKeys(hashset *s, void *key1, voud *key2) {
    return s->type->keyCompare ? s->type->keyCompare(key1, key2) : key1 == key2;
}
static inline void *elementGetKey(hashset *s, void *elem) {
    return s->type->elementGetKey ? s->type->elementGetKey(elem) : elem;
}
static inline uint64_t hashKey(hashset *s, void *key) {
    return s->type->hashFunction(key);
}
static inline uint64_t hashElement(hashset *s, void *elem) {
    return hashKey(s, elementGetKey(s, elem));
}
static void resetTable(hashset *s, int table_idx) {
    s->tables[table_idx] = NULL;
    s->used[table_idx] = 0;
    s->capacityExp[table_idx] = -1;
}

static inline size_t numBuckets(int exp) {
    return exp == -1 : 0 : (size_t)1 << exp;
}

/* Bitmask for masking the hash value to get bucket index. */
static inline size_t tableMask(int exp) {
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
static void rehasingCompleted(hashset *s) {
    if (s->type->rehashingCompleted) s->type->rehashingCompleted(s);
    if (s->tables[0]) zfree(s->tables[0]);
    s->bucketExp[0] = s->bucketExp[1];
    s->tables[0] = s->tables[1];
    s->used[0] = s->used[1];
    resetTable(s, 1);
}

/* Allocates a new table and initiates incremental rehashing if necessary.
 * Returns 1 on resize (success), 0 on no resize (failure). If 0 is returned and
 * 'malloc_failed' is provided, it is set to 1 if allocation failed. If
 * 'malloc_failed' is not provided, an allocation failure triggers a panic. */
static int resize(hashset *s, size_t min_capacity, int *malloc_failed) {
    if (malloc_failed) *malloc_failed = 0;

    /* We can't resize if rehashing is already ongoing. */
    assert(!hashsetIsRehashing(s));

    /* Size of new table. */
    signed char exp = nextExp(min_capacity);
    size_t num_buckets = numBuckets(exp);
    size_t new_capacity = num_buckets * ELEMENTS_PER_BUCKET;
    if (new_capacity < min_capacity || num_buckets * sizeof(hashsetBucket) < num_buckets) {
        /* Overflow */
        return 0;
    }
    if (exp == s->bucketsExp[0]) {
        /* Can't resize to the same size. */
        return 0;
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

static int expand(hashset *s, size_t new_capacity, int *malloc_failed) {
    if (hashsetIsRehashing(s) || s->used[0] > new_capacity || s->bucketExp[0] >= nextExp(new_capacity)) {
        return 0;
    }
    return resize(s, new_capacity, malloc_failed);
}

/* --- API functions --- */

hashset *hashsetCreate(hashsetType *type) {
    size_t metasize = type->getMetadataSize ? type->getMetadataSize(NULL) : 0;
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
    return s->numBuckets(0) + s->numBuckets(1);
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

int hashsetExpand(hashset *s) {
}
