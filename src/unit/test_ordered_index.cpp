/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "ordered_index.h"
#include "server.h"
}

#define TEST_ASSERT(x) ASSERT_TRUE(x)
#define TEST_ASSERT_SCORE_EQ(a, b) ASSERT_DOUBLE_EQ(a, b)

/* Use double infinity to avoid -Wdouble-promotion on macOS where INFINITY is float */
static const double POS_INF = (double)INFINITY;
static const double NEG_INF = (double)-INFINITY;

/* Generic tests that work with any OrderedIndex implementation */

static void test_create_free_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);
    TEST_ASSERT(idx != NULL);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 0);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_insert_single_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);
    sds ele = sdsnew("test");
    OrderedIndexItem *node = orderedIndexInsert(ops, idx, 1.0, ele);

    TEST_ASSERT(node != NULL);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 1);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, node), 1.0);

    const char *ptr;
    size_t len;
    orderedIndexGetElementRaw(ops, node, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "test", 4) == 0);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT(pos == node);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    sdsfree(ele);
    orderedIndexFree(ops, idx);
}

static void test_insert_multiple_ordered_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    TEST_ASSERT(orderedIndexLength(ops, idx) == 10);

    /* Verify forward traversal */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), (double)i);
    }
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    /* Verify backward traversal */
    orderedIndexInitIterator(ops, &iter, idx);
    for (int i = 9; i >= 0; i--) {
        TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), (double)i);
    }
    TEST_ASSERT(!orderedIndexPrev(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_duplicate_scores_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert elements with same score but different keys */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, 1.0, ele);
        sdsfree(ele);
    }

    TEST_ASSERT(orderedIndexLength(ops, idx) == 5);

    /* Verify lexicographic ordering for same scores */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
        const char *ptr;
        size_t len;
        orderedIndexGetElementRaw(ops, pos, &ptr, &len);
        char expected[32];
        snprintf(expected, sizeof(expected), "key%d", i);
        TEST_ASSERT(len == strlen(expected) && memcmp(ptr, expected, len) == 0);
    }
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_rank_operations_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);
    OrderedIndexItem *nodes[10];

    /* Insert 10 elements */
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        nodes[i] = orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    /* Test get_rank */
    for (int i = 0; i < 10; i++) {
        unsigned long rank = orderedIndexGetRank(ops, idx, nodes[i]);
        TEST_ASSERT(rank == (unsigned long)(i + 1)); /* 1-based */
    }

    /* Test get_by_rank */
    for (int i = 0; i < 10; i++) {
        OrderedIndexItem *node = orderedIndexGetByRank(ops, idx, i + 1);
        TEST_ASSERT(node == nodes[i]);
    }

    orderedIndexFree(ops, idx);
}

static void test_delete_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);
    OrderedIndexItem *nodes[5];

    /* Insert 5 elements */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        nodes[i] = orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    TEST_ASSERT(orderedIndexLength(ops, idx) == 5);

    /* Delete middle element */
    orderedIndexDelete(ops, idx, nodes[2]);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 4);

    /* Verify remaining elements */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 0.0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 3.0); /* Skipped 2.0 */
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 4.0);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_pop_first_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Pop from empty - should return NULL */
    TEST_ASSERT(orderedIndexPopFirst(ops, idx) == NULL);

    /* Insert elements */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }
    TEST_ASSERT(orderedIndexLength(ops, idx) == 5);

    /* Pop first - should get score 0.0 */
    OrderedIndexItem *item = orderedIndexPopFirst(ops, idx);
    TEST_ASSERT(item != NULL);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, item), 0.0);
    const char *ptr;
    size_t len;
    orderedIndexGetElementRaw(ops, item, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "key0", 4) == 0);
    orderedIndexFreeItem(ops, item);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 4);

    /* Pop again - should get score 1.0 */
    item = orderedIndexPopFirst(ops, idx);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, item), 1.0);
    orderedIndexFreeItem(ops, item);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 3);

    orderedIndexFree(ops, idx);
}

static void test_pop_last_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Pop from empty - should return NULL */
    TEST_ASSERT(orderedIndexPopLast(ops, idx) == NULL);

    /* Insert elements */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }
    TEST_ASSERT(orderedIndexLength(ops, idx) == 5);

    /* Pop last - should get score 4.0 */
    OrderedIndexItem *item = orderedIndexPopLast(ops, idx);
    TEST_ASSERT(item != NULL);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, item), 4.0);
    const char *ptr;
    size_t len;
    orderedIndexGetElementRaw(ops, item, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "key4", 4) == 0);
    orderedIndexFreeItem(ops, item);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 4);

    /* Pop again - should get score 3.0 */
    item = orderedIndexPopLast(ops, idx);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, item), 3.0);
    orderedIndexFreeItem(ops, item);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 3);

    orderedIndexFree(ops, idx);
}

static void test_update_score_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert elements */
    sds ele1 = sdsnew("key1");
    sds ele2 = sdsnew("key2");
    sds ele3 = sdsnew("key3");
    OrderedIndexItem *node1 = orderedIndexInsert(ops, idx, 1.0, ele1);
    OrderedIndexItem *node2 = orderedIndexInsert(ops, idx, 2.0, ele2);
    orderedIndexInsert(ops, idx, 3.0, ele3);
    sdsfree(ele1);
    sdsfree(ele2);
    sdsfree(ele3);

    /* Update middle element to move it to end */
    OrderedIndexItem *updated = orderedIndexUpdateScore(ops, idx, node2, 4.0);
    TEST_ASSERT(updated != NULL);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, updated), 4.0);
    const char *ptr;
    size_t len;
    orderedIndexGetElementRaw(ops, updated, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "key2", 4) == 0);

    /* Verify order: key1(1.0), key3(3.0), key2(4.0) */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 3.0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 4.0);
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "key2", 4) == 0);
    orderedIndexResetIterator(ops, &iter);

    /* Update to same score (no-op) */
    updated = orderedIndexUpdateScore(ops, idx, node1, 1.0);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, updated), 1.0);

    orderedIndexFree(ops, idx);
}

static void test_delete_range_by_score_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert 10 elements with scores 0-9 */
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    /* Delete range [3, 6] inclusive */
    unsigned long deleted = orderedIndexDeleteRangeByScore(ops, idx, 3.0, 6.0, 0, 0);
    TEST_ASSERT(deleted == 4); /* 3, 4, 5, 6 */
    TEST_ASSERT(orderedIndexLength(ops, idx) == 6);

    /* Verify remaining: 0,1,2,7,8,9 */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), (double)i);
    }
    for (int i = 7; i < 10; i++) {
        TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), (double)i);
    }
    orderedIndexResetIterator(ops, &iter);

    /* Delete with exclusive bounds (2, 8) - should delete 7 */
    deleted = orderedIndexDeleteRangeByScore(ops, idx, 2.0, 8.0, 1, 1);
    TEST_ASSERT(deleted == 1);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 5);

    orderedIndexFree(ops, idx);
}

static void test_delete_range_by_rank_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert 10 elements */
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    /* Delete ranks 3-5 (1-based, so elements at scores 2,3,4) */
    unsigned long deleted = orderedIndexDeleteRangeByRank(ops, idx, 3, 5);
    TEST_ASSERT(deleted == 3);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 7);

    /* Verify first element is still score 0 */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 0.0);
    orderedIndexResetIterator(ops, &iter);

    /* Verify rank 3 is now score 5 (was rank 6) */
    OrderedIndexItem *node = orderedIndexGetByRank(ops, idx, 3);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, node), 5.0);

    orderedIndexFree(ops, idx);
}

static void test_mixed_operations_rank_integrity_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);
    OrderedIndexItem *nodes[100];

    /* Insert 100 elements */
    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        nodes[i] = orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    /* Delete every 3rd element */
    for (int i = 2; i < 100; i += 3) {
        orderedIndexDelete(ops, idx, nodes[i]);
        nodes[i] = NULL;
    }

    /* Update scores of some remaining elements */
    if (nodes[10]) nodes[10] = orderedIndexUpdateScore(ops, idx, nodes[10], 150.0);
    if (nodes[20]) nodes[20] = orderedIndexUpdateScore(ops, idx, nodes[20], 160.0);

    /* Verify all ranks are correct by forward traversal */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    unsigned long expected_rank = 1;
    while (orderedIndexNext(ops, &iter, &pos)) {
        unsigned long actual_rank = orderedIndexGetRank(ops, idx, pos);
        TEST_ASSERT(actual_rank == expected_rank);
        expected_rank++;
    }
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_backward_traversal_after_deletions_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);
    OrderedIndexItem *nodes[20];

    /* Insert 20 elements */
    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        nodes[i] = orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    /* Delete elements at positions 5, 10, 15 */
    orderedIndexDelete(ops, idx, nodes[5]);
    orderedIndexDelete(ops, idx, nodes[10]);
    orderedIndexDelete(ops, idx, nodes[15]);

    /* Traverse backward and verify prev() pointers work correctly */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    int expected_scores[] = {19, 18, 17, 16, 14, 13, 12, 11, 9, 8, 7, 6, 4, 3, 2, 1, 0};
    int idx_score = 0;

    while (orderedIndexPrev(ops, &iter, &pos)) {
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), (double)expected_scores[idx_score]);
        idx_score++;
    }
    TEST_ASSERT(idx_score == 17); /* Should have traversed all 17 remaining elements */
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_lexicographic_edge_cases_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Test empty string */
    sds empty = sdsnew("");
    sds a = sdsnew("a");
    sds z = sdsnew("z");

    orderedIndexInsert(ops, idx, 1.0, z);
    orderedIndexInsert(ops, idx, 1.0, empty);
    orderedIndexInsert(ops, idx, 1.0, a);

    /* Verify lexicographic order: "", "a", "z" */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    const char *ptr;
    size_t len;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 1 && memcmp(ptr, "a", 1) == 0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 1 && memcmp(ptr, "z", 1) == 0);
    orderedIndexResetIterator(ops, &iter);

    sdsfree(empty);
    sdsfree(a);
    sdsfree(z);
    orderedIndexFree(ops, idx);

    /* Test very long string (1KB) */
    idx = orderedIndexCreate(ops);
    char long_buf[1024];
    memset(long_buf, 'x', 1023);
    long_buf[1023] = '\0';
    sds long_str = sdsnew(long_buf);
    sds short_str = sdsnew("short");

    orderedIndexInsert(ops, idx, 1.0, long_str);
    orderedIndexInsert(ops, idx, 1.0, short_str);

    /* Verify ordering */
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 5 && memcmp(ptr, "short", 5) == 0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 1023 && memcmp(ptr, long_buf, 1023) == 0);
    orderedIndexResetIterator(ops, &iter);

    sdsfree(long_str);
    sdsfree(short_str);
    orderedIndexFree(ops, idx);
}

static void test_range_boundary_precision_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert elements with very close scores */
    double base = 1.0;
    double epsilon = 1e-10;

    sds ele1 = sdsnew("at_base");
    sds ele2 = sdsnew("at_base_plus_epsilon");
    sds ele3 = sdsnew("at_base_plus_2epsilon");

    orderedIndexInsert(ops, idx, base, ele1);
    orderedIndexInsert(ops, idx, base + epsilon, ele2);
    orderedIndexInsert(ops, idx, base + 2 * epsilon, ele3);

    /* Query with exclusive bounds (base, base+2*epsilon) - should only get middle element */
    unsigned long deleted = orderedIndexDeleteRangeByScore(ops, idx, base, base + 2 * epsilon, 1, 1);
    TEST_ASSERT(deleted == 1);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 2);

    /* Verify remaining elements */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), base);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), base + 2 * epsilon);
    orderedIndexResetIterator(ops, &iter);

    sdsfree(ele1);
    sdsfree(ele2);
    sdsfree(ele3);
    orderedIndexFree(ops, idx);
}

static void test_special_double_values_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);
    const char *ptr;
    size_t len;

    /* Test ±infinity */
    sds neg_inf = sdsnew("neg_inf");
    sds pos_inf = sdsnew("pos_inf");
    sds zero = sdsnew("zero");
    sds one = sdsnew("one");

    orderedIndexInsert(ops, idx, NEG_INF, neg_inf);
    orderedIndexInsert(ops, idx, POS_INF, pos_inf);
    orderedIndexInsert(ops, idx, 0.0, zero);
    orderedIndexInsert(ops, idx, 1.0, one);

    /* Verify ordering: -inf, 0, 1, +inf */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), NEG_INF);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 0.0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), POS_INF);
    orderedIndexResetIterator(ops, &iter);

    sdsfree(neg_inf);
    sdsfree(pos_inf);
    sdsfree(zero);
    sdsfree(one);
    orderedIndexFree(ops, idx);

    /* Test +0.0 vs -0.0 (should be treated as equal) */
    idx = orderedIndexCreate(ops);
    sds pos_zero = sdsnew("pos_zero");
    sds neg_zero = sdsnew("neg_zero");

    orderedIndexInsert(ops, idx, 0.0, pos_zero);
    orderedIndexInsert(ops, idx, -0.0, neg_zero);

    /* Both should be in the list, ordered lexicographically since scores are equal */
    TEST_ASSERT(orderedIndexLength(ops, idx) == 2);
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 8 && memcmp(ptr, "neg_zero", 8) == 0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 8 && memcmp(ptr, "pos_zero", 8) == 0);
    orderedIndexResetIterator(ops, &iter);

    sdsfree(pos_zero);
    sdsfree(neg_zero);
    orderedIndexFree(ops, idx);

    /* Test denormalized double (very small number near zero) */
    idx = orderedIndexCreate(ops);
    double denorm = 1e-320; /* Denormalized double */
    sds denorm_ele = sdsnew("denorm");
    sds normal_ele = sdsnew("normal");

    orderedIndexInsert(ops, idx, denorm, denorm_ele);
    orderedIndexInsert(ops, idx, 1.0, normal_ele);

    /* Verify denormalized value is handled correctly */
    TEST_ASSERT(orderedIndexLength(ops, idx) == 2);
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), denorm);
    TEST_ASSERT(orderedIndexGetScore(ops, pos) < 1.0);
    orderedIndexResetIterator(ops, &iter);

    sdsfree(denorm_ele);
    sdsfree(normal_ele);
    orderedIndexFree(ops, idx);
}


static void test_edge_cases_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Empty index operations */
    TEST_ASSERT(orderedIndexLength(ops, idx) == 0);
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT(!orderedIndexPrev(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);
    TEST_ASSERT(orderedIndexGetByRank(ops, idx, 1) == NULL);

    orderedIndexFree(ops, idx);
}

static void test_delete_edge_cases_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Delete only element */
    sds ele = sdsnew("only");
    OrderedIndexItem *node = orderedIndexInsert(ops, idx, 1.0, ele);
    orderedIndexDelete(ops, idx, node);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 0);
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);
    sdsfree(ele);

    /* Delete first element */
    OrderedIndexItem *nodes[3];
    for (int i = 0; i < 3; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds e = sdsnew(buf);
        nodes[i] = orderedIndexInsert(ops, idx, (double)i, e);
        sdsfree(e);
    }
    orderedIndexDelete(ops, idx, nodes[0]);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 2);
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    orderedIndexResetIterator(ops, &iter);

    /* Delete last element */
    orderedIndexDelete(ops, idx, nodes[2]);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 1);
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_rank_edge_cases_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert 5 elements */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    /* Rank 0 returns header node (not NULL, not first element - implementation quirk)
     * We don't test this as it's an implementation detail that shouldn't be relied upon */

    /* Rank beyond length returns NULL */
    TEST_ASSERT(orderedIndexGetByRank(ops, idx, 6) == NULL);
    TEST_ASSERT(orderedIndexGetByRank(ops, idx, 100) == NULL);

    /* Valid boundary ranks */
    TEST_ASSERT(orderedIndexGetByRank(ops, idx, 1) != NULL);
    TEST_ASSERT(orderedIndexGetByRank(ops, idx, 5) != NULL);

    orderedIndexFree(ops, idx);
}

static void test_duplicate_insert_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert same score+element twice */
    sds ele1 = sdsnew("duplicate");
    sds ele2 = sdsnew("duplicate");
    OrderedIndexItem *node1 = orderedIndexInsert(ops, idx, 1.0, ele1);
    OrderedIndexItem *node2 = orderedIndexInsert(ops, idx, 1.0, ele2);

    /* Should have 2 nodes (duplicates allowed) */
    TEST_ASSERT(orderedIndexLength(ops, idx) == 2);
    TEST_ASSERT(node1 != node2);

    sdsfree(ele1);
    sdsfree(ele2);
    orderedIndexFree(ops, idx);
}

static void test_update_score_edge_cases_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert elements */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    /* Update first element to move backward (should stay first) */
    OrderedIndexItem *first = orderedIndexGetByRank(ops, idx, 1);
    OrderedIndexItem *updated = orderedIndexUpdateScore(ops, idx, first, -1.0);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, updated), -1.0);
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT(pos == updated);
    orderedIndexResetIterator(ops, &iter);

    /* Update last element to move forward */
    unsigned long len = orderedIndexLength(ops, idx);
    OrderedIndexItem *last = orderedIndexGetByRank(ops, idx, len);
    updated = orderedIndexUpdateScore(ops, idx, last, 10.0);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, updated), 10.0);
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
    TEST_ASSERT(pos == updated);
    orderedIndexResetIterator(ops, &iter);

    /* Update middle element to move backward */
    OrderedIndexItem *middle = orderedIndexGetByRank(ops, idx, 3);
    double old_score = orderedIndexGetScore(ops, middle);
    updated = orderedIndexUpdateScore(ops, idx, middle, 0.5);
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, updated), 0.5);
    TEST_ASSERT(orderedIndexGetScore(ops, updated) < old_score);

    orderedIndexFree(ops, idx);
}

static void test_range_delete_edge_cases_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert 10 elements */
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    /* Delete empty range (min > max) */
    unsigned long deleted = orderedIndexDeleteRangeByScore(ops, idx, 5.0, 4.0, 0, 0);
    TEST_ASSERT(deleted == 0);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 10);

    /* Delete range with no matches */
    deleted = orderedIndexDeleteRangeByScore(ops, idx, 10.5, 11.5, 0, 0);
    TEST_ASSERT(deleted == 0);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 10);

    /* Delete first elements by rank */
    deleted = orderedIndexDeleteRangeByRank(ops, idx, 1, 2);
    TEST_ASSERT(deleted == 2);
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 2.0);
    orderedIndexResetIterator(ops, &iter);

    /* Delete last elements by rank */
    unsigned long len = orderedIndexLength(ops, idx);
    deleted = orderedIndexDeleteRangeByRank(ops, idx, len - 1, len);
    TEST_ASSERT(deleted == 2);
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 7.0);
    orderedIndexResetIterator(ops, &iter);

    /* Delete entire remaining index by score */
    deleted = orderedIndexDeleteRangeByScore(ops, idx, -100.0, 100.0, 0, 0);
    TEST_ASSERT(deleted == 6);
    TEST_ASSERT(orderedIndexLength(ops, idx) == 0);

    orderedIndexFree(ops, idx);
}


static void test_traversal_edge_cases_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert single element */
    sds ele = sdsnew("single");
    orderedIndexInsert(ops, idx, 1.0, ele);

    /* Iterator should get one element then return false */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    /* Same for prev */
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    TEST_ASSERT(!orderedIndexPrev(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    sdsfree(ele);
    orderedIndexFree(ops, idx);
}

static void test_seek_to_rank_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert 5 elements */
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Seek to rank 0 (before first) - next should return rank 1, prev should return NULL */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToRank(ops, &iter, 0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToRank(ops, &iter, 0);
    TEST_ASSERT(!orderedIndexPrev(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    /* Seek to rank 1 - next should return rank 2, prev should return rank 1 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToRank(ops, &iter, 1);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 2.0);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToRank(ops, &iter, 1);
    TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    orderedIndexResetIterator(ops, &iter);

    /* Seek to rank 3 (middle) - next should return rank 4, prev should return rank 3 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToRank(ops, &iter, 3);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 4.0);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToRank(ops, &iter, 3);
    TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 3.0);
    orderedIndexResetIterator(ops, &iter);

    /* Seek to rank 5 (last) - next should return NULL, prev should return rank 5 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToRank(ops, &iter, 5);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToRank(ops, &iter, 5);
    TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 5.0);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_reverse_iteration_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert 5 elements */
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Full reverse traversal */
    orderedIndexInitIterator(ops, &iter, idx);
    int count = 0;
    double expected = 5.0;
    while (orderedIndexPrev(ops, &iter, &pos)) {
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), expected);
        expected -= 1.0;
        count++;
    }
    TEST_ASSERT(count == 5);
    orderedIndexResetIterator(ops, &iter);

    /* Reverse then forward */
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 5.0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 5.0);
    orderedIndexResetIterator(ops, &iter);

    /* Forward then reverse */
    orderedIndexInitIterator(ops, &iter, idx);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    TEST_ASSERT(orderedIndexPrev(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 1.0);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_seek_to_score_range_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert elements with scores 0,2,4,6,8 */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)(i * 2), ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Seek to first in range [2, 6] with offset 0 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 2.0, 6.0, 0, 0, 0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 2.0);
    orderedIndexResetIterator(ops, &iter);

    /* Seek to second in range [2, 6] with offset 1 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 2.0, 6.0, 0, 0, 1);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 4.0);
    orderedIndexResetIterator(ops, &iter);

    /* Seek to last in range [2, 6] with offset -1 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 2.0, 6.0, 0, 0, -1);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 6.0);
    orderedIndexResetIterator(ops, &iter);

    /* Seek with exclusive bounds (2, 6) - should start at 4 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 2.0, 6.0, 1, 1, 0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 4.0);
    orderedIndexResetIterator(ops, &iter);

    /* Seek to empty range - should position at end */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 10.0, 20.0, 0, 0, 0);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    /* Out of range positive offset - should position at end */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 2.0, 6.0, 0, 0, 10);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    /* Negative offset beyond range - should position at end */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 2.0, 6.0, 0, 0, -10);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    /* Second from last with offset -2 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 2.0, 6.0, 0, 0, -2);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 4.0);
    orderedIndexResetIterator(ops, &iter);

    /* Empty range where min > max - should position at end */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 6.0, 2.0, 0, 0, 0);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

static void test_seek_to_score_range_iteration_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert elements with scores 0-9 */
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Seek to range [3, 7] and iterate forward */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 3.0, 7.0, 0, 0, 0);
    int count = 0;
    double expected = 3.0;
    while (orderedIndexNext(ops, &iter, &pos) && orderedIndexGetScore(ops, pos) <= 7.0) {
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), expected);
        expected += 1.0;
        count++;
    }
    TEST_ASSERT(count == 5); /* 3,4,5,6,7 */
    orderedIndexResetIterator(ops, &iter);

    /* Seek to last in range and iterate backward */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 3.0, 7.0, 0, 0, -1);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 7.0);
    /* Now go backward */
    count = 0;
    expected = 7.0;
    while (orderedIndexPrev(ops, &iter, &pos) && orderedIndexGetScore(ops, pos) >= 3.0) {
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), expected);
        expected -= 1.0;
        count++;
    }
    TEST_ASSERT(count == 5); /* 7,6,5,4,3 */
    orderedIndexResetIterator(ops, &iter);

    /* Seek with offset and continue iteration */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, 2.0, 8.0, 0, 0, 2); /* Start at 4 (3rd element) */
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 4.0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 5.0);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

/* Test ZREVRANGEBYSCORE +inf behavior: seek to last element and iterate backwards.
 * This is the pattern used by ZREVRANGEBYSCORE -inf +inf to get all elements in reverse. */
static void test_seek_inf_reverse_iteration_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert elements with scores 1-5 */
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Seek to [-inf, +inf] with offset -1 (last element), then iterate backwards.
     * This is how ZREVRANGEBYSCORE -inf +inf works. */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, NEG_INF, POS_INF, 0, 0, -1);
    int count = 0;
    double expected = 5.0;
    while (orderedIndexNext(ops, &iter, &pos)) {
        if (count == 0) {
            TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), 5.0);
        }
        count++;
        break; /* Just verify first element is correct */
    }
    /* Now iterate backwards through all elements */
    count = 0;
    expected = 5.0;
    while (orderedIndexPrev(ops, &iter, &pos)) {
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), expected);
        expected -= 1.0;
        count++;
    }
    TEST_ASSERT(count == 5);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

/* Test ZRANGEBYSCORE -inf behavior: seek to first element and iterate forwards.
 * This is the pattern used by ZRANGEBYSCORE -inf +inf to get all elements. */
static void test_seek_inf_forward_iteration_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert elements with scores 1-5 */
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        orderedIndexInsert(ops, idx, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Seek to [-inf, +inf] with offset 0 (first element), then iterate forwards.
     * This is how ZRANGEBYSCORE -inf +inf works. */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToScoreRange(ops, &iter, NEG_INF, POS_INF, 0, 0, 0);
    int count = 0;
    double expected = 1.0;
    while (orderedIndexNext(ops, &iter, &pos)) {
        TEST_ASSERT_SCORE_EQ(orderedIndexGetScore(ops, pos), expected);
        expected += 1.0;
        count++;
    }
    TEST_ASSERT(count == 5);
    orderedIndexResetIterator(ops, &iter);

    orderedIndexFree(ops, idx);
}

/* Test seek_to_lex_range for ZRANGEBYLEX-style operations.
 * Lex range is meaningful when all elements have the same score. */
static void test_seek_to_lex_range_generic(const OrderedIndexOps *ops) {
    OrderedIndex *idx = orderedIndexCreate(ops);

    /* Insert elements with same score, different lex values */
    const char *elements[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (int i = 0; i < 5; i++) {
        sds ele = sdsnew(elements[i]);
        orderedIndexInsert(ops, idx, 1.0, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    const char *ptr;
    size_t len;

    sds minLex = sdsnew("banana");
    sds maxLex = sdsnew("date");

    /* Seek to first in lex range [banana, date] with offset 0 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToLexRange(ops, &iter, minLex, maxLex, 0, 0, 0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 6 && memcmp(ptr, "banana", 6) == 0);
    orderedIndexResetIterator(ops, &iter);

    /* Seek to second in lex range [banana, date] with offset 1 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToLexRange(ops, &iter, minLex, maxLex, 0, 0, 1);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 6 && memcmp(ptr, "cherry", 6) == 0);
    orderedIndexResetIterator(ops, &iter);

    /* Seek to last in lex range [banana, date] with offset -1 */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToLexRange(ops, &iter, minLex, maxLex, 0, 0, -1);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "date", 4) == 0);
    orderedIndexResetIterator(ops, &iter);

    /* Seek with exclusive bounds (banana, date) - should start at cherry */
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToLexRange(ops, &iter, minLex, maxLex, 1, 1, 0);
    TEST_ASSERT(orderedIndexNext(ops, &iter, &pos));
    orderedIndexGetElementRaw(ops, pos, &ptr, &len);
    TEST_ASSERT(len == 6 && memcmp(ptr, "cherry", 6) == 0);
    orderedIndexResetIterator(ops, &iter);

    sdsfree(minLex);
    sdsfree(maxLex);

    /* Seek to empty lex range - should position at end */
    sds minEmpty = sdsnew("zzz");
    sds maxEmpty = sdsnew("zzzz");
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToLexRange(ops, &iter, minEmpty, maxEmpty, 0, 0, 0);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);
    sdsfree(minEmpty);
    sdsfree(maxEmpty);

    /* Out of range positive offset - should position at end */
    minLex = sdsnew("banana");
    maxLex = sdsnew("date");
    orderedIndexInitIterator(ops, &iter, idx);
    orderedIndexSeekToLexRange(ops, &iter, minLex, maxLex, 0, 0, 10);
    TEST_ASSERT(!orderedIndexNext(ops, &iter, &pos));
    orderedIndexResetIterator(ops, &iter);
    sdsfree(minLex);
    sdsfree(maxLex);

    orderedIndexFree(ops, idx);
}

/* ========== Skiplist Tests ========== */

class SkiplistOrderedIndexTest : public ::testing::Test {};

TEST_F(SkiplistOrderedIndexTest, CreateFree) {
    test_create_free_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, InsertSingle) {
    test_insert_single_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, InsertMultipleOrdered) {
    test_insert_multiple_ordered_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, DuplicateScores) {
    test_duplicate_scores_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, RankOperations) {
    test_rank_operations_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, Delete) {
    test_delete_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, PopFirst) {
    test_pop_first_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, PopLast) {
    test_pop_last_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, UpdateScore) {
    test_update_score_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, DeleteRangeByScore) {
    test_delete_range_by_score_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, DeleteRangeByRank) {
    test_delete_range_by_rank_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, EdgeCases) {
    test_edge_cases_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, DeleteEdgeCases) {
    test_delete_edge_cases_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, RankEdgeCases) {
    test_rank_edge_cases_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, DuplicateInsert) {
    test_duplicate_insert_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, UpdateScoreEdgeCases) {
    test_update_score_edge_cases_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, RangeDeleteEdgeCases) {
    test_range_delete_edge_cases_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, TraversalEdgeCases) {
    test_traversal_edge_cases_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, MixedOperationsRankIntegrity) {
    test_mixed_operations_rank_integrity_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, BackwardTraversalAfterDeletions) {
    test_backward_traversal_after_deletions_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, LexicographicEdgeCases) {
    test_lexicographic_edge_cases_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, RangeBoundaryPrecision) {
    test_range_boundary_precision_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, SpecialDoubleValues) {
    test_special_double_values_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, SeekToRank) {
    test_seek_to_rank_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, ReverseIteration) {
    test_reverse_iteration_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, SeekToScoreRange) {
    test_seek_to_score_range_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, SeekToScoreRangeIteration) {
    test_seek_to_score_range_iteration_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, SeekInfReverseIteration) {
    test_seek_inf_reverse_iteration_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, SeekInfForwardIteration) {
    test_seek_inf_forward_iteration_generic(&skiplistOrderedIndexOps);
}
TEST_F(SkiplistOrderedIndexTest, SeekToLexRange) {
    test_seek_to_lex_range_generic(&skiplistOrderedIndexOps);
}
