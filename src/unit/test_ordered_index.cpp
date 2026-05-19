/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
}

/* Undefine min/max macros from server.h to avoid conflicts with C++ standard library */
#undef min
#undef max

#include "ordered_index_test.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

#define TEST_ASSERT(x) ASSERT_TRUE(x)
#define TEST_ASSERT_SCORE_EQ(a, b) ASSERT_DOUBLE_EQ(a, b)

/* Verify structural integrity of the ordered index after mutations. */
static ::testing::AssertionResult verifyIntegrity(OrderedIndexTestApi &api, OrderedIndex *oi) {
    char errmsg[256];
    if (api.verifyIntegrity(oi, errmsg, sizeof(errmsg)))
        return ::testing::AssertionResult(true);
    return ::testing::AssertionFailure() << errmsg;
}

#define VERIFY_INTEGRITY(api_ref, idx_ptr) ASSERT_TRUE(verifyIntegrity(api_ref, idx_ptr))

/* Use double infinity to avoid -Wdouble-promotion on macOS where INFINITY is float */
static const double POS_INF = (double)INFINITY;
static const double NEG_INF = (double)-INFINITY;

/* ========== Parameterized test fixture ========== */

class OrderedIndexTest : public ::testing::TestWithParam<OrderedIndexTestApi *> {
  protected:
    OrderedIndexTestApi &api = *GetParam();
};

/* ========== Basic tests ========== */

TEST_P(OrderedIndexTest, CreateFree) {
    OrderedIndex *oi = api.create();
    TEST_ASSERT(oi != NULL);
    TEST_ASSERT(api.length(oi) == 0);
    VERIFY_INTEGRITY(api, oi);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, InsertSingle) {
    OrderedIndex *oi = api.create();
    sds ele = sdsnew("test");
    OrderedIndexItem *node = api.insertSds(oi, 1.0, ele);
    VERIFY_INTEGRITY(api, oi);

    TEST_ASSERT(node != NULL);
    TEST_ASSERT(api.length(oi) == 1);
    TEST_ASSERT_SCORE_EQ(api.getScore(node), 1.0);

    const char *ptr;
    size_t len;
    api.getElementRaw(node, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "test", 4) == 0);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT(pos == node);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    sdsfree(ele);
    api.free(oi);
}

TEST_P(OrderedIndexTest, InsertMultipleOrdered) {
    OrderedIndex *oi = api.create();

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    TEST_ASSERT(api.length(oi) == 10);
    VERIFY_INTEGRITY(api, oi);

    /* Verify forward traversal */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT((pos = api.next(&iter)) != NULL);
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), (double)i);
    }
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    /* Verify backward traversal */
    api.initIterator(&iter, oi);
    for (int i = 9; i >= 0; i--) {
        TEST_ASSERT((pos = api.prev(&iter)) != NULL);
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), (double)i);
    }
    TEST_ASSERT((pos = api.prev(&iter)) == NULL);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, DuplicateScores) {
    OrderedIndex *oi = api.create();

    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, 1.0, ele);
        sdsfree(ele);
    }

    TEST_ASSERT(api.length(oi) == 5);
    VERIFY_INTEGRITY(api, oi);

    /* Verify lexicographic ordering for same scores */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT((pos = api.next(&iter)) != NULL);
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
        const char *ptr;
        size_t len;
        api.getElementRaw(pos, &ptr, &len);
        char expected[32];
        snprintf(expected, sizeof(expected), "key%d", i);
        TEST_ASSERT(len == strlen(expected) && memcmp(ptr, expected, len) == 0);
    }
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, RankOperations) {
    OrderedIndex *oi = api.create();
    OrderedIndexItem *nodes[10];

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        nodes[i] = api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }
    VERIFY_INTEGRITY(api, oi);

    for (int i = 0; i < 10; i++) {
        unsigned long rank = api.getRank(oi, nodes[i]);
        TEST_ASSERT(rank == (unsigned long)(i + 1)); /* 1-based */
    }

    for (int i = 0; i < 10; i++) {
        OrderedIndexItem *node = api.getByRank(oi, i + 1);
        TEST_ASSERT(node == nodes[i]);
    }

    api.free(oi);
}

TEST_P(OrderedIndexTest, Delete) {
    OrderedIndex *oi = api.create();
    OrderedIndexItem *nodes[5];

    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        nodes[i] = api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    TEST_ASSERT(api.length(oi) == 5);

    api.deleteItem(oi, nodes[2]);
    TEST_ASSERT(api.length(oi) == 4);
    VERIFY_INTEGRITY(api, oi);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 0.0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 3.0); /* Skipped 2.0 */
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 4.0);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, PopFirst) {
    OrderedIndex *oi = api.create();

    TEST_ASSERT(api.popFirst(oi) == NULL);

    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }
    TEST_ASSERT(api.length(oi) == 5);

    OrderedIndexItem *item = api.popFirst(oi);
    TEST_ASSERT(item != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(item), 0.0);
    const char *ptr;
    size_t len;
    api.getElementRaw(item, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "key0", 4) == 0);
    api.freeItem(item);
    TEST_ASSERT(api.length(oi) == 4);
    VERIFY_INTEGRITY(api, oi);

    item = api.popFirst(oi);
    TEST_ASSERT_SCORE_EQ(api.getScore(item), 1.0);
    api.freeItem(item);
    TEST_ASSERT(api.length(oi) == 3);
    VERIFY_INTEGRITY(api, oi);

    api.free(oi);
}

TEST_P(OrderedIndexTest, PopLast) {
    OrderedIndex *oi = api.create();

    TEST_ASSERT(api.popLast(oi) == NULL);

    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }
    TEST_ASSERT(api.length(oi) == 5);

    OrderedIndexItem *item = api.popLast(oi);
    TEST_ASSERT(item != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(item), 4.0);
    const char *ptr;
    size_t len;
    api.getElementRaw(item, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "key4", 4) == 0);
    api.freeItem(item);
    TEST_ASSERT(api.length(oi) == 4);
    VERIFY_INTEGRITY(api, oi);

    item = api.popLast(oi);
    TEST_ASSERT_SCORE_EQ(api.getScore(item), 3.0);
    api.freeItem(item);
    TEST_ASSERT(api.length(oi) == 3);
    VERIFY_INTEGRITY(api, oi);

    api.free(oi);
}

TEST_P(OrderedIndexTest, UpdateScore) {
    OrderedIndex *oi = api.create();

    sds ele1 = sdsnew("key1");
    sds ele2 = sdsnew("key2");
    sds ele3 = sdsnew("key3");
    OrderedIndexItem *node1 = api.insertSds(oi, 1.0, ele1);
    OrderedIndexItem *node2 = api.insertSds(oi, 2.0, ele2);
    api.insertSds(oi, 3.0, ele3);
    sdsfree(ele1);
    sdsfree(ele2);
    sdsfree(ele3);

    OrderedIndexItem *updated = api.updateScore(oi, node2, 4.0);
    TEST_ASSERT(updated != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(updated), 4.0);
    VERIFY_INTEGRITY(api, oi);
    const char *ptr;
    size_t len;
    api.getElementRaw(updated, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "key2", 4) == 0);

    /* Verify order: key1(1.0), key3(3.0), key2(4.0) */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 3.0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 4.0);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "key2", 4) == 0);
    api.resetIterator(&iter);

    /* Update to same score (no-op) */
    updated = api.updateScore(oi, node1, 1.0);
    TEST_ASSERT_SCORE_EQ(api.getScore(updated), 1.0);
    VERIFY_INTEGRITY(api, oi);

    api.free(oi);
}

TEST_P(OrderedIndexTest, DeleteRangeByScore) {
    OrderedIndex *oi = api.create();

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    /* Delete range [3, 6] inclusive */
    unsigned long deleted = api.deleteRangeByScore(oi, 3.0, 6.0, 0, 0, NULL, NULL);
    TEST_ASSERT(deleted == 4); /* 3, 4, 5, 6 */
    TEST_ASSERT(api.length(oi) == 6);
    VERIFY_INTEGRITY(api, oi);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT((pos = api.next(&iter)) != NULL);
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), (double)i);
    }
    for (int i = 7; i < 10; i++) {
        TEST_ASSERT((pos = api.next(&iter)) != NULL);
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), (double)i);
    }
    api.resetIterator(&iter);

    /* Delete with exclusive bounds (2, 8) - should delete 7 */
    deleted = api.deleteRangeByScore(oi, 2.0, 8.0, 1, 1, NULL, NULL);
    TEST_ASSERT(deleted == 1);
    TEST_ASSERT(api.length(oi) == 5);
    VERIFY_INTEGRITY(api, oi);

    api.free(oi);
}

TEST_P(OrderedIndexTest, DeleteRangeByRank) {
    OrderedIndex *oi = api.create();

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    /* Delete ranks 3-5 (1-based, so elements at scores 2,3,4) */
    unsigned long deleted = api.deleteRangeByRank(oi, 3, 5, NULL, NULL);
    TEST_ASSERT(deleted == 3);
    TEST_ASSERT(api.length(oi) == 7);
    VERIFY_INTEGRITY(api, oi);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 0.0);
    api.resetIterator(&iter);

    /* Verify rank 3 is now score 5 (was rank 6) */
    OrderedIndexItem *node = api.getByRank(oi, 3);
    TEST_ASSERT_SCORE_EQ(api.getScore(node), 5.0);

    api.free(oi);
}

TEST_P(OrderedIndexTest, MixedOperationsRankIntegrity) {
    OrderedIndex *oi = api.create();
    OrderedIndexItem *nodes[100];

    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        nodes[i] = api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    for (int i = 2; i < 100; i += 3) {
        api.deleteItem(oi, nodes[i]);
        nodes[i] = NULL;
    }
    VERIFY_INTEGRITY(api, oi);

    if (nodes[10]) nodes[10] = api.updateScore(oi, nodes[10], 150.0);
    if (nodes[20]) nodes[20] = api.updateScore(oi, nodes[20], 160.0);
    VERIFY_INTEGRITY(api, oi);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    unsigned long expected_rank = 1;
    while (((pos = api.next(&iter)) != NULL)) {
        unsigned long actual_rank = api.getRank(oi, pos);
        TEST_ASSERT(actual_rank == expected_rank);
        expected_rank++;
    }
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, BackwardTraversalAfterDeletions) {
    OrderedIndex *oi = api.create();
    OrderedIndexItem *nodes[20];

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        nodes[i] = api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    api.deleteItem(oi, nodes[5]);
    api.deleteItem(oi, nodes[10]);
    api.deleteItem(oi, nodes[15]);
    VERIFY_INTEGRITY(api, oi);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    int expected_scores[] = {19, 18, 17, 16, 14, 13, 12, 11, 9, 8, 7, 6, 4, 3, 2, 1, 0};
    int idx_score = 0;

    while (((pos = api.prev(&iter)) != NULL)) {
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), (double)expected_scores[idx_score]);
        idx_score++;
    }
    TEST_ASSERT(idx_score == 17); /* Should have traversed all 17 remaining elements */
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, LexicographicEdgeCases) {
    OrderedIndex *oi = api.create();

    sds empty = sdsnew("");
    sds a = sdsnew("a");
    sds z = sdsnew("z");

    api.insertSds(oi, 1.0, z);
    api.insertSds(oi, 1.0, empty);
    api.insertSds(oi, 1.0, a);

    /* Verify lexicographic order: "", "a", "z" */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    const char *ptr;
    size_t len;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 1 && memcmp(ptr, "a", 1) == 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 1 && memcmp(ptr, "z", 1) == 0);
    api.resetIterator(&iter);

    sdsfree(empty);
    sdsfree(a);
    sdsfree(z);
    api.free(oi);

    /* Test very long string (1KB) */
    oi = api.create();
    char long_buf[1024];
    memset(long_buf, 'x', 1023);
    long_buf[1023] = '\0';
    sds long_str = sdsnew(long_buf);
    sds short_str = sdsnew("short");

    api.insertSds(oi, 1.0, long_str);
    api.insertSds(oi, 1.0, short_str);

    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 5 && memcmp(ptr, "short", 5) == 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 1023 && memcmp(ptr, long_buf, 1023) == 0);
    api.resetIterator(&iter);

    sdsfree(long_str);
    sdsfree(short_str);
    api.free(oi);
}

TEST_P(OrderedIndexTest, RangeBoundaryPrecision) {
    OrderedIndex *oi = api.create();

    double base = 1.0;
    double epsilon = 1e-10;

    sds ele1 = sdsnew("at_base");
    sds ele2 = sdsnew("at_base_plus_epsilon");
    sds ele3 = sdsnew("at_base_plus_2epsilon");

    api.insertSds(oi, base, ele1);
    api.insertSds(oi, base + epsilon, ele2);
    api.insertSds(oi, base + 2 * epsilon, ele3);

    unsigned long deleted = api.deleteRangeByScore(oi, base, base + 2 * epsilon, 1, 1, NULL, NULL);
    TEST_ASSERT(deleted == 1);
    TEST_ASSERT(api.length(oi) == 2);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), base);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), base + 2 * epsilon);
    api.resetIterator(&iter);

    sdsfree(ele1);
    sdsfree(ele2);
    sdsfree(ele3);
    api.free(oi);
}

TEST_P(OrderedIndexTest, SpecialDoubleValues) {
    OrderedIndex *oi = api.create();
    const char *ptr;
    size_t len;

    sds neg_inf = sdsnew("neg_inf");
    sds pos_inf = sdsnew("pos_inf");
    sds zero = sdsnew("zero");
    sds one = sdsnew("one");

    api.insertSds(oi, NEG_INF, neg_inf);
    api.insertSds(oi, POS_INF, pos_inf);
    api.insertSds(oi, 0.0, zero);
    api.insertSds(oi, 1.0, one);

    /* Verify ordering: -inf, 0, 1, +inf */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), NEG_INF);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 0.0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), POS_INF);
    api.resetIterator(&iter);

    sdsfree(neg_inf);
    sdsfree(pos_inf);
    sdsfree(zero);
    sdsfree(one);
    api.free(oi);

    /* Test +0.0 vs -0.0 */
    oi = api.create();
    sds pos_zero = sdsnew("pos_zero");
    sds neg_zero = sdsnew("neg_zero");

    api.insertSds(oi, 0.0, pos_zero);
    api.insertSds(oi, -0.0, neg_zero);

    /* Both should be in the list, ordered lexicographically since scores are equal */
    TEST_ASSERT(api.length(oi) == 2);
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 8 && memcmp(ptr, "neg_zero", 8) == 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 8 && memcmp(ptr, "pos_zero", 8) == 0);
    api.resetIterator(&iter);

    sdsfree(pos_zero);
    sdsfree(neg_zero);
    api.free(oi);

    /* Test denormalized double */
    oi = api.create();
    double denorm = 1e-320; /* Denormalized double */
    sds denorm_ele = sdsnew("denorm");
    sds normal_ele = sdsnew("normal");

    api.insertSds(oi, denorm, denorm_ele);
    api.insertSds(oi, 1.0, normal_ele);

    TEST_ASSERT(api.length(oi) == 2);
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), denorm);
    TEST_ASSERT(api.getScore(pos) < 1.0);
    api.resetIterator(&iter);

    sdsfree(denorm_ele);
    sdsfree(normal_ele);
    api.free(oi);
}

TEST_P(OrderedIndexTest, EdgeCases) {
    OrderedIndex *oi = api.create();

    TEST_ASSERT(api.length(oi) == 0);
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    TEST_ASSERT((pos = api.prev(&iter)) == NULL);
    api.resetIterator(&iter);
    TEST_ASSERT(api.getByRank(oi, 1) == NULL);

    api.free(oi);
}

TEST_P(OrderedIndexTest, DeleteEdgeCases) {
    OrderedIndex *oi = api.create();

    /* Delete only element */
    sds ele = sdsnew("only");
    OrderedIndexItem *node = api.insertSds(oi, 1.0, ele);
    api.deleteItem(oi, node);
    TEST_ASSERT(api.length(oi) == 0);
    VERIFY_INTEGRITY(api, oi);
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);
    sdsfree(ele);

    /* Delete first element */
    OrderedIndexItem *nodes[3];
    for (int i = 0; i < 3; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds e = sdsnew(buf);
        nodes[i] = api.insertSds(oi, (double)i, e);
        sdsfree(e);
    }
    api.deleteItem(oi, nodes[0]);
    TEST_ASSERT(api.length(oi) == 2);
    VERIFY_INTEGRITY(api, oi);
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    api.resetIterator(&iter);

    /* Delete last element */
    api.deleteItem(oi, nodes[2]);
    TEST_ASSERT(api.length(oi) == 1);
    VERIFY_INTEGRITY(api, oi);
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, RankEdgeCases) {
    OrderedIndex *oi = api.create();

    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    TEST_ASSERT(api.getByRank(oi, 6) == NULL);
    TEST_ASSERT(api.getByRank(oi, 100) == NULL);
    TEST_ASSERT(api.getByRank(oi, 1) != NULL);
    TEST_ASSERT(api.getByRank(oi, 5) != NULL);

    api.free(oi);
}

TEST_P(OrderedIndexTest, DuplicateInsert) {
    OrderedIndex *oi = api.create();

    sds ele1 = sdsnew("duplicate");
    sds ele2 = sdsnew("duplicate");
    OrderedIndexItem *node1 = api.insertSds(oi, 1.0, ele1);
    OrderedIndexItem *node2 = api.insertSds(oi, 1.0, ele2);

    /* Should have 2 nodes (duplicates allowed) */
    TEST_ASSERT(api.length(oi) == 2);
    TEST_ASSERT(node1 != node2);

    sdsfree(ele1);
    sdsfree(ele2);
    api.free(oi);
}

TEST_P(OrderedIndexTest, UpdateScoreEdgeCases) {
    OrderedIndex *oi = api.create();

    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    /* Update first element to move backward */
    OrderedIndexItem *first = api.getByRank(oi, 1);
    OrderedIndexItem *updated = api.updateScore(oi, first, -1.0);
    TEST_ASSERT_SCORE_EQ(api.getScore(updated), -1.0);
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT(pos == updated);
    api.resetIterator(&iter);

    /* Update last element to move forward */
    unsigned long len = api.length(oi);
    OrderedIndexItem *last = api.getByRank(oi, len);
    updated = api.updateScore(oi, last, 10.0);
    TEST_ASSERT_SCORE_EQ(api.getScore(updated), 10.0);
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT(pos == updated);
    api.resetIterator(&iter);

    /* Update middle element to move backward */
    OrderedIndexItem *middle = api.getByRank(oi, 3);
    double old_score = api.getScore(middle);
    updated = api.updateScore(oi, middle, 0.5);
    TEST_ASSERT_SCORE_EQ(api.getScore(updated), 0.5);
    TEST_ASSERT(api.getScore(updated) < old_score);

    api.free(oi);
}

TEST_P(OrderedIndexTest, RangeDeleteEdgeCases) {
    OrderedIndex *oi = api.create();

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    /* Delete empty range (min > max) */
    unsigned long deleted = api.deleteRangeByScore(oi, 5.0, 4.0, 0, 0, NULL, NULL);
    TEST_ASSERT(deleted == 0);
    TEST_ASSERT(api.length(oi) == 10);

    /* Delete range with no matches */
    deleted = api.deleteRangeByScore(oi, 10.5, 11.5, 0, 0, NULL, NULL);
    TEST_ASSERT(deleted == 0);
    TEST_ASSERT(api.length(oi) == 10);

    /* Delete first elements by rank */
    deleted = api.deleteRangeByRank(oi, 1, 2, NULL, NULL);
    TEST_ASSERT(deleted == 2);
    VERIFY_INTEGRITY(api, oi);
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 2.0);
    api.resetIterator(&iter);

    /* Delete last elements by rank */
    unsigned long len = api.length(oi);
    deleted = api.deleteRangeByRank(oi, len - 1, len, NULL, NULL);
    TEST_ASSERT(deleted == 2);
    VERIFY_INTEGRITY(api, oi);
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 7.0);
    api.resetIterator(&iter);

    /* Delete entire remaining index by score */
    deleted = api.deleteRangeByScore(oi, -100.0, 100.0, 0, 0, NULL, NULL);
    TEST_ASSERT(deleted == 6);
    TEST_ASSERT(api.length(oi) == 0);
    VERIFY_INTEGRITY(api, oi);

    api.free(oi);
}

TEST_P(OrderedIndexTest, TraversalEdgeCases) {
    OrderedIndex *oi = api.create();

    sds ele = sdsnew("single");
    api.insertSds(oi, 1.0, ele);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    TEST_ASSERT((pos = api.prev(&iter)) == NULL);
    api.resetIterator(&iter);

    sdsfree(ele);
    api.free(oi);
}

TEST_P(OrderedIndexTest, SeekToRank) {
    OrderedIndex *oi = api.create();

    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Seek to rank 0 (before first) */
    api.initIterator(&iter, oi);
    api.seekToRank(&iter, 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    api.seekToRank(&iter, 0);
    TEST_ASSERT((pos = api.prev(&iter)) == NULL);
    api.resetIterator(&iter);

    /* Seek to rank 1 */
    api.initIterator(&iter, oi);
    api.seekToRank(&iter, 1);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 2.0);
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    api.seekToRank(&iter, 1);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    api.resetIterator(&iter);

    /* Seek to rank 3 (middle) */
    api.initIterator(&iter, oi);
    api.seekToRank(&iter, 3);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 4.0);
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    api.seekToRank(&iter, 3);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 3.0);
    api.resetIterator(&iter);

    /* Seek to rank 5 (last) */
    api.initIterator(&iter, oi);
    api.seekToRank(&iter, 5);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    api.seekToRank(&iter, 5);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 5.0);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, ReverseIteration) {
    OrderedIndex *oi = api.create();

    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Full reverse traversal */
    api.initIterator(&iter, oi);
    int count = 0;
    double expected = 5.0;
    while (((pos = api.prev(&iter)) != NULL)) {
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), expected);
        expected -= 1.0;
        count++;
    }
    TEST_ASSERT(count == 5);
    api.resetIterator(&iter);

    /* Reverse then forward */
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 5.0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 5.0);
    api.resetIterator(&iter);

    /* Forward then reverse */
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, SeekToScoreRange) {
    OrderedIndex *oi = api.create();

    /* Insert elements with scores 0,2,4,6,8 */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)(i * 2), ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Seek to first in range [2, 6] with offset 0 */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 2.0);
    api.resetIterator(&iter);

    /* Seek to second in range [2, 6] with offset 1 */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, 1);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 4.0);
    api.resetIterator(&iter);

    /* Seek to last in range [2, 6] with offset -1, positioned for prev() */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, -1);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 6.0);
    api.resetIterator(&iter);

    /* Seek with exclusive bounds (2, 6) - should start at 4 */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 1, 1, 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 4.0);
    api.resetIterator(&iter);

    /* Seek to empty range above all elements */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 10.0, 20.0, 0, 0, 0);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    /* Seek to empty range below all elements */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, -20.0, -10.0, 0, 0, 0);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    /* Out of range positive offset */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, 10);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    /* Negative offset beyond range */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, -10);
    TEST_ASSERT((pos = api.prev(&iter)) == NULL);
    api.resetIterator(&iter);

    /* Second from last with offset -2, positioned for prev() */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, -2);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 4.0);
    api.resetIterator(&iter);

    /* Empty range where min > max */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 6.0, 2.0, 0, 0, 0);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, SeekToScoreRangeIteration) {
    OrderedIndex *oi = api.create();

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Seek to range [3, 7] and iterate forward */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 3.0, 7.0, 0, 0, 0);
    int count = 0;
    double expected = 3.0;
    while (((pos = api.next(&iter)) != NULL) && api.getScore(pos) <= 7.0) {
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), expected);
        expected += 1.0;
        count++;
    }
    TEST_ASSERT(count == 5);
    api.resetIterator(&iter);

    /* Seek to last in range and iterate backward */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 3.0, 7.0, 0, 0, -1);
    count = 0;
    expected = 7.0;
    while (((pos = api.prev(&iter)) != NULL) && api.getScore(pos) >= 3.0) {
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), expected);
        expected -= 1.0;
        count++;
    }
    TEST_ASSERT(count == 5);
    api.resetIterator(&iter);

    /* Seek with offset and continue iteration */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 8.0, 0, 0, 2);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 4.0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    TEST_ASSERT_SCORE_EQ(api.getScore(pos), 5.0);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, SeekInfReverseIteration) {
    OrderedIndex *oi = api.create();

    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, NEG_INF, POS_INF, 0, 0, -1);
    int count = 0;
    double expected = 5.0;
    while (((pos = api.prev(&iter)) != NULL)) {
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), expected);
        expected -= 1.0;
        count++;
    }
    TEST_ASSERT(count == 5);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, SeekInfForwardIteration) {
    OrderedIndex *oi = api.create();

    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, NEG_INF, POS_INF, 0, 0, 0);
    int count = 0;
    double expected = 1.0;
    while (((pos = api.next(&iter)) != NULL)) {
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), expected);
        expected += 1.0;
        count++;
    }
    TEST_ASSERT(count == 5);
    api.resetIterator(&iter);

    api.free(oi);
}

TEST_P(OrderedIndexTest, SeekToLexRange) {
    OrderedIndex *oi = api.create();

    const char *elements[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (int i = 0; i < 5; i++) {
        sds ele = sdsnew(elements[i]);
        api.insertSds(oi, 1.0, ele);
        sdsfree(ele);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    const char *ptr;
    size_t len;

    sds minLex = sdsnew("banana");
    sds maxLex = sdsnew("date");

    /* Seek to first in lex range [banana, date] with offset 0 */
    api.initIterator(&iter, oi);
    api.seekToLexRange(&iter, minLex, maxLex, 0, 0, 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 6 && memcmp(ptr, "banana", 6) == 0);
    api.resetIterator(&iter);

    /* Seek to second in lex range with offset 1 */
    api.initIterator(&iter, oi);
    api.seekToLexRange(&iter, minLex, maxLex, 0, 0, 1);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 6 && memcmp(ptr, "cherry", 6) == 0);
    api.resetIterator(&iter);

    /* Seek to last in lex range with offset -1 */
    /* Seek to last in lex range with offset -1, positioned for prev() */
    api.initIterator(&iter, oi);
    api.seekToLexRange(&iter, minLex, maxLex, 0, 0, -1);
    TEST_ASSERT((pos = api.prev(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "date", 4) == 0);
    api.resetIterator(&iter);

    /* Seek with exclusive bounds (banana, date) - should start at cherry */
    api.initIterator(&iter, oi);
    api.seekToLexRange(&iter, minLex, maxLex, 1, 1, 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 6 && memcmp(ptr, "cherry", 6) == 0);
    api.resetIterator(&iter);

    sdsfree(minLex);
    sdsfree(maxLex);

    /* Seek to empty lex range */
    sds minEmpty = sdsnew("zzz");
    sds maxEmpty = sdsnew("zzzz");
    api.initIterator(&iter, oi);
    api.seekToLexRange(&iter, minEmpty, maxEmpty, 0, 0, 0);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);
    sdsfree(minEmpty);
    sdsfree(maxEmpty);

    /* Out of range positive offset */
    minLex = sdsnew("banana");
    maxLex = sdsnew("date");
    api.initIterator(&iter, oi);
    api.seekToLexRange(&iter, minLex, maxLex, 0, 0, 10);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);
    sdsfree(minLex);
    sdsfree(maxLex);

    api.free(oi);
}

TEST_P(OrderedIndexTest, DeleteRangeByLexInclusive) {
    OrderedIndex *oi = api.create();

    const char *elements[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (int i = 0; i < 5; i++) {
        sds ele = sdsnew(elements[i]);
        api.insertSds(oi, 1.0, ele);
        sdsfree(ele);
    }

    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, NULL, NULL);
    TEST_ASSERT(deleted == 3);
    TEST_ASSERT(api.length(oi) == 2);
    VERIFY_INTEGRITY(api, oi);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    const char *ptr;
    size_t len;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 5 && memcmp(ptr, "apple", 5) == 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 10 && memcmp(ptr, "elderberry", 10) == 0);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_P(OrderedIndexTest, DeleteRangeByLexExclusive) {
    OrderedIndex *oi = api.create();

    const char *elements[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (int i = 0; i < 5; i++) {
        sds ele = sdsnew(elements[i]);
        api.insertSds(oi, 1.0, ele);
        sdsfree(ele);
    }

    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 1, 1, NULL, NULL);
    TEST_ASSERT(deleted == 1);
    TEST_ASSERT(api.length(oi) == 4);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    const char *ptr;
    size_t len;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 5 && memcmp(ptr, "apple", 5) == 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 6 && memcmp(ptr, "banana", 6) == 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 4 && memcmp(ptr, "date", 4) == 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 10 && memcmp(ptr, "elderberry", 10) == 0);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_P(OrderedIndexTest, DeleteRangeByLexBoundaryCases) {
    /* Empty range: min > max lexicographically */
    OrderedIndex *oi = api.create();
    const char *elements[] = {"apple", "banana", "cherry"};
    for (int i = 0; i < 3; i++) {
        sds ele = sdsnew(elements[i]);
        api.insertSds(oi, 1.0, ele);
        sdsfree(ele);
    }

    sds min = sdsnew("zzz");
    sds max = sdsnew("aaa");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, NULL, NULL);
    TEST_ASSERT(deleted == 0);
    TEST_ASSERT(api.length(oi) == 3);
    sdsfree(min);
    sdsfree(max);
    api.free(oi);

    /* Delete all elements */
    oi = api.create();
    for (int i = 0; i < 3; i++) {
        sds ele = sdsnew(elements[i]);
        api.insertSds(oi, 1.0, ele);
        sdsfree(ele);
    }

    min = sdsnew("a");
    max = sdsnew("z");
    deleted = api.deleteRangeByLex(oi, min, max, 0, 0, NULL, NULL);
    TEST_ASSERT(deleted == 3);
    TEST_ASSERT(api.length(oi) == 0);
    sdsfree(min);
    sdsfree(max);
    api.free(oi);

    /* Delete single element */
    oi = api.create();
    for (int i = 0; i < 3; i++) {
        sds ele = sdsnew(elements[i]);
        api.insertSds(oi, 1.0, ele);
        sdsfree(ele);
    }

    min = sdsnew("banana");
    max = sdsnew("banana");
    deleted = api.deleteRangeByLex(oi, min, max, 0, 0, NULL, NULL);
    TEST_ASSERT(deleted == 1);
    TEST_ASSERT(api.length(oi) == 2);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    const char *ptr;
    size_t len;
    api.initIterator(&iter, oi);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 5 && memcmp(ptr, "apple", 5) == 0);
    TEST_ASSERT((pos = api.next(&iter)) != NULL);
    api.getElementRaw(pos, &ptr, &len);
    TEST_ASSERT(len == 6 && memcmp(ptr, "cherry", 6) == 0);
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_P(OrderedIndexTest, DeleteRangeByLexPreservesOutside) {
    OrderedIndex *oi = api.create();

    const char *elements[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot"};
    for (int i = 0; i < 6; i++) {
        sds ele = sdsnew(elements[i]);
        api.insertSds(oi, 1.0, ele);
        sdsfree(ele);
    }

    sds min = sdsnew("charlie");
    sds max = sdsnew("delta");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, NULL, NULL);
    TEST_ASSERT(deleted == 2);
    TEST_ASSERT(api.length(oi) == 4);

    const char *expected[] = {"alpha", "bravo", "echo", "foxtrot"};
    size_t expected_lens[] = {5, 5, 4, 7};
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    const char *ptr;
    size_t len;
    api.initIterator(&iter, oi);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT((pos = api.next(&iter)) != NULL);
        api.getElementRaw(pos, &ptr, &len);
        TEST_ASSERT(len == expected_lens[i] && memcmp(ptr, expected[i], len) == 0);
    }
    TEST_ASSERT((pos = api.next(&iter)) == NULL);
    api.resetIterator(&iter);

    /* Verify scores are preserved */
    api.initIterator(&iter, oi);
    while (((pos = api.next(&iter)) != NULL)) {
        TEST_ASSERT_SCORE_EQ(api.getScore(pos), 1.0);
    }
    api.resetIterator(&iter);

    /* Verify ranks are correct after deletion */
    for (unsigned long r = 1; r <= 4; r++) {
        OrderedIndexItem *node = api.getByRank(oi, r);
        TEST_ASSERT(node != NULL);
        unsigned long rank = api.getRank(oi, node);
        TEST_ASSERT(rank == r);
    }

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

/* ========== Randomized property tests ========== */

struct RandomIndexEntry {
    OrderedIndexItem *node;
    double score;
    std::string element;
};

static std::string test_random_element(std::mt19937 &rng, int maxLen = 16) {
    std::uniform_int_distribution<int> lenDist(1, maxLen);
    std::uniform_int_distribution<int> charDist('a', 'z');
    int len = lenDist(rng);
    std::string s(len, ' ');
    for (int i = 0; i < len; i++) s[i] = (char)charDist(rng);
    return s;
}

static double test_random_score(std::mt19937 &rng) {
    std::uniform_real_distribution<double> dist(-1e6, 1e6);
    return dist(rng);
}

static std::vector<RandomIndexEntry> test_build_random_index(OrderedIndexTestApi &api, OrderedIndex *oi, std::mt19937 &rng, int count) {
    std::vector<RandomIndexEntry> entries;
    for (int i = 0; i < count; i++) {
        double score = test_random_score(rng);
        std::string elem = test_random_element(rng) + std::to_string(i);
        sds ele = sdsnew(elem.c_str());
        OrderedIndexItem *node = api.insertSds(oi, score, ele);
        entries.push_back({node, score, elem});
        sdsfree(ele);
    }
    return entries;
}

TEST_P(OrderedIndexTest, RandomizedInsertAndTraversal) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; trial++) {
        std::uniform_int_distribution<int> sizeDist(1, 50);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        test_build_random_index(api, oi, rng, n);

        ASSERT_EQ(api.length(oi), (unsigned long)n);
        VERIFY_INTEGRITY(api, oi);

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        int count = 0;
        double prevScore = NEG_INF;
        while (((pos = api.next(&iter)) != NULL)) {
            double s = api.getScore(pos);
            ASSERT_GE(s, prevScore);
            prevScore = s;
            count++;
        }
        ASSERT_EQ(count, n);
        api.resetIterator(&iter);
        api.free(oi);
    }
}

TEST_P(OrderedIndexTest, RandomizedBackwardTraversal) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; trial++) {
        std::uniform_int_distribution<int> sizeDist(1, 50);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        test_build_random_index(api, oi, rng, n);

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        int count = 0;
        double prevScore = POS_INF;
        while (((pos = api.prev(&iter)) != NULL)) {
            double s = api.getScore(pos);
            ASSERT_LE(s, prevScore);
            prevScore = s;
            count++;
        }
        ASSERT_EQ(count, n);
        api.resetIterator(&iter);
        api.free(oi);
    }
}

TEST_P(OrderedIndexTest, RandomizedScoreRetrieval) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; trial++) {
        std::uniform_int_distribution<int> sizeDist(1, 50);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        auto entries = test_build_random_index(api, oi, rng, n);

        for (auto &e : entries) {
            TEST_ASSERT_SCORE_EQ(api.getScore(e.node), e.score);
        }
        api.free(oi);
    }
}

TEST_P(OrderedIndexTest, RandomizedRankConsistency) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; trial++) {
        std::uniform_int_distribution<int> sizeDist(1, 50);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        test_build_random_index(api, oi, rng, n);

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        unsigned long expectedRank = 1;
        while (((pos = api.next(&iter)) != NULL)) {
            unsigned long rank = api.getRank(oi, pos);
            ASSERT_EQ(rank, expectedRank);
            OrderedIndexItem *byRank = api.getByRank(oi, expectedRank);
            ASSERT_EQ(byRank, pos);
            expectedRank++;
        }
        ASSERT_EQ(expectedRank - 1, (unsigned long)n);
        api.resetIterator(&iter);
        api.free(oi);
    }
}

TEST_P(OrderedIndexTest, RandomizedDelete) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; trial++) {
        std::uniform_int_distribution<int> sizeDist(2, 30);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        auto entries = test_build_random_index(api, oi, rng, n);

        std::uniform_int_distribution<int> pickDist(0, n - 1);
        int delIdx = pickDist(rng);
        api.deleteItem(oi, entries[delIdx].node);

        ASSERT_EQ(api.length(oi), (unsigned long)(n - 1));
        VERIFY_INTEGRITY(api, oi);

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        int count = 0;
        double prevScore = NEG_INF;
        while (((pos = api.next(&iter)) != NULL)) {
            ASSERT_GE(api.getScore(pos), prevScore);
            prevScore = api.getScore(pos);
            count++;
        }
        ASSERT_EQ(count, n - 1);
        api.resetIterator(&iter);
        api.free(oi);
    }
}

TEST_P(OrderedIndexTest, RandomizedUpdateScore) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; trial++) {
        std::uniform_int_distribution<int> sizeDist(2, 30);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        auto entries = test_build_random_index(api, oi, rng, n);

        std::uniform_int_distribution<int> pickDist(0, n - 1);
        int updIdx = pickDist(rng);
        double newScore = test_random_score(rng);

        OrderedIndexItem *updated = api.updateScore(oi, entries[updIdx].node, newScore);
        ASSERT_NE(updated, nullptr);
        TEST_ASSERT_SCORE_EQ(api.getScore(updated), newScore);
        ASSERT_EQ(api.length(oi), (unsigned long)n);
        VERIFY_INTEGRITY(api, oi);

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        double prevScore = NEG_INF;
        while (((pos = api.next(&iter)) != NULL)) {
            ASSERT_GE(api.getScore(pos), prevScore);
            prevScore = api.getScore(pos);
        }
        api.resetIterator(&iter);
        api.free(oi);
    }
}

TEST_P(OrderedIndexTest, RandomizedPop) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 10; trial++) {
        std::uniform_int_distribution<int> sizeDist(3, 30);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        test_build_random_index(api, oi, rng, n);

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        ASSERT_TRUE(((pos = api.next(&iter)) != NULL));
        double minScore = api.getScore(pos);
        api.resetIterator(&iter);

        api.initIterator(&iter, oi);
        ASSERT_TRUE(((pos = api.prev(&iter)) != NULL));
        double maxScore = api.getScore(pos);
        api.resetIterator(&iter);

        OrderedIndexItem *first = api.popFirst(oi);
        ASSERT_NE(first, nullptr);
        TEST_ASSERT_SCORE_EQ(api.getScore(first), minScore);
        ASSERT_EQ(api.length(oi), (unsigned long)(n - 1));
        api.freeItem(first);
        VERIFY_INTEGRITY(api, oi);

        OrderedIndexItem *last = api.popLast(oi);
        ASSERT_NE(last, nullptr);
        TEST_ASSERT_SCORE_EQ(api.getScore(last), maxScore);
        ASSERT_EQ(api.length(oi), (unsigned long)(n - 2));
        api.freeItem(last);
        VERIFY_INTEGRITY(api, oi);

        api.initIterator(&iter, oi);
        double prevScore = NEG_INF;
        while (((pos = api.next(&iter)) != NULL)) {
            ASSERT_GE(api.getScore(pos), prevScore);
            prevScore = api.getScore(pos);
        }
        api.resetIterator(&iter);
        api.free(oi);
    }
}

TEST_P(OrderedIndexTest, RandomizedDeleteRangeByScore) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; trial++) {
        std::uniform_int_distribution<int> sizeDist(5, 40);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        auto entries = test_build_random_index(api, oi, rng, n);

        double s1 = test_random_score(rng), s2 = test_random_score(rng);
        double lo = (std::min)(s1, s2), hi = (std::max)(s1, s2);

        int expectedDeleted = 0;
        for (auto &e : entries) {
            if (e.score >= lo && e.score <= hi) expectedDeleted++;
        }

        unsigned long deleted = api.deleteRangeByScore(oi, lo, hi, 0, 0, NULL, NULL);
        ASSERT_EQ(deleted, (unsigned long)expectedDeleted);
        ASSERT_EQ(api.length(oi), (unsigned long)(n - expectedDeleted));
        VERIFY_INTEGRITY(api, oi);

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        double prevScore = NEG_INF;
        while (((pos = api.next(&iter)) != NULL)) {
            double s = api.getScore(pos);
            ASSERT_TRUE(s < lo || s > hi);
            ASSERT_GE(s, prevScore);
            prevScore = s;
        }
        api.resetIterator(&iter);
        api.free(oi);
    }
}

TEST_P(OrderedIndexTest, RandomizedDeleteRangeByRank) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; trial++) {
        std::uniform_int_distribution<int> sizeDist(5, 40);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        test_build_random_index(api, oi, rng, n);

        std::uniform_int_distribution<int> rankDist(1, n);
        int r1 = rankDist(rng), r2 = rankDist(rng);
        unsigned long start = (unsigned long)(std::min)(r1, r2);
        unsigned long end = (unsigned long)(std::max)(r1, r2);
        unsigned long expectedDeleted = end - start + 1;

        unsigned long deleted = api.deleteRangeByRank(oi, start, end, NULL, NULL);
        ASSERT_EQ(deleted, expectedDeleted);
        ASSERT_EQ(api.length(oi), (unsigned long)(n)-expectedDeleted);
        VERIFY_INTEGRITY(api, oi);

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        int remaining = 0;
        double prevScore = NEG_INF;
        while (((pos = api.next(&iter)) != NULL)) {
            ASSERT_GE(api.getScore(pos), prevScore);
            prevScore = api.getScore(pos);
            remaining++;
        }
        ASSERT_EQ(remaining, n - (int)expectedDeleted);
        api.resetIterator(&iter);
        api.free(oi);
    }
}

TEST_P(OrderedIndexTest, RandomizedForwardBackwardMirror) {
    std::mt19937 rng(42);
    for (int trial = 0; trial < 20; trial++) {
        std::uniform_int_distribution<int> sizeDist(1, 50);
        int n = sizeDist(rng);

        OrderedIndex *oi = api.create();
        test_build_random_index(api, oi, rng, n);

        std::vector<double> forwardScores;
        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        while (((pos = api.next(&iter)) != NULL)) {
            forwardScores.push_back(api.getScore(pos));
        }
        api.resetIterator(&iter);

        std::vector<double> backwardScores;
        api.initIterator(&iter, oi);
        while (((pos = api.prev(&iter)) != NULL)) {
            backwardScores.push_back(api.getScore(pos));
        }
        api.resetIterator(&iter);

        ASSERT_EQ(forwardScores.size(), backwardScores.size());
        std::reverse(backwardScores.begin(), backwardScores.end());
        for (size_t i = 0; i < forwardScores.size(); i++) {
            ASSERT_EQ(forwardScores[i], backwardScores[i]);
        }
        api.free(oi);
    }
}

/* ========== Count range tests ========== */

TEST_P(OrderedIndexTest, CountScoreRange) {
    OrderedIndex *oi = api.create();

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        sds ele = sdsnew(buf);
        api.insertSds(oi, (double)i, ele);
        sdsfree(ele);
    }

    /* Full range */
    ASSERT_EQ(api.countScoreRange(oi, NEG_INF, POS_INF, 0, 0), 10UL);

    /* Inclusive [3, 6] */
    ASSERT_EQ(api.countScoreRange(oi, 3.0, 6.0, 0, 0), 4UL);

    /* Exclusive (3, 6) */
    ASSERT_EQ(api.countScoreRange(oi, 3.0, 6.0, 1, 1), 2UL);

    /* Single element [5, 5] */
    ASSERT_EQ(api.countScoreRange(oi, 5.0, 5.0, 0, 0), 1UL);

    /* Empty exclusive (5, 5) */
    ASSERT_EQ(api.countScoreRange(oi, 5.0, 5.0, 1, 0), 0UL);

    /* No match above */
    ASSERT_EQ(api.countScoreRange(oi, 10.0, 20.0, 0, 0), 0UL);

    /* No match below */
    ASSERT_EQ(api.countScoreRange(oi, -20.0, -10.0, 0, 0), 0UL);

    /* Min > max */
    ASSERT_EQ(api.countScoreRange(oi, 6.0, 3.0, 0, 0), 0UL);

    /* First element only [0, 0] */
    ASSERT_EQ(api.countScoreRange(oi, 0.0, 0.0, 0, 0), 1UL);

    /* Last element only [9, 9] */
    ASSERT_EQ(api.countScoreRange(oi, 9.0, 9.0, 0, 0), 1UL);

    api.free(oi);
}

TEST_P(OrderedIndexTest, CountScoreRangeEmpty) {
    OrderedIndex *oi = api.create();
    ASSERT_EQ(api.countScoreRange(oi, NEG_INF, POS_INF, 0, 0), 0UL);
    api.free(oi);
}

TEST_P(OrderedIndexTest, CountLexRange) {
    OrderedIndex *oi = api.create();

    const char *elements[] = {"apple", "banana", "cherry", "date", "elderberry"};
    for (int i = 0; i < 5; i++) {
        sds ele = sdsnew(elements[i]);
        api.insertSds(oi, 1.0, ele);
        sdsfree(ele);
    }

    /* Inclusive [banana, date] */
    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    ASSERT_EQ(api.countLexRange(oi, min, max, 0, 0), 3UL);
    sdsfree(min);
    sdsfree(max);

    /* Exclusive (banana, date) */
    min = sdsnew("banana");
    max = sdsnew("date");
    ASSERT_EQ(api.countLexRange(oi, min, max, 1, 1), 1UL);
    sdsfree(min);
    sdsfree(max);

    /* Single element [cherry, cherry] */
    min = sdsnew("cherry");
    max = sdsnew("cherry");
    ASSERT_EQ(api.countLexRange(oi, min, max, 0, 0), 1UL);
    sdsfree(min);
    sdsfree(max);

    /* No match */
    min = sdsnew("fig");
    max = sdsnew("grape");
    ASSERT_EQ(api.countLexRange(oi, min, max, 0, 0), 0UL);
    sdsfree(min);
    sdsfree(max);

    /* All elements */
    min = sdsnew("a");
    max = sdsnew("z");
    ASSERT_EQ(api.countLexRange(oi, min, max, 0, 0), 5UL);
    sdsfree(min);
    sdsfree(max);

    api.free(oi);
}

TEST_P(OrderedIndexTest, CountLexRangeEmpty) {
    OrderedIndex *oi = api.create();
    sds min = sdsnew("a");
    sds max = sdsnew("z");
    ASSERT_EQ(api.countLexRange(oi, min, max, 0, 0), 0UL);
    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

/* ========== Instantiate parameterized tests for all implementations ========== */

INSTANTIATE_TEST_SUITE_P(AllImplementations,
                         OrderedIndexTest,
                         ::testing::Values(&skiplistImpl),
                         orderedIndexTestName);

/* ========== On-Delete Callback Tests ========== */

struct OnDeleteRecord {
    int count;
    std::vector<std::string> elements;
};

static void testOnDeleteCallback(OrderedIndexItem *item, void *ctx) {
    OnDeleteRecord *rec = (OnDeleteRecord *)ctx;
    rec->count++;
    const char *ptr;
    size_t len;
    skiplistGetElementRaw(item, &ptr, &len);
    rec->elements.emplace_back(ptr, len);
    orderedIndexFreeItem(item);
}

class OnDeleteCallbackTest : public ::testing::Test {
  protected:
    SkiplistOrderedIndex api;

    void insertN(OrderedIndex *oi, int n) {
        for (int i = 0; i < n; i++) {
            std::string name = "key" + std::to_string(i);
            sds ele = sdsnew(name.c_str());
            api.insertSds(oi, (double)i, ele);
            sdsfree(ele);
        }
    }

    void insertLex(OrderedIndex *oi, const std::vector<std::string> &elems, double score = 1.0) {
        for (auto &e : elems) {
            sds ele = sdsnew(e.c_str());
            api.insertSds(oi, score, ele);
            sdsfree(ele);
        }
    }

    std::vector<std::string> collectElements(OrderedIndex *oi) {
        std::vector<std::string> result;
        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        while (((pos = api.next(&iter)) != NULL)) {
            const char *ptr;
            size_t len;
            api.getElementRaw(pos, &ptr, &len);
            result.emplace_back(ptr, len);
        }
        api.resetIterator(&iter);
        return result;
    }
};

/* DeleteRangeByScore */

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_EmptyAndNoMatch) {
    OnDeleteRecord rec = {0, {}};

    OrderedIndex *oi = api.create();
    unsigned long deleted = api.deleteRangeByScore(oi, 0.0, 10.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    api.free(oi);

    oi = api.create();
    insertN(oi, 5);
    rec = {0, {}};
    deleted = api.deleteRangeByScore(oi, 10.0, 20.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(api.length(oi), 5UL);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_Subset) {
    OrderedIndex *oi = api.create();
    insertN(oi, 10);

    OnDeleteRecord rec = {0, {}};
    unsigned long deleted = api.deleteRangeByScore(oi, 3.0, 6.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 4UL);
    ASSERT_EQ(rec.count, 4);
    ASSERT_EQ(api.length(oi), 6UL);
    VERIFY_INTEGRITY(api, oi);

    std::sort(rec.elements.begin(), rec.elements.end());
    ASSERT_EQ(rec.elements, (std::vector<std::string>{"key3", "key4", "key5", "key6"}));

    auto remaining = collectElements(oi);
    ASSERT_EQ(remaining, (std::vector<std::string>{"key0", "key1", "key2", "key7", "key8", "key9"}));
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_All) {
    OrderedIndex *oi = api.create();
    insertN(oi, 5);

    OnDeleteRecord rec = {0, {}};
    unsigned long deleted = api.deleteRangeByScore(oi, NEG_INF, POS_INF, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 5UL);
    ASSERT_EQ(rec.count, 5);
    ASSERT_EQ(api.length(oi), 0UL);
    VERIFY_INTEGRITY(api, oi);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_NullCallback) {
    OrderedIndex *oi = api.create();
    insertN(oi, 5);

    unsigned long deleted = api.deleteRangeByScore(oi, 1.0, 3.0, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(api.length(oi), 2UL);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_ExclusiveBounds) {
    OrderedIndex *oi = api.create();
    insertN(oi, 10);

    OnDeleteRecord rec = {0, {}};
    unsigned long deleted = api.deleteRangeByScore(oi, 3.0, 7.0, 1, 1, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    std::sort(rec.elements.begin(), rec.elements.end());
    ASSERT_EQ(rec.elements, (std::vector<std::string>{"key4", "key5", "key6"}));
    ASSERT_EQ(api.length(oi), 7UL);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_SingleElement) {
    OrderedIndex *oi = api.create();
    insertN(oi, 5);

    OnDeleteRecord rec = {0, {}};
    unsigned long deleted = api.deleteRangeByScore(oi, 2.0, 2.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_EQ(rec.elements[0], "key2");
    ASSERT_EQ(api.length(oi), 4UL);
    api.free(oi);
}

/* DeleteRangeByRank */

TEST_F(OnDeleteCallbackTest, DeleteRangeByRank_EmptyAndNoMatch) {
    OnDeleteRecord rec = {0, {}};

    OrderedIndex *oi = api.create();
    unsigned long deleted = api.deleteRangeByRank(oi, 1, 5, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    api.free(oi);

    oi = api.create();
    insertN(oi, 3);
    rec = {0, {}};
    deleted = api.deleteRangeByRank(oi, 10, 20, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(api.length(oi), 3UL);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByRank_Subset) {
    OrderedIndex *oi = api.create();
    insertN(oi, 10);

    OnDeleteRecord rec = {0, {}};
    unsigned long deleted = api.deleteRangeByRank(oi, 3, 5, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    ASSERT_EQ(api.length(oi), 7UL);

    std::sort(rec.elements.begin(), rec.elements.end());
    ASSERT_EQ(rec.elements, (std::vector<std::string>{"key2", "key3", "key4"}));

    auto remaining = collectElements(oi);
    ASSERT_EQ(remaining, (std::vector<std::string>{"key0", "key1", "key5", "key6", "key7", "key8", "key9"}));
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByRank_All) {
    OrderedIndex *oi = api.create();
    insertN(oi, 5);

    OnDeleteRecord rec = {0, {}};
    unsigned long deleted = api.deleteRangeByRank(oi, 1, 5, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 5UL);
    ASSERT_EQ(rec.count, 5);
    ASSERT_EQ(api.length(oi), 0UL);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByRank_NullCallback) {
    OrderedIndex *oi = api.create();
    insertN(oi, 5);

    unsigned long deleted = api.deleteRangeByRank(oi, 2, 4, NULL, NULL);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(api.length(oi), 2UL);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByRank_ExclusiveBounds) {
    OrderedIndex *oi = api.create();
    insertN(oi, 5);

    OnDeleteRecord rec = {0, {}};
    unsigned long deleted = api.deleteRangeByRank(oi, 3, 3, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_EQ(rec.elements[0], "key2");

    auto remaining = collectElements(oi);
    ASSERT_EQ(remaining, (std::vector<std::string>{"key0", "key1", "key3", "key4"}));
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByRank_SingleElement) {
    OrderedIndex *oi = api.create();
    insertN(oi, 5);

    OnDeleteRecord rec = {0, {}};
    unsigned long deleted = api.deleteRangeByRank(oi, 1, 1, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_EQ(rec.elements[0], "key0");
    ASSERT_EQ(api.length(oi), 4UL);
    api.free(oi);
}

/* DeleteRangeByLex */

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_EmptyAndNoMatch) {
    OnDeleteRecord rec = {0, {}};

    OrderedIndex *oi = api.create();
    sds min = sdsnew("a");
    sds max = sdsnew("z");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    sdsfree(min);
    sdsfree(max);
    api.free(oi);

    oi = api.create();
    insertLex(oi, {"apple", "banana", "cherry"});
    rec = {0, {}};
    min = sdsnew("x");
    max = sdsnew("z");
    deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(api.length(oi), 3UL);
    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_Subset) {
    OrderedIndex *oi = api.create();
    insertLex(oi, {"apple", "banana", "cherry", "date", "elderberry"});

    OnDeleteRecord rec = {0, {}};
    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    ASSERT_EQ(api.length(oi), 2UL);

    std::sort(rec.elements.begin(), rec.elements.end());
    ASSERT_EQ(rec.elements, (std::vector<std::string>{"banana", "cherry", "date"}));

    auto remaining = collectElements(oi);
    ASSERT_EQ(remaining, (std::vector<std::string>{"apple", "elderberry"}));

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_All) {
    OrderedIndex *oi = api.create();
    insertLex(oi, {"apple", "banana", "cherry"});

    OnDeleteRecord rec = {0, {}};
    sds min = sdsnew("a");
    sds max = sdsnew("z");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    ASSERT_EQ(api.length(oi), 0UL);

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_NullCallback) {
    OrderedIndex *oi = api.create();
    insertLex(oi, {"apple", "banana", "cherry", "date"});

    sds min = sdsnew("banana");
    sds max = sdsnew("cherry");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 2UL);
    ASSERT_EQ(api.length(oi), 2UL);

    auto remaining = collectElements(oi);
    ASSERT_EQ(remaining, (std::vector<std::string>{"apple", "date"}));

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_ExclusiveBounds) {
    OrderedIndex *oi = api.create();
    insertLex(oi, {"apple", "banana", "cherry", "date", "elderberry"});

    OnDeleteRecord rec = {0, {}};
    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 1, 1, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_EQ(rec.elements[0], "cherry");
    ASSERT_EQ(api.length(oi), 4UL);

    auto remaining = collectElements(oi);
    ASSERT_EQ(remaining, (std::vector<std::string>{"apple", "banana", "date", "elderberry"}));

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_SingleElement) {
    OrderedIndex *oi = api.create();
    insertLex(oi, {"apple", "banana", "cherry"});

    OnDeleteRecord rec = {0, {}};
    sds min = sdsnew("banana");
    sds max = sdsnew("banana");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_EQ(rec.elements[0], "banana");
    ASSERT_EQ(api.length(oi), 2UL);

    auto remaining = collectElements(oi);
    ASSERT_EQ(remaining, (std::vector<std::string>{"apple", "cherry"}));

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

/* ========== Range-Delete Hashtable Consistency Tests ========== */

static void hashtableConsistencyOnDelete(OrderedIndexItem *item, void *ctx) {
    std::set<std::string> *ht = (std::set<std::string> *)ctx;
    const char *ptr;
    size_t len;
    skiplistGetElementRaw(item, &ptr, &len);
    ht->erase(std::string(ptr, len));
    orderedIndexFreeItem(item);
}

class RangeDeleteHashtableConsistencyTest : public ::testing::Test {
  protected:
    SkiplistOrderedIndex api;

    void insertN(OrderedIndex *oi, std::set<std::string> &ht, int n) {
        for (int i = 0; i < n; i++) {
            std::string name = "key" + std::to_string(i);
            sds ele = sdsnew(name.c_str());
            api.insertSds(oi, (double)i, ele);
            ht.insert(name);
            sdsfree(ele);
        }
    }

    void insertLex(OrderedIndex *oi, std::set<std::string> &ht, const std::vector<std::string> &elems, double score = 1.0) {
        for (auto &e : elems) {
            sds ele = sdsnew(e.c_str());
            api.insertSds(oi, score, ele);
            ht.insert(e);
            sdsfree(ele);
        }
    }

    std::set<std::string> collectIndexElements(OrderedIndex *oi) {
        std::set<std::string> result;
        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        while (((pos = api.next(&iter)) != NULL)) {
            const char *ptr;
            size_t len;
            api.getElementRaw(pos, &ptr, &len);
            result.insert(std::string(ptr, len));
        }
        api.resetIterator(&iter);
        return result;
    }
};

/* ByScore */

TEST_F(RangeDeleteHashtableConsistencyTest, ByScore_PartialDelete) {
    OrderedIndex *oi = api.create();
    std::set<std::string> simulatedHt;
    insertN(oi, simulatedHt, 10);

    api.deleteRangeByScore(oi, 3.0, 6.0, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    std::set<std::string> indexElements = collectIndexElements(oi);
    ASSERT_EQ(indexElements, simulatedHt);
    ASSERT_EQ(indexElements.size(), 6UL);

    api.free(oi);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByScore_FullDelete) {
    OrderedIndex *oi = api.create();
    std::set<std::string> simulatedHt;
    insertN(oi, simulatedHt, 10);

    api.deleteRangeByScore(oi, NEG_INF, POS_INF, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    std::set<std::string> indexElements = collectIndexElements(oi);
    ASSERT_EQ(indexElements, simulatedHt);
    ASSERT_TRUE(indexElements.empty());

    api.free(oi);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByScore_EmptyRange) {
    OrderedIndex *oi = api.create();
    std::set<std::string> simulatedHt;
    insertN(oi, simulatedHt, 10);

    api.deleteRangeByScore(oi, 20.0, 30.0, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    std::set<std::string> indexElements = collectIndexElements(oi);
    ASSERT_EQ(indexElements, simulatedHt);
    ASSERT_EQ(indexElements.size(), 10UL);

    api.free(oi);
}

/* ByRank */

TEST_F(RangeDeleteHashtableConsistencyTest, ByRank_PartialDelete) {
    OrderedIndex *oi = api.create();
    std::set<std::string> simulatedHt;
    insertN(oi, simulatedHt, 10);

    api.deleteRangeByRank(oi, 3, 5, hashtableConsistencyOnDelete, &simulatedHt);

    std::set<std::string> indexElements = collectIndexElements(oi);
    ASSERT_EQ(indexElements, simulatedHt);
    ASSERT_EQ(indexElements.size(), 7UL);

    api.free(oi);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByRank_FullDelete) {
    OrderedIndex *oi = api.create();
    std::set<std::string> simulatedHt;
    insertN(oi, simulatedHt, 10);

    api.deleteRangeByRank(oi, 1, 10, hashtableConsistencyOnDelete, &simulatedHt);

    std::set<std::string> indexElements = collectIndexElements(oi);
    ASSERT_EQ(indexElements, simulatedHt);
    ASSERT_TRUE(indexElements.empty());

    api.free(oi);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByRank_EmptyRange) {
    OrderedIndex *oi = api.create();
    std::set<std::string> simulatedHt;
    insertN(oi, simulatedHt, 10);

    api.deleteRangeByRank(oi, 20, 30, hashtableConsistencyOnDelete, &simulatedHt);

    std::set<std::string> indexElements = collectIndexElements(oi);
    ASSERT_EQ(indexElements, simulatedHt);
    ASSERT_EQ(indexElements.size(), 10UL);

    api.free(oi);
}

/* ByLex */

TEST_F(RangeDeleteHashtableConsistencyTest, ByLex_PartialDelete) {
    OrderedIndex *oi = api.create();
    std::set<std::string> simulatedHt;
    insertLex(oi, simulatedHt, {"apple", "banana", "cherry", "date", "elderberry"});

    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    api.deleteRangeByLex(oi, min, max, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    std::set<std::string> indexElements = collectIndexElements(oi);
    ASSERT_EQ(indexElements, simulatedHt);
    ASSERT_EQ(indexElements.size(), 2UL);

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByLex_FullDelete) {
    OrderedIndex *oi = api.create();
    std::set<std::string> simulatedHt;
    insertLex(oi, simulatedHt, {"apple", "banana", "cherry", "date", "elderberry"});

    sds min = sdsnew("a");
    sds max = sdsnew("z");
    api.deleteRangeByLex(oi, min, max, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    std::set<std::string> indexElements = collectIndexElements(oi);
    ASSERT_EQ(indexElements, simulatedHt);
    ASSERT_TRUE(indexElements.empty());

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByLex_EmptyRange) {
    OrderedIndex *oi = api.create();
    std::set<std::string> simulatedHt;
    insertLex(oi, simulatedHt, {"apple", "banana", "cherry", "date", "elderberry"});

    sds min = sdsnew("zzz");
    sds max = sdsnew("zzzz");
    api.deleteRangeByLex(oi, min, max, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    std::set<std::string> indexElements = collectIndexElements(oi);
    ASSERT_EQ(indexElements, simulatedHt);
    ASSERT_EQ(indexElements.size(), 5UL);

    sdsfree(min);
    sdsfree(max);
    api.free(oi);
}
