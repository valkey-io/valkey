/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "kvstore.h"
#include "monotonic.h"
}

uint64_t hashTestCallback(const void *key) {
    return hashtableGenHashFunction((const char *)key, strlen((const char *)key));
}

uint64_t hashConflictTestCallback(const void *key) {
    UNUSED(key);
    return 0;
}

int cmpTestCallback(const void *k1, const void *k2) {
    return strcmp((const char *)k1, (const char *)k2) == 0;
}

void freeTestCallback(void *val) {
    zfree(val);
}

/* Hashtable types used for tests - initialized in SetUpTestSuite */
static hashtableType KvstoreHashtableTestType;
static hashtableType KvstoreConflictHashtableTestType;

char *stringFromInt(int value) {
    char buf[32];
    int len;
    char *s;

    len = snprintf(buf, sizeof(buf), "%d", value);
    s = (char *)zmalloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

class KvstoreTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        monotonicInit();

        /* Initialize KvstoreHashtableTestType explicitly by field name to avoid
         * dependency on field order (designated initializers require C++20). */
        memset(&KvstoreHashtableTestType, 0, sizeof(KvstoreHashtableTestType));
        KvstoreHashtableTestType.hashFunction = hashTestCallback;
        KvstoreHashtableTestType.keyCompare = cmpTestCallback;
        KvstoreHashtableTestType.entryDestructor = freeTestCallback;
        KvstoreHashtableTestType.rehashingStarted = kvstoreHashtableRehashingStarted;
        KvstoreHashtableTestType.rehashingCompleted = kvstoreHashtableRehashingCompleted;
        KvstoreHashtableTestType.trackMemUsage = kvstoreHashtableTrackMemUsage;
        KvstoreHashtableTestType.getMetadataSize = kvstoreHashtableMetadataSize;

        /* Initialize KvstoreConflictHashtableTestType */
        memset(&KvstoreConflictHashtableTestType, 0, sizeof(KvstoreConflictHashtableTestType));
        KvstoreConflictHashtableTestType.hashFunction = hashConflictTestCallback;
        KvstoreConflictHashtableTestType.keyCompare = cmpTestCallback;
        KvstoreConflictHashtableTestType.entryDestructor = freeTestCallback;
        KvstoreConflictHashtableTestType.rehashingStarted = kvstoreHashtableRehashingStarted;
        KvstoreConflictHashtableTestType.rehashingCompleted = kvstoreHashtableRehashingCompleted;
        KvstoreConflictHashtableTestType.trackMemUsage = kvstoreHashtableTrackMemUsage;
        KvstoreConflictHashtableTestType.getMetadataSize = kvstoreHashtableMetadataSize;
    }
};

TEST_F(KvstoreTest, kvstoreAdd16Keys) {
    int i;

    int didx = 0;
    kvstore *kvs0 = kvstoreCreate(&KvstoreHashtableTestType, 0, 0);
    kvstore *kvs1 = kvstoreCreate(&KvstoreHashtableTestType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND);
    kvstore *kvs2 = kvstoreCreate(&KvstoreHashtableTestType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND | KVSTORE_FREE_EMPTY_HASHTABLES);

    for (i = 0; i < 16; i++) {
        ASSERT_TRUE(kvstoreHashtableAdd(kvs0, didx, stringFromInt(i)));
        ASSERT_TRUE(kvstoreHashtableAdd(kvs1, didx, stringFromInt(i)));
        ASSERT_TRUE(kvstoreHashtableAdd(kvs2, didx, stringFromInt(i)));
    }
    ASSERT_EQ(kvstoreHashtableSize(kvs0, didx), 16u);
    ASSERT_EQ(kvstoreSize(kvs0), 16u);
    ASSERT_EQ(kvstoreHashtableSize(kvs1, didx), 16u);
    ASSERT_EQ(kvstoreSize(kvs1), 16u);
    ASSERT_EQ(kvstoreHashtableSize(kvs2, didx), 16u);
    ASSERT_EQ(kvstoreSize(kvs2), 16u);

    kvstoreRelease(kvs0);
    kvstoreRelease(kvs1);
    kvstoreRelease(kvs2);
}

TEST_F(KvstoreTest, kvstoreIteratorRemoveAllKeysNoDeleteEmptyHashtable) {
    hashtableType *type[] = {
        &KvstoreHashtableTestType,
        &KvstoreConflictHashtableTestType,
        nullptr,
    };

    for (int t = 0; type[t] != nullptr; t++) {
        hashtableType *testType = type[t];
        printf("Testing %d hashtableType\n", t);

        int i;
        void *key;
        kvstoreIterator *kvs_it;

        int didx = 0;
        int curr_slot = 0;
        kvstore *kvs1 = kvstoreCreate(testType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND);

        for (i = 0; i < 16; i++) {
            ASSERT_TRUE(kvstoreHashtableAdd(kvs1, didx, stringFromInt(i)));
        }

        kvs_it = kvstoreIteratorInit(kvs1, HASHTABLE_ITER_SAFE);
        while (kvstoreIteratorNext(kvs_it, &key)) {
            curr_slot = kvstoreIteratorGetCurrentHashtableIndex(kvs_it);
            ASSERT_TRUE(kvstoreHashtableDelete(kvs1, curr_slot, key));
        }
        kvstoreIteratorRelease(kvs_it);

        hashtable *ht = kvstoreGetHashtable(kvs1, didx);
        ASSERT_NE(ht, nullptr);
        ASSERT_EQ(kvstoreHashtableSize(kvs1, didx), 0u);
        ASSERT_EQ(kvstoreSize(kvs1), 0u);

        kvstoreRelease(kvs1);
    }
}

TEST_F(KvstoreTest, kvstoreIteratorRemoveAllKeysDeleteEmptyHashtable) {
    int i;
    void *key;
    kvstoreIterator *kvs_it;

    int didx = 0;
    int curr_slot = 0;
    kvstore *kvs2 = kvstoreCreate(&KvstoreHashtableTestType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND | KVSTORE_FREE_EMPTY_HASHTABLES);

    for (i = 0; i < 16; i++) {
        ASSERT_TRUE(kvstoreHashtableAdd(kvs2, didx, stringFromInt(i)));
    }

    kvs_it = kvstoreIteratorInit(kvs2, HASHTABLE_ITER_SAFE);
    while (kvstoreIteratorNext(kvs_it, &key)) {
        curr_slot = kvstoreIteratorGetCurrentHashtableIndex(kvs_it);
        ASSERT_TRUE(kvstoreHashtableDelete(kvs2, curr_slot, key));
    }
    kvstoreIteratorRelease(kvs_it);

    /* Make sure the hashtable was removed from the rehashing list. */
    while (kvstoreIncrementallyRehash(kvs2, 1000)) {
    }

    hashtable *ht = kvstoreGetHashtable(kvs2, didx);
    ASSERT_EQ(ht, nullptr);
    ASSERT_EQ(kvstoreHashtableSize(kvs2, didx), 0u);
    ASSERT_EQ(kvstoreSize(kvs2), 0u);

    kvstoreRelease(kvs2);
}

TEST_F(KvstoreTest, kvstoreHashtableIteratorRemoveAllKeysNoDeleteEmptyHashtable) {
    int i;
    void *key;
    kvstoreHashtableIterator *kvs_di;

    int didx = 0;
    kvstore *kvs1 = kvstoreCreate(&KvstoreHashtableTestType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND);

    for (i = 0; i < 16; i++) {
        ASSERT_TRUE(kvstoreHashtableAdd(kvs1, didx, stringFromInt(i)));
    }

    kvs_di = kvstoreGetHashtableIterator(kvs1, didx, HASHTABLE_ITER_SAFE);
    while (kvstoreHashtableIteratorNext(kvs_di, &key)) {
        ASSERT_TRUE(kvstoreHashtableDelete(kvs1, didx, key));
    }
    kvstoreReleaseHashtableIterator(kvs_di);

    hashtable *ht = kvstoreGetHashtable(kvs1, didx);
    ASSERT_NE(ht, nullptr);
    ASSERT_EQ(kvstoreHashtableSize(kvs1, didx), 0u);
    ASSERT_EQ(kvstoreSize(kvs1), 0u);

    kvstoreRelease(kvs1);
}

TEST_F(KvstoreTest, kvstoreHashtableIteratorRemoveAllKeysDeleteEmptyHashtable) {
    int i;
    void *key;
    kvstoreHashtableIterator *kvs_di;

    int didx = 0;
    kvstore *kvs2 = kvstoreCreate(&KvstoreHashtableTestType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND | KVSTORE_FREE_EMPTY_HASHTABLES);

    for (i = 0; i < 16; i++) {
        ASSERT_TRUE(kvstoreHashtableAdd(kvs2, didx, stringFromInt(i)));
    }

    kvs_di = kvstoreGetHashtableIterator(kvs2, didx, HASHTABLE_ITER_SAFE);
    while (kvstoreHashtableIteratorNext(kvs_di, &key)) {
        ASSERT_TRUE(kvstoreHashtableDelete(kvs2, didx, key));
    }
    kvstoreReleaseHashtableIterator(kvs_di);

    hashtable *ht = kvstoreGetHashtable(kvs2, didx);
    ASSERT_EQ(ht, nullptr);
    ASSERT_EQ(kvstoreHashtableSize(kvs2, didx), 0u);
    ASSERT_EQ(kvstoreSize(kvs2), 0u);

    kvstoreRelease(kvs2);
}

/* Starts rehashing in the hash table at 'didx' by adding some keys and then
 * expanding it. */
static void startRehashing(kvstore *kvs, int didx, int first_key) {
    for (int i = first_key; i < first_key + 100; i++) {
        ASSERT_TRUE(kvstoreHashtableAdd(kvs, didx, stringFromInt(i)));
    }
    ASSERT_TRUE(kvstoreHashtableExpand(kvs, didx, 10000));
    ASSERT_TRUE(hashtableIsRehashing(kvstoreGetHashtable(kvs, didx)));
}

TEST_F(KvstoreTest, kvstoreIncrementallyRehashSkipsPausedHashtable) {
    /* Use a large time budget. If the function spins on the paused hash
     * table, it uses the whole budget. */
    const uint64_t threshold_us = 1000000;
    kvstore *kvs = kvstoreCreate(&KvstoreHashtableTestType, 1, 0);

    /* Hash table 0 is first in the rehashing list. */
    startRehashing(kvs, 0, 0);
    startRehashing(kvs, 1, 100);
    ASSERT_EQ(kvstoreHashtableRehashingCount(kvs), 2u);

    hashtable *ht0 = kvstoreGetHashtable(kvs, 0);
    hashtable *ht1 = kvstoreGetHashtable(kvs, 1);
    hashtablePauseRehashing(ht0);

    /* The paused hash table is skipped and the next one is rehashed. */
    uint64_t elapsed_us = kvstoreIncrementallyRehash(kvs, threshold_us);
    ASSERT_LT(elapsed_us, threshold_us);
    ASSERT_TRUE(hashtableIsRehashing(ht0));
    ASSERT_FALSE(hashtableIsRehashing(ht1));
    ASSERT_EQ(kvstoreHashtableRehashingCount(kvs), 1u);

    /* Only the paused hash table is left. Return without using the budget. */
    elapsed_us = kvstoreIncrementallyRehash(kvs, threshold_us);
    ASSERT_LT(elapsed_us, threshold_us);
    ASSERT_TRUE(hashtableIsRehashing(ht0));

    /* After resuming, the hash table completes rehashing. */
    hashtableResumeRehashing(ht0);
    kvstoreIncrementallyRehash(kvs, threshold_us);
    ASSERT_FALSE(hashtableIsRehashing(ht0));
    ASSERT_EQ(kvstoreHashtableRehashingCount(kvs), 0u);

    kvstoreRelease(kvs);
}

TEST_F(KvstoreTest, kvstoreIncrementallyRehashStopsWhenResizeForbidden) {
    const uint64_t threshold_us = 1000000;
    kvstore *kvs = kvstoreCreate(&KvstoreHashtableTestType, 1, 0);

    startRehashing(kvs, 0, 0);
    startRehashing(kvs, 1, 100);
    ASSERT_EQ(kvstoreHashtableRehashingCount(kvs), 2u);

    /* No hash table can rehash. Return without using the budget. */
    hashtableSetResizePolicy(HASHTABLE_RESIZE_FORBID);
    uint64_t elapsed_us = kvstoreIncrementallyRehash(kvs, threshold_us);
    hashtableSetResizePolicy(HASHTABLE_RESIZE_ALLOW);
    ASSERT_LT(elapsed_us, threshold_us);
    ASSERT_EQ(kvstoreHashtableRehashingCount(kvs), 2u);

    kvstoreIncrementallyRehash(kvs, threshold_us);
    ASSERT_EQ(kvstoreHashtableRehashingCount(kvs), 0u);

    kvstoreRelease(kvs);
}

TEST_F(KvstoreTest, kvstoreHashtableExpand) {
    kvstore *kvs = kvstoreCreate(&KvstoreHashtableTestType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND | KVSTORE_FREE_EMPTY_HASHTABLES);

    ASSERT_EQ(kvstoreGetHashtable(kvs, 0), nullptr);
    ASSERT_TRUE(kvstoreHashtableExpand(kvs, 0, 10000));
    ASSERT_NE(kvstoreGetHashtable(kvs, 0), nullptr);
    ASSERT_GT(kvstoreBuckets(kvs), 0u);
    ASSERT_EQ(kvstoreBuckets(kvs), kvstoreHashtableBuckets(kvs, 0));

    kvstoreRelease(kvs);
}
