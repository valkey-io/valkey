/*
 * Index-based KV store implementation
 * This file implements a KV store comprised of an array of hash tables (see hashset.c)
 * The purpose of this KV store is to have easy access to all keys that belong
 * in the same hash table (i.e. are in the same hashset-index)
 *
 * For example, when the server is running in cluster mode, we use kvstore to save
 * all keys that map to the same hash-slot in a separate hash table within the kvstore
 * struct.
 * This enables us to easily access all keys that map to a specific hash-slot.
 *
 * Copyright (c) Redis contributors.
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
#include "fmacros.h"

#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#include "zmalloc.h"
#include "kvstore.h"
#include "serverassert.h"
#include "monotonic.h"

#define UNUSED(V) ((void)V)

static hashset *kvstoreIteratorNextHashset(kvstoreIterator *kvs_it);

struct _kvstore {
    int flags;
    hashsetType *dtype;
    hashset **hashsets;
    long long num_hashsets;
    long long num_hashsets_bits;
    list *rehashing;                        /* List of hash tables in this kvstore that are currently rehashing. */
    int resize_cursor;                      /* Cron job uses this cursor to gradually resize hash tables (only used if num_hashsets > 1). */
    int allocated_hashsets;                 /* The number of allocated hashsets. */
    int non_empty_hashsets;                 /* The number of non-empty hashsets. */
    unsigned long long key_count;           /* Total number of keys in this kvstore. */
    unsigned long long bucket_count;        /* Total number of buckets in this kvstore across hash tables. */
    unsigned long long *hashset_size_index; /* Binary indexed tree (BIT) that describes cumulative key frequencies up until
                                            given hashset-index. */
    size_t overhead_hashtable_rehashing;    /* Num buckets overhead of hash tables rehashing. */
};

/* Structure for kvstore iterator that allows iterating across multiple hashsets. */
struct _kvstoreIterator {
    kvstore *kvs;
    long long didx;
    long long next_didx;
    hashsetIterator di;
};

/* Structure for kvstore hashset iterator that allows iterating the corresponding hashset. */
struct _kvstoreHashsetIterator {
    kvstore *kvs;
    long long didx;
    hashsetIterator di;
};

/* Hashset metadata for database, used for record the position in rehashing list. */
typedef struct {
    listNode *rehashing_node; /* list node in rehashing list */
    kvstore *kvs;
} kvstoreHashsetMetadata;

/**********************************/
/*** Helpers **********************/
/**********************************/

/* Get the hash table pointer based on hashset-index. */
hashset *kvstoreGetHashset(kvstore *kvs, int didx) {
    return kvs->hashsets[didx];
}

static hashset **kvstoreGetHashsetRef(kvstore *kvs, int didx) {
    return &kvs->hashsets[didx];
}

static int kvstoreHashsetIsRehashingPaused(kvstore *kvs, int didx) {
    hashset *d = kvstoreGetHashset(kvs, didx);
    return d ? hashsetIsRehashingPaused(d) : 0;
}

/* Returns total (cumulative) number of keys up until given hashset-index (inclusive).
 * Time complexity is O(log(kvs->num_hashsets)). */
static unsigned long long cumulativeKeyCountRead(kvstore *kvs, int didx) {
    if (kvs->num_hashsets == 1) {
        assert(didx == 0);
        return kvstoreSize(kvs);
    }
    int idx = didx + 1;
    unsigned long long sum = 0;
    while (idx > 0) {
        sum += kvs->hashset_size_index[idx];
        idx -= (idx & -idx);
    }
    return sum;
}

static void addHashsetIndexToCursor(kvstore *kvs, int didx, unsigned long long *cursor) {
    if (kvs->num_hashsets == 1) return;
    /* didx can be -1 when iteration is over and there are no more hashsets to visit. */
    if (didx < 0) return;
    *cursor = (*cursor << kvs->num_hashsets_bits) | didx;
}

static int getAndClearHashsetIndexFromCursor(kvstore *kvs, unsigned long long *cursor) {
    if (kvs->num_hashsets == 1) return 0;
    int didx = (int)(*cursor & (kvs->num_hashsets - 1));
    *cursor = *cursor >> kvs->num_hashsets_bits;
    return didx;
}

/* Updates binary index tree (also known as Fenwick tree), increasing key count for a given hashset.
 * You can read more about this data structure here https://en.wikipedia.org/wiki/Fenwick_tree
 * Time complexity is O(log(kvs->num_hashsets)). */
static void cumulativeKeyCountAdd(kvstore *kvs, int didx, long delta) {
    kvs->key_count += delta;

    hashset *s = kvstoreGetHashset(kvs, didx);
    size_t size = hashsetSize(s);
    if (delta < 0 && size == 0) {
        kvs->non_empty_hashsets--; /* It became empty. */
    } else if (delta > 0 && size == (size_t)delta) {
        kvs->non_empty_hashsets++; /* It was empty before. */
    }

    /* BIT does not need to be calculated when there's only one hashset. */
    if (kvs->num_hashsets == 1) return;

    /* Update the BIT */
    int idx = didx + 1; /* Unlike hashset indices, BIT is 1-based, so we need to add 1. */
    while (idx <= kvs->num_hashsets) {
        if (delta < 0) {
            assert(kvs->hashset_size_index[idx] >= (unsigned long long)labs(delta));
        }
        kvs->hashset_size_index[idx] += delta;
        idx += (idx & -idx);
    }
}

/* Create the hashset if it does not exist and return it. */
static hashset *createHashsetIfNeeded(kvstore *kvs, int didx) {
    hashset *d = kvstoreGetHashset(kvs, didx);
    if (d) return d;

    kvs->hashsets[didx] = hashsetCreate(kvs->dtype);
    kvstoreHashsetMetadata *metadata = (kvstoreHashsetMetadata *)hashsetMetadata(kvs->hashsets[didx]);
    metadata->kvs = kvs;
    kvs->allocated_hashsets++;
    return kvs->hashsets[didx];
}

/* Called when the hashset will delete entries, the function will check
 * KVSTORE_FREE_EMPTY_HASHSETS to determine whether the empty hashset needs
 * to be freed.
 *
 * Note that for rehashing hashsets, that is, in the case of safe iterators
 * and Scan, we won't delete the hashset. We will check whether it needs
 * to be deleted when we're releasing the iterator. */
static void freeHashsetIfNeeded(kvstore *kvs, int didx) {
    if (!(kvs->flags & KVSTORE_FREE_EMPTY_HASHSETS) || !kvstoreGetHashset(kvs, didx) || kvstoreHashsetSize(kvs, didx) != 0 ||
        kvstoreHashsetIsRehashingPaused(kvs, didx))
        return;
    hashsetRelease(kvs->hashsets[didx]);
    kvs->hashsets[didx] = NULL;
    kvs->allocated_hashsets--;
}

/*************************************/
/*** hashset callbacks ***************/
/*************************************/

/* Adds hash table to the rehashing list, which allows us
 * to quickly find rehash targets during incremental rehashing.
 *
 * If there are multiple hashsets, updates the bucket count for the given hash table
 * in a DB, bucket count incremented with the new ht size during the rehashing phase.
 * If there's one hashset, bucket count can be retrieved directly from single hashset bucket. */
void kvstoreHashsetRehashingStarted(hashset *d) {
    kvstoreHashsetMetadata *metadata = (kvstoreHashsetMetadata *)hashsetMetadata(d);
    kvstore *kvs = metadata->kvs;
    listAddNodeTail(kvs->rehashing, d);
    metadata->rehashing_node = listLast(kvs->rehashing);

    size_t from, to;
    hashsetRehashingInfo(d, &from, &to);
    kvs->bucket_count += to; /* Started rehashing (Add the new ht size) */
    kvs->overhead_hashtable_rehashing += from;
}

/* Remove hash table from the rehashing list.
 *
 * Updates the bucket count for the given hash table in a DB. It removes
 * the old ht size of the hash table from the total sum of buckets for a DB.  */
void kvstoreHashsetRehashingCompleted(hashset *d) {
    kvstoreHashsetMetadata *metadata = (kvstoreHashsetMetadata *)hashsetMetadata(d);
    kvstore *kvs = metadata->kvs;
    if (metadata->rehashing_node) {
        listDelNode(kvs->rehashing, metadata->rehashing_node);
        metadata->rehashing_node = NULL;
    }

    size_t from, to;
    hashsetRehashingInfo(d, &from, &to);
    kvs->bucket_count -= from; /* Finished rehashing (Remove the old ht size) */
    kvs->overhead_hashtable_rehashing -= from;
}

/* Returns the size of the DB hashset metadata in bytes. */
size_t kvstoreHashsetMetadataSize(void) {
    return sizeof(kvstoreHashsetMetadata);
}

/**********************************/
/*** API **************************/
/**********************************/

/* Create an array of hash tables
 * num_hashsets_bits is the log2 of the amount of hash tables needed (e.g. 0 for 1 hashset,
 * 3 for 8 hashsets, etc.)
 */
kvstore *kvstoreCreate(hashsetType *type, int num_hashsets_bits, int flags) {
    /* We can't support more than 2^16 hashsets because we want to save 48 bits
     * for the hashset cursor, see kvstoreScan */
    assert(num_hashsets_bits <= 16);

    /* The hashsetType of kvstore needs to use the specific callbacks.
     * If there are any changes in the future, it will need to be modified. */
    assert(type->rehashingStarted == kvstoreHashsetRehashingStarted);
    assert(type->rehashingCompleted == kvstoreHashsetRehashingCompleted);
    assert(type->getMetadataSize == kvstoreHashsetMetadataSize);

    kvstore *kvs = zcalloc(sizeof(*kvs));
    kvs->dtype = type;
    kvs->flags = flags;

    kvs->num_hashsets_bits = num_hashsets_bits;
    kvs->num_hashsets = 1 << kvs->num_hashsets_bits;
    kvs->hashsets = zcalloc(sizeof(hashset *) * kvs->num_hashsets);
    if (!(kvs->flags & KVSTORE_ALLOCATE_HASHSETS_ON_DEMAND)) {
        for (int i = 0; i < kvs->num_hashsets; i++) createHashsetIfNeeded(kvs, i);
    }

    kvs->rehashing = listCreate();
    kvs->key_count = 0;
    kvs->non_empty_hashsets = 0;
    kvs->resize_cursor = 0;
    kvs->hashset_size_index = kvs->num_hashsets > 1 ? zcalloc(sizeof(unsigned long long) * (kvs->num_hashsets + 1)) : NULL;
    kvs->bucket_count = 0;
    kvs->overhead_hashtable_rehashing = 0;

    return kvs;
}

void kvstoreEmpty(kvstore *kvs, void(callback)(hashset *)) {
    for (int didx = 0; didx < kvs->num_hashsets; didx++) {
        hashset *d = kvstoreGetHashset(kvs, didx);
        if (!d) continue;
        kvstoreHashsetMetadata *metadata = (kvstoreHashsetMetadata *)hashsetMetadata(d);
        if (metadata->rehashing_node) metadata->rehashing_node = NULL;
        hashsetEmpty(d, callback);
        freeHashsetIfNeeded(kvs, didx);
    }

    listEmpty(kvs->rehashing);

    kvs->key_count = 0;
    kvs->non_empty_hashsets = 0;
    kvs->resize_cursor = 0;
    kvs->bucket_count = 0;
    if (kvs->hashset_size_index) memset(kvs->hashset_size_index, 0, sizeof(unsigned long long) * (kvs->num_hashsets + 1));
    kvs->overhead_hashtable_rehashing = 0;
}

void kvstoreRelease(kvstore *kvs) {
    for (int didx = 0; didx < kvs->num_hashsets; didx++) {
        hashset *d = kvstoreGetHashset(kvs, didx);
        if (!d) continue;
        kvstoreHashsetMetadata *metadata = (kvstoreHashsetMetadata *)hashsetMetadata(d);
        if (metadata->rehashing_node) metadata->rehashing_node = NULL;
        hashsetRelease(d);
    }
    zfree(kvs->hashsets);

    listRelease(kvs->rehashing);
    if (kvs->hashset_size_index) zfree(kvs->hashset_size_index);

    zfree(kvs);
}

unsigned long long int kvstoreSize(kvstore *kvs) {
    if (kvs->num_hashsets != 1) {
        return kvs->key_count;
    } else {
        return kvs->hashsets[0] ? hashsetSize(kvs->hashsets[0]) : 0;
    }
}

/* This method provides the cumulative sum of all the hash table buckets
 * across hash tables in a database. */
unsigned long kvstoreBuckets(kvstore *kvs) {
    if (kvs->num_hashsets != 1) {
        return kvs->bucket_count;
    } else {
        return kvs->hashsets[0] ? hashsetBuckets(kvs->hashsets[0]) : 0;
    }
}

size_t kvstoreMemUsage(kvstore *kvs) {
    size_t mem = sizeof(*kvs);

    size_t HASHSET_FIXED_SIZE = 42; /* dummy; FIXME: Define in hashset.h */
    mem += kvstoreBuckets(kvs) * HASHSET_BUCKET_SIZE;
    mem += kvs->allocated_hashsets * (HASHSET_FIXED_SIZE + kvstoreHashsetMetadataSize());

    /* Values are hashset* shared with kvs->hashsets */
    mem += listLength(kvs->rehashing) * sizeof(listNode);

    if (kvs->hashset_size_index) mem += sizeof(unsigned long long) * (kvs->num_hashsets + 1);

    return mem;
}

/*
 * This method is used to iterate over the elements of the entire kvstore specifically across hashsets.
 * It's a three pronged approach.
 *
 * 1. It uses the provided cursor `cursor` to retrieve the hashset index from it.
 * 2. If the hash table is in a valid state checked through the provided callback `hashsetScanValidFunction`,
 *    it performs a hashsetScan over the appropriate `keyType` hash table of `db`.
 * 3. If the hashset is entirely scanned i.e. the cursor has reached 0, the next non empty hashset is discovered.
 *    The hashset information is embedded into the cursor and returned.
 *
 * To restrict the scan to a single hashset, pass a valid hashset index as
 * 'onlydidx', otherwise pass -1.
 */
unsigned long long kvstoreScan(kvstore *kvs,
                               unsigned long long cursor,
                               int onlydidx,
                               hashsetScanFunction scan_cb,
                               kvstoreScanShouldSkipHashset *skip_cb,
                               void *privdata,
                               int flags) {
    unsigned long long next_cursor = 0;
    /* During hash table traversal, 48 upper bits in the cursor are used for positioning in the HT.
     * Following lower bits are used for the hashset index number, ranging from 0 to 2^num_hashsets_bits-1.
     * Hashset index is always 0 at the start of iteration and can be incremented only if there are
     * multiple hashsets. */
    int didx = getAndClearHashsetIndexFromCursor(kvs, &cursor);
    if (onlydidx >= 0) {
        if (didx < onlydidx) {
            /* Fast-forward to onlydidx. */
            assert(onlydidx < kvs->num_hashsets);
            didx = onlydidx;
            cursor = 0;
        } else if (didx > onlydidx) {
            /* The cursor is already past onlydidx. */
            return 0;
        }
    }

    hashset *d = kvstoreGetHashset(kvs, didx);

    int skip = !d || (skip_cb && skip_cb(d));
    if (!skip) {
        next_cursor = hashsetScan(d, cursor, scan_cb, privdata, flags);
        /* In hashsetScan, scan_cb may delete entries (e.g., in active expire case). */
        freeHashsetIfNeeded(kvs, didx);
    }
    /* scanning done for the current hash table or if the scanning wasn't possible, move to the next hashset index. */
    if (next_cursor == 0 || skip) {
        if (onlydidx >= 0) return 0;
        didx = kvstoreGetNextNonEmptyHashsetIndex(kvs, didx);
    }
    if (didx == -1) {
        return 0;
    }
    addHashsetIndexToCursor(kvs, didx, &next_cursor);
    return next_cursor;
}

/*
 * This functions increases size of kvstore to match desired number.
 * It resizes all individual hash tables, unless skip_cb indicates otherwise.
 *
 * Based on the parameter `try_expand`, appropriate hashset expand API is invoked.
 * if try_expand is set to 1, `hashsetTryExpand` is used else `hashsetExpand`.
 * The return code is either 1 or 0 for both the API(s).
 * 1 response is for successful expansion. However, 0 response signifies failure in allocation in
 * `hashsetTryExpand` call and in case of `hashsetExpand` call it signifies no expansion was performed.
 */
int kvstoreExpand(kvstore *kvs, uint64_t newsize, int try_expand, kvstoreExpandShouldSkipHashsetIndex *skip_cb) {
    for (int i = 0; i < kvs->num_hashsets; i++) {
        hashset *d = kvstoreGetHashset(kvs, i);
        if (!d || (skip_cb && skip_cb(i))) continue;
        if (try_expand) {
            if (!hashsetTryExpand(d, newsize)) return 0;
        } else {
            hashsetExpand(d, newsize);
        }
    }

    return 1;
}

/* Returns fair random hashset index, probability of each hashset being returned is proportional to the number of elements
 * that hash table holds. This function guarantees that it returns a hashset-index of a non-empty hashset, unless the entire
 * kvstore is empty. Time complexity of this function is O(log(kvs->num_hashsets)). */
int kvstoreGetFairRandomHashsetIndex(kvstore *kvs) {
    unsigned long target = kvstoreSize(kvs) ? (random() % kvstoreSize(kvs)) + 1 : 0;
    return kvstoreFindHashsetIndexByKeyIndex(kvs, target);
}

void kvstoreGetStats(kvstore *kvs, char *buf, size_t bufsize, int full) {
    buf[0] = '\0';

    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;
    hashsetStats *mainHtStats = NULL;
    hashsetStats *rehashHtStats = NULL;
    hashset *d;
    kvstoreIterator *kvs_it = kvstoreIteratorInit(kvs);
    while ((d = kvstoreIteratorNextHashset(kvs_it))) {
        hashsetStats *stats = hashsetGetStatsHt(d, 0, full);
        if (!mainHtStats) {
            mainHtStats = stats;
        } else {
            hashsetCombineStats(stats, mainHtStats);
            hashsetFreeStats(stats);
        }
        if (hashsetIsRehashing(d)) {
            stats = hashsetGetStatsHt(d, 1, full);
            if (!rehashHtStats) {
                rehashHtStats = stats;
            } else {
                hashsetCombineStats(stats, rehashHtStats);
                hashsetFreeStats(stats);
            }
        }
    }
    kvstoreIteratorRelease(kvs_it);

    if (mainHtStats && bufsize > 0) {
        l = hashsetGetStatsMsg(buf, bufsize, mainHtStats, full);
        hashsetFreeStats(mainHtStats);
        buf += l;
        bufsize -= l;
    }

    if (rehashHtStats && bufsize > 0) {
        l = hashsetGetStatsMsg(buf, bufsize, rehashHtStats, full);
        hashsetFreeStats(rehashHtStats);
        buf += l;
        bufsize -= l;
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize - 1] = '\0';
}

/* Finds a hashset containing target element in a key space ordered by hashset index.
 * Consider this example. Hash Tables are represented by brackets and keys by dots:
 *  #0   #1   #2     #3    #4
 * [..][....][...][.......][.]
 *                    ^
 *                 target
 *
 * In this case hashset #3 contains key that we are trying to find.
 *
 * The return value is 0 based hashset-index, and the range of the target is [1..kvstoreSize], kvstoreSize inclusive.
 *
 * To find the hashset, we start with the root node of the binary index tree and search through its children
 * from the highest index (2^num_hashsets_bits in our case) to the lowest index. At each node, we check if the target
 * value is greater than the node's value. If it is, we remove the node's value from the target and recursively
 * search for the new target using the current node as the parent.
 * Time complexity of this function is O(log(kvs->num_hashsets))
 */
int kvstoreFindHashsetIndexByKeyIndex(kvstore *kvs, unsigned long target) {
    if (kvs->num_hashsets == 1 || kvstoreSize(kvs) == 0) return 0;
    assert(target <= kvstoreSize(kvs));

    int result = 0, bit_mask = 1 << kvs->num_hashsets_bits;
    for (int i = bit_mask; i != 0; i >>= 1) {
        int current = result + i;
        /* When the target index is greater than 'current' node value the we will update
         * the target and search in the 'current' node tree. */
        if (target > kvs->hashset_size_index[current]) {
            target -= kvs->hashset_size_index[current];
            result = current;
        }
    }
    /* Adjust the result to get the correct hashset:
     * 1. result += 1;
     *    After the calculations, the index of target in hashset_size_index should be the next one,
     *    so we should add 1.
     * 2. result -= 1;
     *    Unlike BIT(hashset_size_index is 1-based), hashset indices are 0-based, so we need to subtract 1.
     * As the addition and subtraction cancel each other out, we can simply return the result. */
    return result;
}

/* Wrapper for kvstoreFindHashsetIndexByKeyIndex to get the first non-empty hashset index in the kvstore. */
int kvstoreGetFirstNonEmptyHashsetIndex(kvstore *kvs) {
    return kvstoreFindHashsetIndexByKeyIndex(kvs, 1);
}

/* Returns next non-empty hashset index strictly after given one, or -1 if provided didx is the last one. */
int kvstoreGetNextNonEmptyHashsetIndex(kvstore *kvs, int didx) {
    if (kvs->num_hashsets == 1) {
        assert(didx == 0);
        return -1;
    }
    unsigned long long next_key = cumulativeKeyCountRead(kvs, didx) + 1;
    return next_key <= kvstoreSize(kvs) ? kvstoreFindHashsetIndexByKeyIndex(kvs, next_key) : -1;
}

int kvstoreNumNonEmptyHashsets(kvstore *kvs) {
    return kvs->non_empty_hashsets;
}

int kvstoreNumAllocatedHashsets(kvstore *kvs) {
    return kvs->allocated_hashsets;
}

int kvstoreNumHashsets(kvstore *kvs) {
    return kvs->num_hashsets;
}

/* Returns kvstore iterator that can be used to iterate through sub-hash tables.
 *
 * The caller should free the resulting kvs_it with kvstoreIteratorRelease. */
kvstoreIterator *kvstoreIteratorInit(kvstore *kvs) {
    kvstoreIterator *kvs_it = zmalloc(sizeof(*kvs_it));
    kvs_it->kvs = kvs;
    kvs_it->didx = -1;
    kvs_it->next_didx = kvstoreGetFirstNonEmptyHashsetIndex(kvs_it->kvs); /* Finds first non-empty hashset index. */
    hashsetInitSafeIterator(&kvs_it->di, NULL);
    return kvs_it;
}

/* Free the kvs_it returned by kvstoreIteratorInit. */
void kvstoreIteratorRelease(kvstoreIterator *kvs_it) {
    hashsetIterator *iter = &kvs_it->di;
    hashsetResetIterator(iter);
    /* In the safe iterator context, we may delete entries. */
    freeHashsetIfNeeded(kvs_it->kvs, kvs_it->didx);
    zfree(kvs_it);
}

/* Returns next hash table from the iterator, or NULL if iteration is complete. */
static hashset *kvstoreIteratorNextHashset(kvstoreIterator *kvs_it) {
    if (kvs_it->next_didx == -1) return NULL;

    /* The hashset may be deleted during the iteration process, so here need to check for NULL. */
    if (kvs_it->didx != -1 && kvstoreGetHashset(kvs_it->kvs, kvs_it->didx)) {
        /* Before we move to the next hashset, reset the iter of the previous hashset. */
        hashsetIterator *iter = &kvs_it->di;
        hashsetResetIterator(iter);
        /* In the safe iterator context, we may delete entries. */
        freeHashsetIfNeeded(kvs_it->kvs, kvs_it->didx);
    }

    kvs_it->didx = kvs_it->next_didx;
    kvs_it->next_didx = kvstoreGetNextNonEmptyHashsetIndex(kvs_it->kvs, kvs_it->didx);
    return kvs_it->kvs->hashsets[kvs_it->didx];
}

int kvstoreIteratorGetCurrentHashsetIndex(kvstoreIterator *kvs_it) {
    assert(kvs_it->didx >= 0 && kvs_it->didx < kvs_it->kvs->num_hashsets);
    return kvs_it->didx;
}

/* Fetches the next element and returns 1. Returns 0 if there are no more elements. */
int kvstoreIteratorNext(kvstoreIterator *kvs_it, void **next) {
    if (kvs_it->di.hashset && hashsetNext(&kvs_it->di, next)) {
        return 1;
    } else {
        /* No current hashset or reached the end of the hash table. */
        hashset *d = kvstoreIteratorNextHashset(kvs_it);
        if (!d) return 0;
        hashsetInitSafeIterator(&kvs_it->di, d);
        return hashsetNext(&kvs_it->di, next);
    }
}

/* This method traverses through kvstore hash tables and triggers a resize.
 * It first tries to shrink if needed, and if it isn't, it tries to expand. */
void kvstoreTryResizeHashsets(kvstore *kvs, int limit) {
    if (limit > kvs->num_hashsets) limit = kvs->num_hashsets;

    for (int i = 0; i < limit; i++) {
        int didx = kvs->resize_cursor;
        hashset *d = kvstoreGetHashset(kvs, didx);
        if (d && !hashsetShrinkIfNeeded(d)) {
            hashsetExpandIfNeeded(d);
        }
        kvs->resize_cursor = (didx + 1) % kvs->num_hashsets;
    }
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use threshold_us
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns the amount of microsecs spent if some rehashing was
 * performed, otherwise 0 is returned. */
uint64_t kvstoreIncrementallyRehash(kvstore *kvs, uint64_t threshold_us) {
    if (listLength(kvs->rehashing) == 0) return 0;

    /* Our goal is to rehash as many hash tables as we can before reaching threshold_us,
     * after each hash table completes rehashing, it removes itself from the list. */
    listNode *node;
    monotime timer;
    uint64_t elapsed_us = 0;
    elapsedStart(&timer);
    while ((node = listFirst(kvs->rehashing))) {
        hashsetRehashMicroseconds(listNodeValue(node), threshold_us - elapsed_us);

        elapsed_us = elapsedUs(timer);
        if (elapsed_us >= threshold_us) {
            break; /* Reached the time limit. */
        }
    }
    return elapsed_us;
}

/* Size in bytes of hash tables used by the hashsets. */
size_t kvstoreOverheadHashtableLut(kvstore *kvs) {
    return kvs->bucket_count * HASHSET_BUCKET_SIZE;
}

size_t kvstoreOverheadHashtableRehashing(kvstore *kvs) {
    return kvs->overhead_hashtable_rehashing * HASHSET_BUCKET_SIZE;
}

unsigned long kvstoreHashsetRehashingCount(kvstore *kvs) {
    return listLength(kvs->rehashing);
}

unsigned long kvstoreHashsetSize(kvstore *kvs, int didx) {
    hashset *d = kvstoreGetHashset(kvs, didx);
    if (!d) return 0;
    return hashsetSize(d);
}

kvstoreHashsetIterator *kvstoreGetHashsetIterator(kvstore *kvs, int didx) {
    kvstoreHashsetIterator *kvs_di = zmalloc(sizeof(*kvs_di));
    kvs_di->kvs = kvs;
    kvs_di->didx = didx;
    hashsetInitIterator(&kvs_di->di, kvstoreGetHashset(kvs, didx));
    return kvs_di;
}

kvstoreHashsetIterator *kvstoreGetHashsetSafeIterator(kvstore *kvs, int didx) {
    kvstoreHashsetIterator *kvs_di = zmalloc(sizeof(*kvs_di));
    kvs_di->kvs = kvs;
    kvs_di->didx = didx;
    hashsetInitSafeIterator(&kvs_di->di, kvstoreGetHashset(kvs, didx));
    return kvs_di;
}

/* Free the kvs_di returned by kvstoreGetHashsetIterator and kvstoreGetHashsetSafeIterator. */
void kvstoreReleaseHashsetIterator(kvstoreHashsetIterator *kvs_di) {
    /* The hashset may be deleted during the iteration process, so here need to check for NULL. */
    if (kvstoreGetHashset(kvs_di->kvs, kvs_di->didx)) {
        hashsetResetIterator(&kvs_di->di);
        /* In the safe iterator context, we may delete entries. */
        freeHashsetIfNeeded(kvs_di->kvs, kvs_di->didx);
    }

    zfree(kvs_di);
}

/* Get the next element of the hashset through kvstoreHashsetIterator and hashsetNext. */
int kvstoreHashsetIteratorNext(kvstoreHashsetIterator *kvs_di, void **next) {
    /* The hashset may be deleted during the iteration process, so here need to check for NULL. */
    hashset *t = kvstoreGetHashset(kvs_di->kvs, kvs_di->didx);
    if (!t) return 0;
    return hashsetNext(&kvs_di->di, next);
}

int kvstoreHashsetRandomElement(kvstore *kvs, int didx, void **element) {
    hashset *d = kvstoreGetHashset(kvs, didx);
    if (!d) return 0;
    return hashsetRandomElement(d, element);
}

int kvstoreHashsetFairRandomElement(kvstore *kvs, int didx, void **element) {
    hashset *d = kvstoreGetHashset(kvs, didx);
    if (!d) return 0;
    return hashsetFairRandomElement(d, element);
}

unsigned int kvstoreHashsetSampleElements(kvstore *kvs, int didx, void **dst, unsigned int count) {
    hashset *d = kvstoreGetHashset(kvs, didx);
    if (!d) return 0;
    return hashsetSampleElements(d, dst, count);
}

int kvstoreHashsetExpand(kvstore *kvs, int didx, unsigned long size) {
    hashset *d = kvstoreGetHashset(kvs, didx);
    if (!d) return 0;
    return hashsetExpand(d, size);
}

unsigned long kvstoreHashsetScan(kvstore *kvs,
                                 int didx,
                                 unsigned long v,
                                 hashsetScanFunction fn,
                                 void *privdata,
                                 int flags) {
    hashset *d = kvstoreGetHashset(kvs, didx);
    if (!d) return 0;
    return hashsetScan(d, v, fn, privdata, flags);
}

/* This function doesn't defrag the data (keys and values) within hashset. It
 * only reallocates the memory used by the hashset structure itself using the
 * provided allocation function. This feature was added for the active defrag
 * feature.
 *
 * The provided defragfn callback should either return NULL (if reallocation is
 * not necessary) or reallocate the memory like realloc() would do. */
void kvstoreHashsetDefragInternals(kvstore *kvs, void *(*defragfn)(void *)) {
    for (int didx = 0; didx < kvs->num_hashsets; didx++) {
        hashset **ref = kvstoreGetHashsetRef(kvs, didx), *new;
        if (!*ref) continue;
        new = hashsetDefragInternals(*ref, defragfn);
        if (new) {
            *ref = new;
            kvstoreHashsetMetadata *metadata = hashsetMetadata(new);
            if (metadata->rehashing_node) metadata->rehashing_node->value = new;
        }
    }
}

uint64_t kvstoreGetHash(kvstore *kvs, const void *key) {
    return kvs->dtype->hashFunction(key);
}

/* void *kvstoreHashsetFetchElement(kvstore *kvs, int didx, const void *key) { */
/*     hashset *t = kvstoreGetHashset(kvs, didx); */
/*     if (!t) return NULL; */
/*     return hashsetFetchElement(t, key); */
/* } */

int kvstoreHashsetFind(kvstore *kvs, int didx, void *key, void **found) {
    hashset *t = kvstoreGetHashset(kvs, didx);
    if (!t) return 0;
    return hashsetFind(t, key, found);
}

void **kvstoreHashsetFindRef(kvstore *kvs, int didx, const void *key) {
    hashset *t = kvstoreGetHashset(kvs, didx);
    if (!t) return NULL;
    return hashsetFindRef(t, key);
}

/* was AddRaw */
int kvstoreHashsetAddOrFind(kvstore *kvs, int didx, void *key, void **existing) {
    hashset *d = createHashsetIfNeeded(kvs, didx);
    int ret = hashsetAddOrFind(d, key, existing);
    if (ret) cumulativeKeyCountAdd(kvs, didx, 1);
    return ret;
}

int kvstoreHashsetAdd(kvstore *kvs, int didx, void *element) {
    hashset *d = createHashsetIfNeeded(kvs, didx);
    int ret = hashsetAdd(d, element);
    if (ret) cumulativeKeyCountAdd(kvs, didx, 1);
    return ret;
}

void *kvstoreHashsetFindPositionForInsert(kvstore *kvs, int didx, void *key, void **existing) {
    hashset *t = createHashsetIfNeeded(kvs, didx);
    return hashsetFindPositionForInsert(t, key, existing);
}

/* Must be used together with kvstoreHashsetFindPositionForInsert, with returned
 * position and with the same didx. */
void kvstoreHashsetInsertAtPosition(kvstore *kvs, int didx, void *elem, void *position) {
    hashset *t = kvstoreGetHashset(kvs, didx);
    hashsetInsertAtPosition(t, elem, position);
    cumulativeKeyCountAdd(kvs, didx, 1);
}

void **kvstoreHashsetTwoPhasePopFindRef(kvstore *kvs, int didx, const void *key, void **position) {
    hashset *s = kvstoreGetHashset(kvs, didx);
    if (!s) return NULL;
    return hashsetTwoPhasePopFindRef(s, key, position);
}

void kvstoreHashsetTwoPhasePopDelete(kvstore *kvs, int didx, void *position) {
    hashset *d = kvstoreGetHashset(kvs, didx);
    hashsetTwoPhasePopDelete(d, position);
    cumulativeKeyCountAdd(kvs, didx, -1);
    freeHashsetIfNeeded(kvs, didx);
}

int kvstoreHashsetPop(kvstore *kvs, int didx, const void *key, void **popped) {
    hashset *t = kvstoreGetHashset(kvs, didx);
    if (!t) return 0;
    int ret = hashsetPop(t, key, popped);
    if (ret) {
        cumulativeKeyCountAdd(kvs, didx, -1);
        freeHashsetIfNeeded(kvs, didx);
    }
    return ret;
}

int kvstoreHashsetDelete(kvstore *kvs, int didx, const void *key) {
    hashset *t = kvstoreGetHashset(kvs, didx);
    if (!t) return 0;
    int ret = hashsetDelete(t, key);
    if (ret) {
        cumulativeKeyCountAdd(kvs, didx, -1);
        freeHashsetIfNeeded(kvs, didx);
    }
    return ret;
}
