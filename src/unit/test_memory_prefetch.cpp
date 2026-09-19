/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "memory_prefetch.h"
#include "server.h"
}

class MemoryPrefetchTest : public ::testing::Test {
  protected:
    serverDb db;

    void SetUp() override {
        memset(&db, 0, sizeof(db));
        db.id = 0;
        db.keys = kvstoreCreate(&kvstoreKeysHashtableType, 0, 0);
    }

    void TearDown() override {
        if (db.keys != NULL) {
            kvstoreEmpty(db.keys, NULL);
            kvstoreRelease(db.keys);
            db.keys = NULL;
        }
    }
};

/* Test prefetchStringKey with empty database, non-empty database, and edge cases. */
TEST_F(MemoryPrefetchTest, TestPrefetchStringKey) {
    robj *key1 = createStringObject("key1", 4);
    robj *key2 = createStringObject("key2", 4);

    /* Edge cases: NULL database, NULL key, non-string object */
    prefetchStringKey(NULL, key1);
    prefetchStringKey(&db, NULL);

    robj *int_key = createObject(OBJ_STRING, (void *)100);
    int_key->encoding = OBJ_ENCODING_INT;
    prefetchStringKey(&db, int_key);
    decrRefCount(int_key);

    robj *list_obj = createQuicklistObject(0, 0);
    prefetchStringKey(&db, list_obj);
    decrRefCount(list_obj);

    /* Prefetch on empty database - should execute safely without crash */
    prefetchStringKey(&db, key1);

    /* Add key1 into db.keys */
    sds k1_sds = sdsnew("key1");
    robj *val1 = createStringObject("val1", 4);
    val1 = objectSetKeyAndExpire(val1, k1_sds, -1);
    sdsfree(k1_sds);
    kvstoreHashtableAdd(db.keys, 0, val1);

    /* Prefetch existing key and non-existing key */
    prefetchStringKey(&db, key1);
    prefetchStringKey(&db, key2);

    decrRefCount(key1);
    decrRefCount(key2);
}

/* Test prefetchStringKey and prefetchKeyBucketRange with chained collision buckets and active rehashing. */
TEST_F(MemoryPrefetchTest, TestPrefetchStringKeyCollisionsAndRehash) {
    hashtableSetResizePolicy(HASHTABLE_RESIZE_AVOID);

    const int num_keys = 20;
    robj *keys[num_keys];

    /* Insert the first key to initialize hashtable and determine the target bucket. */
    keys[0] = createStringObject("collision_key_0", 15);
    sds k0_sds = sdsnew("collision_key_0");
    robj *val0 = createRawStringObject("collision_data", 14);
    val0 = objectSetKeyAndExpire(val0, k0_sds, -1);
    sdsfree(k0_sds);
    kvstoreHashtableAdd(db.keys, 0, val0);

    hashtable *ht = kvstoreGetHashtable(db.keys, 0);
    ASSERT_TRUE(ht != NULL);
    ASSERT_GT(hashtableBuckets(ht), (size_t)0);

    size_t bucket_mask = hashtableBuckets(ht) - 1;
    sds first_key_sds = (sds)objectGetVal(keys[0]);
    size_t target_bucket = hashtableGetType(ht)->hashFunction(first_key_sds) & bucket_mask;

    int found = 1;
    int candidate_id = 1;

    /* Derive remaining keys that hash to the exact same bucket index to force a collision chain. */
    while (found < num_keys) {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "collision_key_%d", candidate_id++);
        sds k_sds = sdsnewlen(buf, len);
        uint64_t hash = hashtableGetType(ht)->hashFunction(k_sds);
        size_t b_idx = hash & bucket_mask;

        if (b_idx == target_bucket) {
            keys[found] = createStringObject(buf, len);
            robj *val = createRawStringObject("collision_data", 14);
            val = objectSetKeyAndExpire(val, k_sds, -1);
            sdsfree(k_sds);
            kvstoreHashtableAdd(db.keys, 0, val);
            found++;
        } else {
            sdsfree(k_sds);
        }
    }

    /* Verify that a collision chain with chained child buckets was formed. */
    ASSERT_GT(hashtableChainedBuckets(ht, 0), (size_t)0);

    /* Prefetch existing keys across chained collision buckets */
    for (int i = 0; i < num_keys; i++) {
        prefetchStringKey(&db, keys[i]);
    }
    prefetchKeyBucketRange(&db, keys, 0, num_keys, 1, 8);

    /* Trigger active rehashing and assert that rehashing is active */
    hashtableSetResizePolicy(HASHTABLE_RESIZE_ALLOW);
    hashtableExpand(ht, 128);
    ASSERT_TRUE(hashtableIsRehashing(ht));

    /* Prefetch while table is actively rehashing across table 0 and table 1 */
    for (int i = 0; i < num_keys; i++) {
        prefetchStringKey(&db, keys[i]);
    }
    prefetchKeyBucketRange(&db, keys, 0, num_keys, 1, 8);

    for (int i = 0; i < num_keys; i++) {
        decrRefCount(keys[i]);
    }

    hashtableSetResizePolicy(HASHTABLE_RESIZE_ALLOW);
}


/* Test prefetchKeyBucketRange with NULL pointers and invalid stride values. */
TEST_F(MemoryPrefetchTest, TestPrefetchKeyBucketRangeInvalidInputs) {
    robj *argv[4];
    for (int i = 0; i < 4; i++) {
        argv[i] = createStringObject("k", 1);
    }

    /* NULL db or keys should return safely */
    prefetchKeyBucketRange(NULL, argv, 0, 4, 1, 2);

    serverDb empty_db;
    memset(&empty_db, 0, sizeof(empty_db));
    prefetchKeyBucketRange(&empty_db, argv, 0, 4, 1, 2);

    /* NULL argv should return safely */
    prefetchKeyBucketRange(&db, NULL, 0, 4, 1, 2);

    /* stride <= 0 should return safely without division by zero */
    prefetchKeyBucketRange(&db, argv, 0, 4, 0, 2);
    prefetchKeyBucketRange(&db, argv, 0, 4, -1, 2);

    /* start >= end should return safely */
    prefetchKeyBucketRange(&db, argv, 4, 2, 1, 2);
    prefetchKeyBucketRange(&db, argv, 2, 2, 1, 2);

    for (int i = 0; i < 4; i++) {
        decrRefCount(argv[i]);
    }
}

/* Test prefetchKeyBucketRange with offset <= 0. */
TEST_F(MemoryPrefetchTest, TestPrefetchKeyBucketRangeZeroOrNegativeOffset) {
    robj *argv[6];
    argv[0] = NULL;
    for (int i = 1; i < 6; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "k%d", i);
        argv[i] = createStringObject(buf, len);
    }

    /* offset == 0 should do nothing and return immediately */
    prefetchKeyBucketRange(&db, argv, 1, 6, 1, 0);

    /* negative offsets should do nothing and return immediately */
    prefetchKeyBucketRange(&db, argv, 1, 6, 1, -1);
    prefetchKeyBucketRange(&db, argv, 1, 6, 1, -5);

    for (int i = 1; i < 6; i++) {
        decrRefCount(argv[i]);
    }
}

/* Test prefetchKeyBucketRange when range contains 0 or 1 keys. */
TEST_F(MemoryPrefetchTest, TestPrefetchKeyBucketRangeSingleOrEmptyKey) {
    robj *argv[4];
    argv[0] = NULL;
    for (int i = 1; i < 4; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "k%d", i);
        argv[i] = createStringObject(buf, len);
    }

    /* Empty ranges */
    prefetchKeyBucketRange(&db, argv, 1, 1, 1, 4);
    prefetchKeyBucketRange(&db, argv, 3, 2, 1, 4);

    /* Single key range with stride 1 (end - start = 1) */
    prefetchKeyBucketRange(&db, argv, 1, 2, 1, 4);

    /* Single key range with stride 2 (end - start = 2) */
    prefetchKeyBucketRange(&db, argv, 1, 3, 2, 4);

    for (int i = 1; i < 4; i++) {
        decrRefCount(argv[i]);
    }
}

/* Test prefetchKeyBucketRange with stride 1 (MGET, DEL, EXISTS, TOUCH pattern). */
TEST_F(MemoryPrefetchTest, TestPrefetchKeyBucketRangeStride1) {
    robj *argv[11];
    argv[0] = NULL;
    for (int i = 1; i < 11; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "k%d", i);
        argv[i] = createStringObject(buf, len);
    }

    /* Populate database with some keys */
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "k%d", i);
        sds k_sds = sdsnewlen(buf, len);
        robj *val = createStringObject("val", 3);
        val = objectSetKeyAndExpire(val, k_sds, -1);
        sdsfree(k_sds);
        kvstoreHashtableAdd(db.keys, 0, val);
    }

    /* Prefetch 4 keys (offset 4, stride 1, start 1, end 11 -> prefetch argv[1..4]) */
    prefetchKeyBucketRange(&db, argv, 1, 11, 1, 4);

    for (int i = 1; i < 11; i++) {
        decrRefCount(argv[i]);
    }
}

/* Test prefetchKeyBucketRange with stride 2 (MSET / MSETNX / MSETEX pattern). */
TEST_F(MemoryPrefetchTest, TestPrefetchKeyBucketRangeStride2) {
    /* MSET pattern: argv[1]=k1, argv[2]=v1, argv[3]=k2, argv[4]=v2, argv[5]=k3, argv[6]=v3, argv[7]=k4, argv[8]=v4 */
    robj *mset_argv[9];
    mset_argv[0] = NULL;
    for (int i = 1; i < 9; i += 2) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "k%d", i);
        mset_argv[i] = createStringObject(buf, len);
        mset_argv[i + 1] = createStringObject("v", 1);
    }

    /* offset = 2, stride = 2: prefetches start (1) and 1 + 2 = 3 */
    prefetchKeyBucketRange(&db, mset_argv, 1, 9, 2, 2);

    for (int i = 1; i < 9; i++) {
        decrRefCount(mset_argv[i]);
    }

    /* MSETEX pattern: argv[0]=cmd, argv[1]=ttl, argv[2]=k1, argv[3]=v1, argv[4]=k2, argv[5]=v2, argv[6]=k3, argv[7]=v3 */
    robj *msetex_argv[8];
    msetex_argv[0] = NULL;
    msetex_argv[1] = createStringObject("1000", 4);
    for (int i = 2; i < 8; i += 2) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "k%d", i);
        msetex_argv[i] = createStringObject(buf, len);
        msetex_argv[i + 1] = createStringObject("v", 1);
    }

    /* start = 2, end = 8, stride = 2, offset = 2: prefetches argv[2] and argv[4] */
    prefetchKeyBucketRange(&db, msetex_argv, 2, 8, 2, 2);

    for (int i = 1; i < 8; i++) {
        decrRefCount(msetex_argv[i]);
    }
}

/* Test prefetchKeyBucketRange when offset exceeds the number of elements. */
TEST_F(MemoryPrefetchTest, TestPrefetchKeyBucketRangeOffsetExceedsEnd) {
    robj *argv[5];
    argv[0] = NULL;
    for (int i = 1; i < 5; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "k%d", i);
        argv[i] = createStringObject(buf, len);
    }

    /* offset 50 exceeds total elements (4 keys) */
    prefetchKeyBucketRange(&db, argv, 1, 5, 1, 50);

    /* stride 2 with large offset */
    prefetchKeyBucketRange(&db, argv, 1, 5, 2, 50);

    for (int i = 1; i < 5; i++) {
        decrRefCount(argv[i]);
    }
}
