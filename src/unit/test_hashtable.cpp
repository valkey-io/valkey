/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "fmacros.h"
#include "hashtable.h"
#include "monotonic.h"
#include "mt19937-64.h"
#include "zmalloc.h"

extern bool accurate;
extern bool large_memory;
extern char *seed;
/* From util.c: getRandomBytes to seed hash function. */
void getRandomBytes(unsigned char *p, size_t len);
}

/* Global variable to test the memory tracking callback. */
static size_t mem_usage;

/* Init hash function salt and seed random generator. */
static void randomSeed(void) {
    unsigned long long seed;
    getRandomBytes((unsigned char *)&seed, sizeof(seed));
    init_genrand64(seed);
    srandom((unsigned)seed);
    uint8_t hashseed[16];
    getRandomBytes(hashseed, sizeof(hashseed));
    hashtableSetHashFunctionSeed(hashseed);
}

/* An entry holding a string key and a string value in one allocation. */
typedef struct keyval {
    unsigned int keysize; /* Sizes, including null-terminator */
    unsigned int valsize;
    char data[1]; /* key and value - C++ doesn't support flexible array members */
} keyval;

static keyval *create_keyval(const char *key, const char *val) {
    size_t keysize = strlen(key) + 1;
    size_t valsize = strlen(val) + 1;
    keyval *e = (keyval *)malloc(sizeof(keyval) + keysize + valsize);
    e->keysize = keysize;
    e->valsize = valsize;
    memcpy(e->data, key, keysize);
    memcpy(e->data + keysize, val, valsize);
    return e;
}

static const void *getkey(const void *entry) {
    const keyval *e = (const keyval *)entry;
    return e->data;
}

static const void *getval(const void *entry) {
    const keyval *e = (const keyval *)entry;
    return e->data + e->keysize;
}

static uint64_t hashfunc(const void *key) {
    return hashtableGenHashFunction((const char *)key, strlen((const char *)key));
}

static int keycmp(const void *key1, const void *key2) {
    return strcmp((const char *)key1, (const char *)key2);
}

static void freekeyval(void *keyval) {
    free(keyval);
}

static void trackmemusage(hashtable *ht, ssize_t delta) {
    UNUSED(ht);
    mem_usage += delta;
}

/* Hashtable type used for some of the tests. */
static hashtableType keyval_type;

/* Callback for testing hashtableEmpty(). */
static long empty_callback_call_counter;

extern "C" {
void emptyCallback(hashtable *ht) {
    UNUSED(ht);
    empty_callback_call_counter++;
}

/* Prototypes for debugging */
void hashtableDump(hashtable *ht);
void hashtableHistogram(hashtable *ht);
int hashtableLongestBucketChain(hashtable *ht);
size_t nextCursor(size_t v, size_t mask);
}

class HashtableTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        /* Initialize monotonic clock once for all tests */
        monotonicInit();

        memset(&keyval_type, 0, sizeof(keyval_type));
        keyval_type.entryGetKey = getkey;
        keyval_type.hashFunction = hashfunc;
        keyval_type.keyCompare = keycmp;
        keyval_type.entryDestructor = freekeyval;
        keyval_type.trackMemUsage = trackmemusage;
    }

    void SetUp() override {
        mem_usage = 0;
        /* Ensure resize policy is reset to default */
        hashtableSetResizePolicy(HASHTABLE_RESIZE_ALLOW);
    }

    void TearDown() override {
        /* Reset resize policy to default after each test */
        hashtableSetResizePolicy(HASHTABLE_RESIZE_ALLOW);
        /* Ensure mem_usage is back to 0 */
        ASSERT_EQ(mem_usage, 0u);
    }
};

TEST_F(HashtableTest, cursor) {
    ASSERT_EQ(nextCursor(0x0000, 0xffff), 0x8000u);
    ASSERT_EQ(nextCursor(0x8000, 0xffff), 0x4000u);
    ASSERT_EQ(nextCursor(0x4001, 0xffff), 0xc001u);
    ASSERT_EQ(nextCursor(0xffff, 0xffff), 0x0000u);
}

TEST_F(HashtableTest, set_hash_function_seed) {
    randomSeed();
}

static void add_find_delete_test_helper() {
    int count = accurate ? 1000000 : 200;
    ASSERT_EQ(mem_usage, 0u);
    hashtable *ht = hashtableCreate(&keyval_type);
    int j;

    /* Add */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e = create_keyval(key, val);
        ASSERT_TRUE(hashtableAdd(ht, e));
    }
    ASSERT_EQ(hashtableMemUsage(ht), mem_usage);

    if (count < 1000) {
        hashtableHistogram(ht);
        printf("Mem usage: %zu\n", hashtableMemUsage(ht));
    }

    /* Find */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        void *found;
        ASSERT_TRUE(hashtableFind(ht, key, &found));
        keyval *e = (keyval *)found;
        ASSERT_EQ(strcmp((const char *)getval(e), val), 0);
    }

    /* Delete half of them */
    for (j = 0; j < count / 2; j++) {
        char key[32];
        snprintf(key, sizeof(key), "%d", j);
        if (j % 3 == 0) {
            /* Test hashtablePop */
            char val[32];
            snprintf(val, sizeof(val), "%d", count - j + 42);
            void *popped;
            ASSERT_TRUE(hashtablePop(ht, key, &popped));
            keyval *e = (keyval *)popped;
            ASSERT_EQ(strcmp((const char *)getval(e), val), 0);
            free(e);
        } else {
            ASSERT_TRUE(hashtableDelete(ht, key));
        }
    }
    ASSERT_EQ(hashtableMemUsage(ht), mem_usage);

    /* Empty, i.e. delete remaining entries, with progress callback. */
    empty_callback_call_counter = 0;
    hashtableEmpty(ht, emptyCallback);
    ASSERT_GT(empty_callback_call_counter, 0);

    /* Release memory */
    hashtableRelease(ht);
    ASSERT_EQ(mem_usage, 0u);
}

TEST_F(HashtableTest, add_find_delete) {
    size_t used_memory_before = zmalloc_used_memory();
    add_find_delete_test_helper();
    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

TEST_F(HashtableTest, add_find_delete_avoid_resize) {
    size_t used_memory_before = zmalloc_used_memory();
    hashtableSetResizePolicy(HASHTABLE_RESIZE_AVOID);
    add_find_delete_test_helper();
    hashtableSetResizePolicy(HASHTABLE_RESIZE_ALLOW);
    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

TEST_F(HashtableTest, instant_rehashing) {
    long count = 200;

    /* A set of longs, i.e. pointer-sized values. */
    hashtableType type = {};
    type.instant_rehashing = 1;
    hashtable *ht = hashtableCreate(&type);
    long j;

    /* Populate and check that rehashing is never ongoing. */
    for (j = 0; j < count; j++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)j));
        ASSERT_FALSE(hashtableIsRehashing(ht));
    }

    /* Delete and check that rehashing is never ongoing. */
    for (j = 0; j < count; j++) {
        ASSERT_TRUE(hashtableDelete(ht, (void *)j));
        ASSERT_FALSE(hashtableIsRehashing(ht));
    }

    hashtableRelease(ht);
}

TEST_F(HashtableTest, empty_buckets_rehashing) {
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);
    hashtableSetCanAbortShrink(false);
    long j;
    long keep = 0;
    size_t keep_bucket = 0;

    /* Populate and make sure there is no rehashing ongoing. */
    for (j = 0; j < 1000; j++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)j));
    }
    while (hashtableIsRehashing(ht)) {
        ASSERT_FALSE(hashtableAdd(ht, (void *)0));
    }

    /* Keep the entry from the highest bucket index so shrink rehashing doesn't
     * complete too early with randomized hash seeds and start a second resize. */
    size_t mask = hashtableBuckets(ht) - 1;
    for (j = 0; j < 1000; j++) {
        const void *key = (void *)j;
        uint64_t hash = hashtableGenHashFunction((const char *)&key, sizeof(key));
        size_t bucket_idx = hash & mask;
        if (bucket_idx > keep_bucket) {
            keep_bucket = bucket_idx;
            keep = j;
        }
    }

    /* Delete all elements except one so there are a lot of empty buckets. */
    hashtablePauseAutoShrink(ht);
    for (j = 0; j < 1000; j++) {
        if (j == keep) continue;
        ASSERT_TRUE(hashtableDelete(ht, (void *)j));
    }
    hashtableResumeAutoShrink(ht);
    ASSERT_EQ(hashtableSize(ht), 1u);
    ASSERT_EQ(hashtableGetRehashingIndex(ht), 0);

    /* Add elements to trigger rehashing, a rehash step will rehash a maximum of 10 buckets. */
    for (j = 0; j < 10; j++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)(1000 + j)));
    }
    ASSERT_EQ(hashtableSize(ht), 11u);
    /* Check that at least 90 buckets are rehashed or that rehashing is completed. */
    ASSERT_TRUE(hashtableGetRehashingIndex(ht) >= 90 || hashtableGetRehashingIndex(ht) == -1);

    hashtableRelease(ht);
}

TEST_F(HashtableTest, shrink_rehashing_abort) {
    hashtableType type = {0};
    hashtable *ht = hashtableCreate(&type);
    hashtableSetCanAbortShrink(true);
    long j;
    long keep = 0;
    size_t keep_bucket = 0;

    /* Populate and make sure there is no rehashing ongoing. */
    for (j = 0; j < 20000; j++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)j));
    }
    while (hashtableIsRehashing(ht)) {
        ASSERT_FALSE(hashtableAdd(ht, (void *)0));
    }

    /* Keep the entry from the highest bucket index so shrink rehashing doesn't
     * complete too early with randomized hash seeds and start a second resize. */
    size_t mask = hashtableBuckets(ht) - 1;
    for (j = 0; j < 20000; j++) {
        const void *key = (void *)j;
        uint64_t hash = hashtableGenHashFunction((const char *)&key, sizeof(key));
        size_t bucket_idx = hash & mask;
        if (bucket_idx > keep_bucket) {
            keep_bucket = bucket_idx;
            keep = j;
        }
    }

    /* Delete all elements except one so there are a lot of empty buckets. */
    hashtablePauseAutoShrink(ht);
    for (j = 0; j < 20000; j++) {
        if (j == keep) continue;
        ASSERT_TRUE(hashtableDelete(ht, (void *)j));
    }
    hashtableResumeAutoShrink(ht);
    ASSERT_EQ(hashtableSize(ht), 1u);
    ASSERT_EQ(hashtableGetRehashingIndex(ht), 0);

    /* Add elements to reach MAX_FILL_PERCENT_HARD will trigger the shrink rehashing to abort. */
    long add = hashtableEntriesPerBucket() * 5;
    for (j = 0; j < add; j++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)(20000 + j)));
    }

    /* Check that we restart the rehashing. */
    ASSERT_EQ(hashtableSize(ht), (size_t)(add + 1));
    ASSERT_EQ(hashtableGetRehashingIndex(ht), 0);

    /* Fuzzy test around normal add and delete to make sure we are ok. */
    for (j = 0; j < 20000; j++) {
        hashtableAdd(ht, (void *)j);
        ASSERT_TRUE(hashtableDelete(ht, (void *)j));
    }

    hashtableRelease(ht);
}

TEST_F(HashtableTest, bucket_chain_length) {
    unsigned long count = 1000000;

    /* A set of longs, i.e. pointer-sized integer values. */
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);
    unsigned long j;
    for (j = 0; j < count; j++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)j));
    }
    /* If it's rehashing, add a few more until rehashing is complete.
     * We also make sure that we won't resize during the rehashing. */
    while (hashtableIsRehashing(ht)) {
        ASSERT_FALSE(hashtableExpand(ht, count * 2));
        j++;
        ASSERT_TRUE(hashtableAdd(ht, (void *)j));
    }
    ASSERT_LT(j, count * 2);
    int max_chainlen_not_rehashing = hashtableLongestBucketChain(ht);
    ASSERT_LT(max_chainlen_not_rehashing, 10);

    /* Add more until rehashing starts again. */
    while (!hashtableIsRehashing(ht)) {
        j++;
        ASSERT_TRUE(hashtableAdd(ht, (void *)j));
    }
    ASSERT_LT(j, count * 2);
    int max_chainlen_rehashing = hashtableLongestBucketChain(ht);
    ASSERT_LT(max_chainlen_rehashing, 10);

    hashtableRelease(ht);
}

TEST_F(HashtableTest, two_phase_insert_and_pop) {
    int count = accurate ? 1000000 : 200;
    hashtable *ht = hashtableCreate(&keyval_type);
    int j;

    /* hashtableFindPositionForInsert + hashtableInsertAtPosition */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        hashtablePosition position;
        bool ret = hashtableFindPositionForInsert(ht, key, &position, nullptr);
        ASSERT_TRUE(ret);
        keyval *e = create_keyval(key, val);
        hashtableInsertAtPosition(ht, e, &position);
    }

    if (count < 1000) {
        hashtableHistogram(ht);
    }

    /* Check that all entries were inserted. */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        void *found;
        ASSERT_TRUE(hashtableFind(ht, key, &found));
        keyval *e = (keyval *)found;
        ASSERT_EQ(strcmp((const char *)getval(e), val), 0);
    }

    /* Test two-phase pop. */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        hashtablePosition position;
        size_t size_before_find = hashtableSize(ht);
        void **ref = hashtableTwoPhasePopFindRef(ht, key, &position);
        ASSERT_NE(ref, nullptr);
        keyval *e = *(keyval **)ref;
        ASSERT_EQ(strcmp((const char *)getval(e), val), 0);
        ASSERT_EQ(hashtableSize(ht), size_before_find);
        hashtableTwoPhasePopDelete(ht, &position);
        ASSERT_EQ(hashtableSize(ht), size_before_find - 1);
        free(e);
    }
    ASSERT_EQ(hashtableSize(ht), 0u);

    hashtableRelease(ht);
}

TEST_F(HashtableTest, replace_reallocated_entry) {
    size_t used_memory_before = zmalloc_used_memory();

    int count = 100, j;
    hashtable *ht = hashtableCreate(&keyval_type);

    /* Add */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e = create_keyval(key, val);
        ASSERT_TRUE(hashtableAdd(ht, e));
    }

    /* Find and replace */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        void *found;
        ASSERT_TRUE(hashtableFind(ht, key, &found));
        keyval *old = (keyval *)found;
        ASSERT_EQ(strcmp((const char *)getkey(old), key), 0);
        ASSERT_EQ(strcmp((const char *)getval(old), val), 0);
        snprintf(val, sizeof(val), "%d", j + 1234);
        keyval *new_entry = create_keyval(key, val);
        /* If we free 'old' before the call to hashtableReplaceReallocatedEntry,
         * we get a use-after-free warning, so instead we just overwrite it with
         * junk. The purpose is to verify that the function doesn't use the
         * memory it points to. */
        memset(old->data, 'x', old->keysize + old->valsize);
        ASSERT_TRUE(hashtableReplaceReallocatedEntry(ht, old, new_entry));
        free(old);
    }

    /* Check */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", j + 1234);
        void *found;
        ASSERT_TRUE(hashtableFind(ht, key, &found));
        keyval *e = (keyval *)found;
        ASSERT_EQ(strcmp((const char *)getval(e), val), 0);
    }

    hashtableRelease(ht);
    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

TEST_F(HashtableTest, incremental_find) {
    size_t count = 2000000;
    uint8_t *element_array = (uint8_t *)malloc(count);
    memset(element_array, 0, count);

    /* A set of uint8_t pointers */
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Populate */
    for (size_t j = 0; j < count; j++) {
        ASSERT_TRUE(hashtableAdd(ht, element_array + j));
    }

    monotime timer;

    /* Compare to looking up one by one. */
    elapsedStart(&timer);
    for (size_t i = 0; i < count; i++) {
        uint8_t *key = &element_array[i];
        void *found;
        ASSERT_EQ(hashtableFind(ht, key, &found), 1);
        ASSERT_EQ(found, key);
    }
    uint64_t us2 = elapsedUs(timer);
    printf("Lookup %zu elements one by one took %lu microseconds.\n",
           count, (unsigned long)us2);

    /* Lookup elements in batches. */
    for (size_t batch_size = 1; batch_size <= 64; batch_size *= 2) {
        elapsedStart(&timer);
        for (size_t batch = 0; batch < count / batch_size; batch++) {
            /* Init batches. */
            hashtableIncrementalFindState *states = (hashtableIncrementalFindState *)malloc(sizeof(hashtableIncrementalFindState) * batch_size);
            for (size_t i = 0; i < batch_size; i++) {
                void *key = &element_array[batch * batch_size + i];
                hashtableIncrementalFindInit(&states[i], ht, key);
            }
            /* Work on batches in round-robin order until all are done. */
            size_t num_left;
            do {
                num_left = batch_size;
                for (size_t i = 0; i < batch_size; i++) {
                    if (!hashtableIncrementalFindStep(&states[i])) {
                        num_left--;
                    }
                }
            } while (num_left > 0);

            /* Fetch results. */
            for (size_t i = 0; i < batch_size; i++) {
                void *found;
                ASSERT_EQ(hashtableIncrementalFindGetResult(&states[i], &found), 1);
                ASSERT_EQ(found, &element_array[batch * batch_size + i]);
            }
            free(states);
        }
        uint64_t us1 = elapsedUs(timer);
        printf("Lookup %zu elements in batches of %zu took %lu microseconds.\n",
               count, batch_size, (unsigned long)us1);
    }

    hashtableRelease(ht);
    free(element_array);
}

/* Helper types and functions for scan tests */
typedef struct {
    long count;
    uint8_t entry_seen[1]; /* Flexible array member workaround for C++ */
} scandata;

static void scanfn(void *privdata, void *entry) {
    scandata *data = (scandata *)privdata;
    unsigned long j = (unsigned long)entry;
    data->entry_seen[j]++;
    data->count++;
}

TEST_F(HashtableTest, scan) {
    long num_entries = large_memory ? 1000000 : 200000;
    int num_rounds = accurate ? 20 : 5;

    /* A set of longs, i.e. pointer-sized values. */
    hashtableType type = {};
    long j;

    for (int round = 0; round < num_rounds; round++) {
        /* First round count = num_entries, then some more. */
        long count = num_entries * (1 + 2 * (double)round / num_rounds);

        /* Seed, to make sure each round is different. */
        randomSeed();

        /* Populate */
        hashtable *ht = hashtableCreate(&type);
        for (j = 0; j < count; j++) {
            ASSERT_TRUE(hashtableAdd(ht, (void *)j));
        }

        /* Scan */
        scandata *data = (scandata *)calloc(1, sizeof(scandata) + count);
        long max_entries_per_cycle = 0;
        unsigned num_cycles = 0;
        long scanned_count = 0;
        size_t cursor = 0;
        do {
            data->count = 0;
            cursor = hashtableScan(ht, cursor, scanfn, data);
            if (data->count > max_entries_per_cycle) {
                max_entries_per_cycle = data->count;
            }
            scanned_count += data->count;
            data->count = 0;
            num_cycles++;
        } while (cursor != 0);

        /* Verify that every entry was returned exactly once. */
        ASSERT_EQ(scanned_count, count);
        for (j = 0; j < count; j++) {
            ASSERT_GE(data->entry_seen[j], 1u);
            ASSERT_LE(data->entry_seen[j], 2u);
        }

        /* Print some information for curious readers. */
        printf("Scanned %ld; max emitted per call: %ld; avg emitted per call: %.2lf\n",
               count, max_entries_per_cycle, (double)count / num_cycles);

        /* Cleanup */
        hashtableRelease(ht);
        free(data);
    }
}

/* Helper types for mock hash entry tests */
typedef struct {
    uint64_t value;
    uint64_t hash;
} mock_hash_entry;

static mock_hash_entry *mock_hash_entry_create(uint64_t value, uint64_t hash) {
    mock_hash_entry *entry = (mock_hash_entry *)malloc(sizeof(mock_hash_entry));
    entry->value = value;
    entry->hash = hash;
    return entry;
}

static uint64_t mock_hash_entry_get_hash(const void *entry) {
    if (entry == nullptr) return 0UL;
    const mock_hash_entry *mock = (const mock_hash_entry *)entry;
    return (mock->hash != 0) ? mock->hash : mock->value;
}

TEST_F(HashtableTest, iterator) {
    size_t count = 2000000;
    uint8_t *entry_array = (uint8_t *)malloc(count);
    memset(entry_array, 0, count);

    /* A set of uint8_t pointers */
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Populate */
    for (size_t j = 0; j < count; j++) {
        ASSERT_TRUE(hashtableAdd(ht, entry_array + j));
    }

    /* Iterate */
    size_t num_returned = 0;
    hashtableIterator iter;
    void *next;
    hashtableInitIterator(&iter, ht, 0);
    while (hashtableNext(&iter, &next)) {
        uint8_t *entry = (uint8_t *)next;
        num_returned++;
        ASSERT_GE(entry, entry_array);
        ASSERT_LT(entry, entry_array + count);
        /* increment entry at this position as a counter */
        (*entry)++;
    }
    hashtableCleanupIterator(&iter);

    /* Check that all entries were returned exactly once. */
    ASSERT_EQ(num_returned, count);
    for (size_t j = 0; j < count; j++) {
        ASSERT_EQ(entry_array[j], 1u) << "Entry " << j << " returned " << (int)entry_array[j] << " times";
    }

    hashtableRelease(ht);
    free(entry_array);
}

TEST_F(HashtableTest, safe_iterator) {
    size_t count = 1000;
    uint8_t *entry_counts = (uint8_t *)malloc(count * 2);
    memset(entry_counts, 0, count * 2);

    /* A set of pointers into the uint8_t array. */
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Populate */
    for (size_t j = 0; j < count; j++) {
        ASSERT_TRUE(hashtableAdd(ht, entry_counts + j));
    }

    /* Iterate */
    size_t num_returned = 0;
    hashtableIterator iter;
    void *next;
    hashtableInitIterator(&iter, ht, HASHTABLE_ITER_SAFE);
    while (hashtableNext(&iter, &next)) {
        uint8_t *entry = (uint8_t *)next;
        size_t index = entry - entry_counts;
        num_returned++;
        ASSERT_GE(entry, entry_counts);
        ASSERT_LT(entry, entry_counts + count * 2);
        /* increment entry at this position as a counter */
        (*entry)++;
        if (index % 4 == 0) {
            ASSERT_TRUE(hashtableDelete(ht, entry));
        }
        /* Add new item each time we see one of the original items */
        if (index < count) {
            ASSERT_TRUE(hashtableAdd(ht, entry + count));
        }
    }
    hashtableCleanupIterator(&iter);

    /* Check that all entries present during the whole iteration were returned
     * exactly once. (Some are deleted after being returned.) */
    ASSERT_GE(num_returned, count);
    for (size_t j = 0; j < count; j++) {
        ASSERT_EQ(entry_counts[j], 1u) << "Entry " << j << " returned " << (int)entry_counts[j] << " times";
    }
    /* Check that entries inserted during the iteration were returned at most
     * once. */
    unsigned long num_optional_returned = 0;
    for (size_t j = count; j < count * 2; j++) {
        ASSERT_LE(entry_counts[j], 1u);
        num_optional_returned += entry_counts[j];
    }
    printf("Safe iterator returned %lu of the %zu entries inserted while iterating.\n", num_optional_returned, count);

    hashtableRelease(ht);
    free(entry_counts);
}

TEST_F(HashtableTest, compact_bucket_chain) {
    size_t used_memory_before = zmalloc_used_memory();

    /* Create a table with only one bucket chain. */
    hashtableSetResizePolicy(HASHTABLE_RESIZE_AVOID);
    unsigned long count = 30;

    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Populate */
    unsigned long j;
    for (j = 0; j < count; j++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)j));
    }
    ASSERT_EQ(hashtableBuckets(ht), 1u);
    printf("Populated a single bucket chain, avoiding resize.\n");
    hashtableHistogram(ht);

    /* Delete half of the entries while iterating. */
    size_t num_chained_buckets = hashtableChainedBuckets(ht, 0);
    size_t num_returned = 0;
    hashtableIterator iter;
    hashtableInitIterator(&iter, ht, HASHTABLE_ITER_SAFE);
    void *entry;
    while (hashtableNext(&iter, &entry)) {
        /* As long as the iterator is still returning entries from the same
         * bucket chain, the bucket chain is not compacted, so it still has the
         * same number of buckets. */
        ASSERT_EQ(hashtableChainedBuckets(ht, 0), num_chained_buckets);
        num_returned++;
        if (num_returned % 2 == 0) {
            ASSERT_TRUE(hashtableDelete(ht, entry));
        }
        if (num_returned == count) {
            printf("Last iteration. Half of them have been deleted.\n");
            hashtableHistogram(ht);
        }
    }
    hashtableCleanupIterator(&iter);

    /* Verify that the bucket chain has been compacted by filling the holes and
     * freeing empty child buckets. */
    printf("When the iterator leaves the bucket chain, compaction should happen.\n");
    hashtableHistogram(ht);
    ASSERT_LT(hashtableChainedBuckets(ht, 0), num_chained_buckets);

    hashtableRelease(ht);
    hashtableSetResizePolicy(HASHTABLE_RESIZE_ALLOW);
    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

TEST_F(HashtableTest, random_entry) {
    randomSeed();

    size_t count = large_memory ? 7000 : 400;
    long num_rounds = accurate ? 1000000 : 10000;

    /* A set of ints */
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Populate */
    unsigned *times_picked = (unsigned *)zmalloc(sizeof(unsigned) * count);
    memset(times_picked, 0, sizeof(unsigned) * count);
    for (size_t j = 0; j < count; j++) {
        ASSERT_TRUE(hashtableAdd(ht, times_picked + j));
    }

    /* Pick entries, and count how many times each entry is picked. */
    for (long i = 0; i < num_rounds; i++) {
        /* Using void* variable to avoid a cast that violates strict aliasing */
        void *entry;
        ASSERT_TRUE(hashtableFairRandomEntry(ht, &entry));
        unsigned *picked = (unsigned *)entry;
        ASSERT_GE(picked, times_picked);
        ASSERT_LT(picked, times_picked + count);
        /* increment entry at this position as a counter */
        (*picked)++;
    }
    hashtableRelease(ht);

    /* Fairness measurement
     * --------------------
     *
     * Selecting a single random entry: For any entry in the hash table, let
     * X=1 if the we selected the entry (success) and X=0 otherwise. With m
     * entries, our entry is sepected with probability p = 1/m, the expected
     * value is E(X) = 1/m, E(X^2) = 1/m and the variance:
     *
     *     Var(X) = E(X^2) - (E(X))^2 = 1/m - 1/(m^2) = (1/m) * (1 - 1/m).
     *
     * Repeating the selection of a random entry: Let's repeat the experiment
     * n times and let Y be the number of times our entry was selected. This
     * is a binomial distribution.
     *
     *     Y = X_1 + X_2 + ... + X_n
     *     E(Y) = n/m
     *
     * The variance of a sum of independent random variables is the sum of the
     * variances, so Y has variance np(1−p).
     *
     *     Var(Y) = npq = np(1 - p) = (n/m) * (1 - 1/m) = n * (m - 1) / (m * m)
     */
    double m = (double)count, n = (double)num_rounds;
    double expected = n / m;                 /* E(Y) */
    double variance = n * (m - 1) / (m * m); /* Var(Y) */
    double std_dev = sqrt(variance);

    /* With large n, the distribution approaches a normal distribution and we
     * can use p68 = within 1 std dev, p95 = within 2 std dev, p99.7 = within 3
     * std dev. */
    long p68 = 0, p95 = 0, p99 = 0, p4dev = 0, p5dev = 0, p10percent = 0;
    for (size_t j = 0; j < count; j++) {
        double dev = expected - times_picked[j];
        p68 += (dev >= -std_dev && dev <= std_dev);
        p95 += (dev >= -std_dev * 2 && dev <= std_dev * 2);
        p99 += (dev >= -std_dev * 3 && dev <= std_dev * 3);
        p4dev += (dev >= -std_dev * 4 && dev <= std_dev * 4);
        p5dev += (dev >= -std_dev * 5 && dev <= std_dev * 5);
        p10percent += (dev >= -0.1 * expected && dev <= 0.1 * expected);
    }

    zfree(times_picked);

    printf("Random entry fairness test\n");
    printf("  Pick one of %zu entries, %ld times.\n", count, num_rounds);
    printf("  Expecting each entry to be picked %.2lf times, std dev %.3lf.\n", expected, std_dev);
    printf("  Within 1 std dev (p68) = %.2lf%%\n", 100 * p68 / m);
    printf("  Within 2 std dev (p95) = %.2lf%%\n", 100 * p95 / m);
    printf("  Within 3 std dev (p99) = %.2lf%%\n", 100 * p99 / m);
    printf("  Within 4 std dev       = %.2lf%%\n", 100 * p4dev / m);
    printf("  Within 5 std dev       = %.2lf%%\n", 100 * p5dev / m);
    printf("  Within 10%% dev         = %.2lf%%\n", 100 * p10percent / m);

    /* Conclusion? The number of trials (n) relative to the probabilities (p and
     * 1 − p) must be sufficiently large (n * p ≥ 5 and n * (1 − p) ≥ 5) to
     * approximate a binomial distribution with a normal distribution. */
    if (n / m >= 5 && n * (1 - 1 / m) >= 5) {
        /* Check that 80% of the elements are picked within 3 std deviations of
         * the expected number. This is a low bar, since typically the 99% of
         * the elements are within this range.
         *
         * There is an edge case. When n is very large and m is very small, the
         * std dev of a binomial distribution is very small, which becomes too
         * strict for our bucket layout and makes the test flaky. For example
         * with m = 400 and n = 1M, we get an expected value of 2500 and a std
         * dev of 50, which is just 2% of the expected value. We lower the bar
         * for this case and accept that 80% of elements are just within 10% of
         * the expected value. */
        ASSERT_TRUE(100 * p99 / m >= 80.0 || 100 * p10percent / m >= 80.0) << "Too unfair randomness";
    } else {
        printf("To uncertain numbers to draw any conclusions about fairness.\n");
    }
}

TEST_F(HashtableTest, random_entry_with_long_chain) {
    /* We use an estimator of true probability.
     * We determine how many samples to take based on how precise of a
     * measurement we want to take, and how certain we want to be that the
     * measurement is correct.
     * https://en.wikipedia.org/wiki/Checking_whether_a_coin_is_fair#Estimator_of_true_probability
     */

    /* In a thousand runs the worst deviation seen was 0.018 +/- 0.01.
     * This means the true deviation was at least 0.008 or 0.8%.
     * Accept a deviation of 5% to be on the safe side so we don't get
     * a flaky test case. */
    const double acceptable_probability_deviation = 0.05;

    const size_t num_chained_entries = 64;
    const size_t num_random_entries = 448;
    const double p_fair = (double)num_chained_entries / (num_chained_entries + num_random_entries);

    /* Precision of our measurement */
    const double precision = accurate ? 0.001 : 0.01;

    /* This is confidence level for our measurement as the Z value of a normal
     * distribution. 5 sigma corresponds to 0.00002% probability that our
     * measurement is farther than 'precision' from the truth. This value is
     * used in particle physics. */
    const double z = 5;

    const double n = p_fair * (1 - p_fair) * z * z / (precision * precision);
    const size_t num_samples = (size_t)n + 1;

    hashtableType type = {};
    type.hashFunction = mock_hash_entry_get_hash;
    type.entryDestructor = freekeyval;

    hashtable *ht = hashtableCreate(&type);
    hashtableExpand(ht, num_random_entries + num_chained_entries);
    uint64_t chain_hash = (uint64_t)genrand64_int64();
    if (chain_hash == 0) chain_hash++;

    /* add random entries */
    for (size_t i = 0; i < num_random_entries; i++) {
        uint64_t random_hash = (uint64_t)genrand64_int64();
        if (random_hash == chain_hash) random_hash++;
        hashtableAdd(ht, mock_hash_entry_create(random_hash, 0));
    }

    /* create long chain */
    for (size_t i = 0; i < num_chained_entries; i++) {
        hashtableAdd(ht, mock_hash_entry_create(i, chain_hash));
    }

    ASSERT_FALSE(hashtableIsRehashing(ht));

    printf("Created a table with a long bucket chain.\n");
    hashtableHistogram(ht);

    printf("Taking %zu random samples\n", num_samples);
    size_t count_chain_entry_picked = 0;
    for (size_t i = 0; i < num_samples; i++) {
        void *entry;
        ASSERT_TRUE(hashtableFairRandomEntry(ht, &entry));
        mock_hash_entry *mock_entry = (mock_hash_entry *)entry;
        if (mock_entry->hash == chain_hash) {
            count_chain_entry_picked++;
        }
    }
    const double measured_probability = (double)count_chain_entry_picked / num_samples;
    const double deviation = fabs(measured_probability - p_fair);
    printf("Measured probability: %.1f%%\n", measured_probability * 100);
    printf("Expected probability: %.1f%%\n", p_fair * 100);
    printf("Measured probability deviated %1.1f%% +/- %1.1f%% from expected probability\n",
           deviation * 100, precision * 100);
    ASSERT_LE(deviation, precision + acceptable_probability_deviation);

    hashtableRelease(ht);
}

/* Helper function for scan tests */
static void deleteScanFn(void *privdata, void *entry) {
    hashtable *ht = (hashtable *)privdata;
    hashtableDelete(ht, entry);
}

/* This is a test for random entry selection in sparse hashtables.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=HashtableTest.DISABLED_random_entry_sparse_table --gtest_also_run_disabled_tests
 */
TEST_F(HashtableTest, DISABLED_random_entry_sparse_table) {
    randomSeed();

    size_t count = large_memory ? 100000000 : 1000000;
    long num_rounds = accurate ? 256 * 1024 : 1024;

    /* A set of pointers */
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);
    monotime timer;

    /* Populate */
    unsigned *values = (unsigned *)zmalloc(sizeof(unsigned) * count);
    for (size_t j = 0; j < count; j++) {
        ASSERT_TRUE(hashtableAdd(ht, &values[j]));
    }

    /* Pick random elements */
    elapsedStart(&timer);
    for (long i = 0; i < num_rounds; i++) {
        void *entry;
        ASSERT_TRUE(hashtableFairRandomEntry(ht, &entry));
    }
    uint64_t us0 = elapsedUs(timer);
    printf("Fair random, filled hashtable, avg time: %.3lfµs\n", (double)us0 / num_rounds);

    size_t cursor = random();

    for (int n = 2; n <= 8; n *= 2) {
        /* Scan and delete until only 1/n of the values remain. */
        while (hashtableSize(ht) > count / n) {
            cursor = hashtableScan(ht, cursor, deleteScanFn, ht);
        }

        /* Pick random elements. */
        elapsedStart(&timer);
        for (long i = 0; i < num_rounds; i++) {
            void *entry;
            ASSERT_TRUE(hashtableFairRandomEntry(ht, &entry));
        }
        uint64_t us = elapsedUs(timer);
        printf("Fair random, 1/%d filled hashtable, avg time: %.3lfµs\n", n, (double)us / num_rounds);
        /* Allow max 10 times slower than in a dense table. */
        ASSERT_LE(us, us0 * 10);
    }
    hashtableRelease(ht);
    zfree(values);
}

TEST_F(HashtableTest, safe_iterator_invalidation) {
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Add some entries */
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)(long)i));
    }

    /* Create safe and non-safe iterators */
    hashtableIterator safe_iter1, safe_iter2, unsafe_iter, *dyn_safe_iter;
    hashtableInitIterator(&safe_iter1, ht, HASHTABLE_ITER_SAFE);
    hashtableInitIterator(&safe_iter2, ht, HASHTABLE_ITER_SAFE);
    hashtableInitIterator(&unsafe_iter, ht, 0);
    dyn_safe_iter = hashtableCreateIterator(ht, HASHTABLE_ITER_SAFE);

    /* Verify iterators work before invalidation */
    void *entry;
    ASSERT_TRUE(hashtableNext(&safe_iter1, &entry));
    ASSERT_TRUE(hashtableNext(&safe_iter2, &entry));

    /* Reset iterators to test state */
    hashtableCleanupIterator(&safe_iter1);
    hashtableCleanupIterator(&safe_iter2);
    hashtableReleaseIterator(dyn_safe_iter);
    hashtableInitIterator(&safe_iter1, ht, HASHTABLE_ITER_SAFE);
    hashtableInitIterator(&safe_iter2, ht, HASHTABLE_ITER_SAFE);

    /* Release hashtable - should invalidate safe iterators */
    hashtableRelease(ht);

    /* Test that safe iterators are now invalid */
    ASSERT_FALSE(hashtableNext(&safe_iter1, &entry));
    ASSERT_FALSE(hashtableNext(&safe_iter2, &entry));

    /* Reset invalidated iterators (should handle gracefully) */
    hashtableCleanupIterator(&safe_iter1);
    hashtableCleanupIterator(&safe_iter2);
    hashtableCleanupIterator(&unsafe_iter);
}

TEST_F(HashtableTest, safe_iterator_empty_no_invalidation) {
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Add some entries */
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)(long)i));
    }

    /* Create safe iterator */
    hashtableIterator safe_iter;
    hashtableInitIterator(&safe_iter, ht, HASHTABLE_ITER_SAFE);

    /* Empty hashtable - should NOT invalidate safe iterators */
    hashtableEmpty(ht, nullptr);

    /* Iterator should still be valid but return false since table is empty */
    void *entry;
    ASSERT_FALSE(hashtableNext(&safe_iter, &entry));

    hashtableCleanupIterator(&safe_iter);
    hashtableRelease(ht);
}

TEST_F(HashtableTest, safe_iterator_reset_invalidation) {
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Add some entries */
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)(long)i));
    }

    /* Create safe iterators */
    hashtableIterator safe_iter1, safe_iter2;
    hashtableInitIterator(&safe_iter1, ht, HASHTABLE_ITER_SAFE);
    hashtableInitIterator(&safe_iter2, ht, HASHTABLE_ITER_SAFE);

    /* Verify iterators work before reset */
    void *entry;
    ASSERT_TRUE(hashtableNext(&safe_iter1, &entry));
    ASSERT_TRUE(hashtableNext(&safe_iter2, &entry));

    /* Reset iterators to test state */
    hashtableCleanupIterator(&safe_iter1);
    hashtableCleanupIterator(&safe_iter2);
    hashtableInitIterator(&safe_iter1, ht, HASHTABLE_ITER_SAFE);
    hashtableInitIterator(&safe_iter2, ht, HASHTABLE_ITER_SAFE);

    /* Reset one iterator before release - should untrack it */
    hashtableCleanupIterator(&safe_iter1);
    /* Repeated calls are ok */
    hashtableCleanupIterator(&safe_iter1);

    /* Release hashtable - should invalidate remaining safe iterator */
    hashtableRelease(ht);

    /* Test that safe iterators are prevented from invalid access */
    ASSERT_FALSE(hashtableNext(&safe_iter1, &entry));
    ASSERT_FALSE(hashtableNext(&safe_iter2, &entry));

    /* Reset invalidated iterators (should handle gracefully) */
    hashtableCleanupIterator(&safe_iter1);
    hashtableCleanupIterator(&safe_iter2);
}

TEST_F(HashtableTest, safe_iterator_reset_untracking) {
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Add some entries */
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)(long)i));
    }

    /* Create safe iterator */
    hashtableIterator safe_iter;
    hashtableInitIterator(&safe_iter, ht, HASHTABLE_ITER_SAFE);

    /* Reset iterator - should remove from tracking */
    hashtableCleanupIterator(&safe_iter);

    /* Release hashtable - iterator should not be invalidated since it was reset */
    hashtableRelease(ht);

    /* Create new hashtable and reinit iterator */
    ht = hashtableCreate(&type);
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)(long)i));
    }
    hashtableInitIterator(&safe_iter, ht, HASHTABLE_ITER_SAFE);

    /* Should work since it's tracking the new hashtable */
    void *entry;
    ASSERT_TRUE(hashtableNext(&safe_iter, &entry));

    hashtableCleanupIterator(&safe_iter);
    hashtableRelease(ht);
}

TEST_F(HashtableTest, safe_iterator_pause_resume_tracking) {
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);

    /* Add entries to trigger rehashing */
    for (int i = 0; i < 100; i++) {
        ASSERT_TRUE(hashtableAdd(ht, (void *)(long)i));
    }

    /* Create multiple safe iterators */
    hashtableIterator iter1, iter2, iter3;
    hashtableInitIterator(&iter1, ht, HASHTABLE_ITER_SAFE);
    hashtableInitIterator(&iter2, ht, HASHTABLE_ITER_SAFE);
    hashtableInitIterator(&iter3, ht, HASHTABLE_ITER_SAFE);

    /* Start iterating with first iterator - should pause rehashing and track iterator */
    void *entry;
    ASSERT_TRUE(hashtableNext(&iter1, &entry));

    /* Start iterating with second iterator - should also be tracked */
    ASSERT_TRUE(hashtableNext(&iter2, &entry));

    /* Verify rehashing is paused (safe iterators should pause it) */
    ASSERT_TRUE(hashtableIsRehashingPaused(ht));

    /* Reset first iterator - should untrack it but rehashing still paused due to iter2 */
    hashtableCleanupIterator(&iter1);

    /* Start third iterator */
    ASSERT_TRUE(hashtableNext(&iter3, &entry));

    /* Reset second iterator - rehashing should still be paused due to iter3 */
    hashtableCleanupIterator(&iter2);
    ASSERT_TRUE(hashtableIsRehashingPaused(ht));

    /* Reset third iterator - now rehashing should be resumed */
    hashtableCleanupIterator(&iter3);

    /* Verify all iterators are properly untracked by releasing hashtable */
    hashtableRelease(ht);
}

TEST_F(HashtableTest, null_hashtable_iterator) {
    /* Test safe iterator with NULL hashtable */
    hashtableIterator safe_iter;
    hashtableInitIterator(&safe_iter, nullptr, HASHTABLE_ITER_SAFE);

    /* hashtableNext should return false for NULL hashtable */
    void *entry;
    ASSERT_FALSE(hashtableNext(&safe_iter, &entry));

    /* Reset should handle NULL hashtable gracefully */
    hashtableCleanupIterator(&safe_iter);

    /* Test non-safe iterator with NULL hashtable */
    hashtableIterator unsafe_iter;
    hashtableInitIterator(&unsafe_iter, nullptr, 0);

    /* hashtableNext should return false for NULL hashtable */
    ASSERT_FALSE(hashtableNext(&unsafe_iter, &entry));

    /* Reset should handle NULL hashtable gracefully */
    hashtableCleanupIterator(&unsafe_iter);

    /* Test reinitializing NULL iterator with valid hashtable */
    hashtableType type = {};
    hashtable *ht = hashtableCreate(&type);
    ASSERT_TRUE(hashtableAdd(ht, (void *)1));

    hashtableRetargetIterator(&safe_iter, ht);
    ASSERT_TRUE(hashtableNext(&safe_iter, &entry));
    ASSERT_EQ(entry, (void *)1);

    hashtableCleanupIterator(&safe_iter);
    hashtableRelease(ht);
}

TEST_F(HashtableTest, hashtable_retarget_iterator) {
    hashtableType type = {};
    hashtable *ht1 = hashtableCreate(&type);
    hashtable *ht2 = hashtableCreate(&type);

    /* Add different entries to each hashtable */
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(hashtableAdd(ht1, (void *)(long)(i + 10)));
        ASSERT_TRUE(hashtableAdd(ht2, (void *)(long)(i + 20)));
    }

    /* Create iterator on first hashtable */
    hashtableIterator iter;
    hashtableInitIterator(&iter, ht1, HASHTABLE_ITER_SAFE);

    /* Iterate partially through first hashtable */
    void *entry;
    int count1 = 0;
    while (hashtableNext(&iter, &entry) && count1 < 3) {
        long val = (long)entry;
        ASSERT_GE(val, 10);
        ASSERT_LT(val, 15);
        count1++;
    }

    /* Retarget to second hashtable */
    hashtableRetargetIterator(&iter, ht2);

    /* Iterate partially through second hashtable */
    int count2 = 0;
    while (hashtableNext(&iter, &entry) && count2 < 2) {
        long val = (long)entry;
        ASSERT_GE(val, 20);
        ASSERT_LT(val, 25);
        count2++;
    }

    /* Retarget back to first hashtable */
    hashtableRetargetIterator(&iter, ht1);

    /* Iterate partially through first hashtable again */
    int count3 = 0;
    while (hashtableNext(&iter, &entry) && count3 < 4) {
        long val = (long)entry;
        ASSERT_GE(val, 10);
        ASSERT_LT(val, 15);
        count3++;
    }

    hashtableRelease(ht1);
    hashtableRelease(ht2);

    ASSERT_FALSE(hashtableNext(&iter, &entry));

    hashtableCleanupIterator(&iter);
}
