/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
}

/* Undefine min/max macros from server.h to avoid conflicts */
#undef min
#undef max

#include "ordered_index_test.h"

#include <cmath>
#include <cstring>
#include <string> /* only for GTest name generator */

/* Clean up shared lex sentinels allocated by OrderedIndexTest::SetUp(). */
static void cleanupSharedSentinels(void) __attribute__((destructor));
static void cleanupSharedSentinels(void) {
    if (shared.minstring) {
        sdsfree(shared.minstring);
        shared.minstring = NULL;
    }
    if (shared.maxstring) {
        sdsfree(shared.maxstring);
        shared.maxstring = NULL;
    }
}

/* ---- C-style test helpers ---- */

#define TEST_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TEST_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Collect all elements from an ordered index into a pre-allocated sds array.
 * Caller must free each sds and the array itself. */
static sds *collectIndexToSds(OrderedIndexTestApi &api, OrderedIndex *oi, size_t *out_n) {
    size_t n = api.length(oi);
    *out_n = n;
    if (n == 0) return NULL;
    sds *arr = (sds *)zmalloc(sizeof(sds) * n);
    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    for (size_t i = 0; i < n; i++) {
        OrderedIndexItem *pos = api.next(&iter);
        const char *ptr;
        size_t len;
        api.getElementRaw(pos, &ptr, &len);
        arr[i] = sdsnewlen(ptr, len);
    }
    api.resetIterator(&iter);
    return arr;
}

static void freeSdsArray(sds *arr, size_t n) {
    for (size_t i = 0; i < n; i++) sdsfree(arr[i]);
    zfree(arr);
}

/* Assert that an sds array matches an expected list of C strings. */
#define ASSERT_SDS_ARRAY_EQ(arr, n, ...)                \
    do {                                                \
        const char *_exp[] = {__VA_ARGS__};             \
        size_t _exp_n = sizeof(_exp) / sizeof(_exp[0]); \
        ASSERT_EQ((size_t)(n), _exp_n);                 \
        for (size_t _i = 0; _i < _exp_n; _i++) {        \
            ASSERT_STREQ(arr[_i], _exp[_i]);            \
        }                                               \
    } while (0)

static int sdsArrayCmp(const void *a, const void *b) {
    return sdscmp(*(sds *)a, *(sds *)b);
}

static void sortSdsArray(sds *arr, size_t n) {
    qsort(arr, n, sizeof(sds), sdsArrayCmp);
}

static void reverseDoubleArray(double *arr, size_t n) {
    for (size_t i = 0; i < n / 2; i++) {
        double tmp = arr[i];
        arr[i] = arr[n - 1 - i];
        arr[n - 1 - i] = tmp;
    }
}

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

/* ========== Shared test data ========== */

static const char *FRUITS[] = {"apple", "banana", "cherry", "date", "elderberry"};
static const int FRUITS_COUNT = 5;
static const char *NATO[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot"};
static const int NATO_COUNT = 6;

/* ========== Parameterized test fixture ========== */

class OrderedIndexTest : public ::testing::TestWithParam<OrderedIndexTestApi *> {
  protected:
    OrderedIndexTestApi &api = *GetParam();
    OrderedIndex *oi = nullptr;

    void SetUp() override {
        /* Ensure shared lex sentinels are initialized (normally done by createSharedObjects) */
        if (!shared.minstring) shared.minstring = sdsnew("minstring");
        if (!shared.maxstring) shared.maxstring = sdsnew("maxstring");
        oi = api.create();
    }
    void TearDown() override {
        if (oi) api.free(oi);
    }

    /* Insert a string literal at given score. */
    OrderedIndexItem *insert(double score, const char *ele) {
        sds s = sdsnew(ele);
        OrderedIndexItem *node = api.insertSds(oi, score, s);
        sdsfree(s);
        return node;
    }

    /* Insert N sequential elements ("key0"..."keyN-1") at scores 0..N-1. */
    void populateSequential(int n) {
        for (int i = 0; i < n; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key%d", i);
            insert((double)i, buf);
        }
    }

    /* Assert next iterator element has expected score. */
    OrderedIndexItem *assertNextScore(OrderedIndexIterator *iter, double expected) {
        OrderedIndexItem *pos = api.next(iter);
        EXPECT_NE(pos, nullptr);
        if (pos) {
            EXPECT_DOUBLE_EQ(api.getScore(pos), expected);
        }
        return pos;
    }

    /* Assert prev iterator element has expected score. */
    OrderedIndexItem *assertPrevScore(OrderedIndexIterator *iter, double expected) {
        OrderedIndexItem *pos = api.prev(iter);
        EXPECT_NE(pos, nullptr);
        if (pos) {
            EXPECT_DOUBLE_EQ(api.getScore(pos), expected);
        }
        return pos;
    }

    /* Assert element content matches expected string. */
    void assertElement(OrderedIndexItem *node, const char *expected) {
        const char *ptr;
        size_t len;
        api.getElementRaw(node, &ptr, &len);
        ASSERT_EQ(len, strlen(expected));
        ASSERT_EQ(memcmp(ptr, expected, len), 0);
    }

    /* Assert node has expected score. */
    void assertScore(OrderedIndexItem *node, double expected) {
        ASSERT_DOUBLE_EQ(api.getScore(node), expected);
    }

    /* Delete lex range using const char* (handles sds lifecycle). */
    unsigned long deleteLexRange(const char *min_str, const char *max_str, int min_ex, int max_ex, OrderedIndexOnDelete cb, void *ctx) {
        sds min = sdsnew(min_str);
        sds max = sdsnew(max_str);
        unsigned long deleted = api.deleteRangeByLex(oi, min, max, min_ex, max_ex, cb, ctx);
        sdsfree(min);
        sdsfree(max);
        return deleted;
    }

    /* Count lex range using const char*. */
    unsigned long countLexRange(const char *min_str, const char *max_str, int min_ex, int max_ex) {
        sds min = sdsnew(min_str);
        sds max = sdsnew(max_str);
        unsigned long count = api.countLexRange(oi, min, max, min_ex, max_ex);
        sdsfree(min);
        sdsfree(max);
        return count;
    }

    /* Seek to lex range using const char*. */
    void seekToLexRange(OrderedIndexIterator *it, const char *min_str, const char *max_str, int min_ex, int max_ex, long offset) {
        sds min = sdsnew(min_str);
        sds max = sdsnew(max_str);
        api.seekToLexRange(it, min, max, min_ex, max_ex, offset);
        sdsfree(min);
        sdsfree(max);
    }

    /* Assert full forward traversal matches expected element names. */
    void assertAllElements(const char *expected[], size_t count) {
        OrderedIndexIterator it;
        api.initIterator(&it, oi);
        OrderedIndexItem *pos;
        size_t i = 0;
        while ((pos = api.next(&it)) != NULL) {
            ASSERT_LT(i, count) << "More elements than expected";
            assertElement(pos, expected[i]);
            i++;
        }
        api.resetIterator(&it);
        ASSERT_EQ(i, count) << "Fewer elements than expected";
    }

    /* Verify structural integrity. */
    void verifyOI() {
        ASSERT_TRUE(verifyIntegrity(api, oi));
    }
};

/* Variadic macro for clean assertAllElements call sites. */
#define ASSERT_ALL_ELEMENTS(...)                                     \
    do {                                                             \
        const char *_elems[] = {__VA_ARGS__};                        \
        assertAllElements(_elems, sizeof(_elems) / sizeof(*_elems)); \
    } while (0)

/* ========== Basic tests ========== */

TEST_P(OrderedIndexTest, CreateFree) {
    ASSERT_NE(oi, nullptr);
    ASSERT_EQ(api.length(oi), 0UL);
    verifyOI();

    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, InsertSingle) {
    OrderedIndexItem *node = insert(1.0, "test");
    verifyOI();

    ASSERT_NE(node, nullptr);
    ASSERT_EQ(api.length(oi), 1UL);
    assertScore(node, 1.0);
    assertElement(node, "test");

    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    ASSERT_EQ(api.next(&iter), node);
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, InsertMultipleOrdered) {
    populateSequential(10);

    ASSERT_EQ(api.length(oi), 10UL);
    verifyOI();

    /* Verify forward traversal */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    for (int i = 0; i < 10; i++) {
        ASSERT_NE((pos = api.next(&iter)), nullptr);
        ASSERT_DOUBLE_EQ(api.getScore(pos), (double)i);
    }
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);

    /* Verify backward traversal */
    api.initIterator(&iter, oi);
    for (int i = 9; i >= 0; i--) {
        ASSERT_NE((pos = api.prev(&iter)), nullptr);
        ASSERT_DOUBLE_EQ(api.getScore(pos), (double)i);
    }
    ASSERT_EQ(api.prev(&iter), nullptr);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, DuplicateScores) {
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert(1.0, buf);
    }

    ASSERT_EQ(api.length(oi), 5UL);
    verifyOI();

    /* Verify lexicographic ordering for same scores */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        pos = assertNextScore(&iter, 1.0);
        assertElement(pos, buf);
    }
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, IndexOperations) {
    OrderedIndexItem *nodes[10];

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }
    verifyOI();

    for (int i = 0; i < 10; i++) {
        unsigned long idx = api.getIndex(oi, nodes[i]);
        ASSERT_EQ(idx, (unsigned long)i);
    }

    for (int i = 0; i < 10; i++) {
        OrderedIndexItem *node = api.getByIndex(oi, i);
        ASSERT_EQ(node, nodes[i]);
    }
}

TEST_P(OrderedIndexTest, Delete) {
    OrderedIndexItem *nodes[5];
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }
    ASSERT_EQ(api.length(oi), 5UL);

    api.deleteItem(oi, nodes[2]);
    ASSERT_EQ(api.length(oi), 4UL);
    verifyOI();

    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    assertNextScore(&iter, 0.0);
    assertNextScore(&iter, 1.0);
    assertNextScore(&iter, 3.0); /* Skipped 2.0 */
    assertNextScore(&iter, 4.0);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, PopFirst) {
    ASSERT_EQ(api.popFirst(oi), nullptr);

    populateSequential(5);
    ASSERT_EQ(api.length(oi), 5UL);

    OrderedIndexItem *item = api.popFirst(oi);
    ASSERT_NE(item, nullptr);
    assertScore(item, 0.0);
    assertElement(item, "key0");
    api.freeItem(item);
    ASSERT_EQ(api.length(oi), 4UL);
    verifyOI();

    item = api.popFirst(oi);
    assertScore(item, 1.0);
    api.freeItem(item);
    ASSERT_EQ(api.length(oi), 3UL);
    verifyOI();
}

TEST_P(OrderedIndexTest, PopLast) {
    ASSERT_EQ(api.popLast(oi), nullptr);

    populateSequential(5);
    ASSERT_EQ(api.length(oi), 5UL);

    OrderedIndexItem *item = api.popLast(oi);
    ASSERT_NE(item, nullptr);
    assertScore(item, 4.0);
    assertElement(item, "key4");
    api.freeItem(item);
    ASSERT_EQ(api.length(oi), 4UL);
    verifyOI();

    item = api.popLast(oi);
    assertScore(item, 3.0);
    api.freeItem(item);
    ASSERT_EQ(api.length(oi), 3UL);
    verifyOI();
}

TEST_P(OrderedIndexTest, UpdateScore) {
    OrderedIndexItem *node1 = insert(1.0, "key1");
    OrderedIndexItem *node2 = insert(2.0, "key2");
    insert(3.0, "key3");

    OrderedIndexItem *updated = api.updateScore(oi, node2, 4.0);
    ASSERT_NE(updated, nullptr);
    assertScore(updated, 4.0);
    verifyOI();
    assertElement(updated, "key2");

    /* Verify order: key1(1.0), key3(3.0), key2(4.0) */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    assertNextScore(&iter, 1.0);
    assertNextScore(&iter, 3.0);
    pos = assertNextScore(&iter, 4.0);
    assertElement(pos, "key2");
    api.resetIterator(&iter);

    /* Update to same score (no-op) */
    updated = api.updateScore(oi, node1, 1.0);
    assertScore(updated, 1.0);
    verifyOI();
}

TEST_P(OrderedIndexTest, DeleteRangeByScore) {
    populateSequential(10);

    /* Delete range [3, 6] inclusive */
    unsigned long deleted = api.deleteRangeByScore(oi, 3.0, 6.0, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 4UL);
    ASSERT_EQ(api.length(oi), 6UL);
    verifyOI();

    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    for (int i = 0; i < 3; i++) {
        assertNextScore(&iter, (double)i);
    }
    for (int i = 7; i < 10; i++) {
        assertNextScore(&iter, (double)i);
    }
    api.resetIterator(&iter);

    /* Delete with exclusive bounds (2, 8) - should delete 7 */
    deleted = api.deleteRangeByScore(oi, 2.0, 8.0, 1, 1, NULL, NULL);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(api.length(oi), 5UL);
    verifyOI();
}

TEST_P(OrderedIndexTest, DeleteRangeByIndex) {
    populateSequential(10);

    /* Delete indices 2-4 (elements at scores 2,3,4) */
    unsigned long deleted = api.deleteRangeByIndex(oi, 2, 4, NULL, NULL);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(api.length(oi), 7UL);
    verifyOI();

    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    assertNextScore(&iter, 0.0);
    api.resetIterator(&iter);

    /* Verify index 2 is now score 5 (was index 5) */
    OrderedIndexItem *node = api.getByIndex(oi, 2);
    assertScore(node, 5.0);
}

TEST_P(OrderedIndexTest, MixedOperationsIndexIntegrity) {
    OrderedIndexItem *nodes[100];

    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }

    for (int i = 2; i < 100; i += 3) {
        api.deleteItem(oi, nodes[i]);
        nodes[i] = NULL;
    }
    verifyOI();

    /* Update scores of surviving nodes (indices not ≡ 2 mod 3) */
    nodes[10] = api.updateScore(oi, nodes[10], 150.0);
    nodes[19] = api.updateScore(oi, nodes[19], 160.0);
    verifyOI();

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    unsigned long expectedIdx = 0;
    while (((pos = api.next(&iter)) != NULL)) {
        unsigned long actualIdx = api.getIndex(oi, pos);
        ASSERT_EQ(actualIdx, expectedIdx);
        expectedIdx++;
    }
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, BackwardTraversalAfterDeletions) {
    OrderedIndexItem *nodes[20];

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }

    api.deleteItem(oi, nodes[5]);
    api.deleteItem(oi, nodes[10]);
    api.deleteItem(oi, nodes[15]);
    verifyOI();

    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    int expected_scores[] = {19, 18, 17, 16, 14, 13, 12, 11, 9, 8, 7, 6, 4, 3, 2, 1, 0};

    for (int i = 0; i < 17; i++) {
        assertPrevScore(&iter, (double)expected_scores[i]);
    }
    ASSERT_EQ(api.prev(&iter), nullptr);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, LexicographicEdgeCases) {
    insert(1.0, "z");
    insert(1.0, "");
    insert(1.0, "a");

    /* Verify lexicographic order: "", "a", "z" */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, "");
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, "a");
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, "z");
    api.resetIterator(&iter);
    api.free(oi);

    /* Test very long string (1KB) */
    oi = api.create();
    char long_buf[1024];
    memset(long_buf, 'x', 1023);
    long_buf[1023] = '\0';
    insert(1.0, long_buf);
    insert(1.0, "short");

    api.initIterator(&iter, oi);
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, "short");
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, long_buf);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, RangeBoundaryPrecision) {
    double base = 1.0;
    double epsilon = 1e-10;

    insert(base, "at_base");
    insert(base + epsilon, "at_base_plus_epsilon");
    insert(base + 2 * epsilon, "at_base_plus_2epsilon");

    unsigned long deleted = api.deleteRangeByScore(oi, base, base + 2 * epsilon, 1, 1, NULL, NULL);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(api.length(oi), 2UL);

    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    assertNextScore(&iter, base);
    assertNextScore(&iter, base + 2 * epsilon);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, SpecialDoubleValues) {
    insert(NEG_INF, "neg_inf");
    insert(POS_INF, "pos_inf");
    insert(0.0, "zero");
    insert(1.0, "one");

    /* Verify ordering: -inf, 0, 1, +inf */
    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    api.initIterator(&iter, oi);
    assertNextScore(&iter, NEG_INF);
    assertNextScore(&iter, 0.0);
    assertNextScore(&iter, 1.0);
    assertNextScore(&iter, POS_INF);
    api.resetIterator(&iter);
    api.free(oi);

    /* Test +0.0 vs -0.0 */
    oi = api.create();
    insert(0.0, "pos_zero");
    insert(-0.0, "neg_zero");

    /* Both should be in the list, ordered lexicographically since scores are equal */
    ASSERT_EQ(api.length(oi), 2UL);
    api.initIterator(&iter, oi);
    pos = assertNextScore(&iter, 0.0);
    assertElement(pos, "neg_zero");
    pos = assertNextScore(&iter, 0.0);
    assertElement(pos, "pos_zero");
    api.resetIterator(&iter);
    api.free(oi);

    /* Test denormalized double */
    oi = api.create();
    double denorm = 1e-320; /* Denormalized double */
    insert(denorm, "denorm");
    insert(1.0, "normal");

    ASSERT_EQ(api.length(oi), 2UL);
    api.initIterator(&iter, oi);
    pos = assertNextScore(&iter, denorm);
    ASSERT_TRUE(api.getScore(pos) < 1.0);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, EmptyIndexOperations) {
    ASSERT_EQ(api.length(oi), 0UL);
    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    ASSERT_EQ(api.next(&iter), nullptr);
    ASSERT_EQ(api.prev(&iter), nullptr);
    api.resetIterator(&iter);
    ASSERT_EQ(api.getByIndex(oi, 0), nullptr);
}

TEST_P(OrderedIndexTest, DeleteEdgeCases) {
    /* Delete only element */
    OrderedIndexItem *node = insert(1.0, "only");
    api.deleteItem(oi, node);
    ASSERT_EQ(api.length(oi), 0UL);
    verifyOI();
    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);

    /* Delete first element */
    OrderedIndexItem *nodes[3];
    for (int i = 0; i < 3; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }
    api.deleteItem(oi, nodes[0]);
    ASSERT_EQ(api.length(oi), 2UL);
    verifyOI();
    api.initIterator(&iter, oi);
    assertNextScore(&iter, 1.0);
    api.resetIterator(&iter);

    /* Delete last element */
    api.deleteItem(oi, nodes[2]);
    ASSERT_EQ(api.length(oi), 1UL);
    verifyOI();
    api.initIterator(&iter, oi);
    assertPrevScore(&iter, 1.0);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, IndexEdgeCases) {
    populateSequential(5);

    ASSERT_EQ(api.getByIndex(oi, 5), nullptr);
    ASSERT_EQ(api.getByIndex(oi, 99), nullptr);
    ASSERT_NE(api.getByIndex(oi, 0), nullptr);
    ASSERT_NE(api.getByIndex(oi, 4), nullptr);
}

TEST_P(OrderedIndexTest, DuplicateInsert) {
    OrderedIndexItem *node1 = insert(1.0, "duplicate");
    OrderedIndexItem *node2 = insert(1.0, "duplicate");

    /* Should have 2 nodes (duplicates allowed) */
    ASSERT_EQ(api.length(oi), 2UL);
    ASSERT_NE(node1, node2);
}

TEST_P(OrderedIndexTest, UpdateScoreEdgeCases) {
    populateSequential(5); /* scores: 0, 1, 2, 3, 4 */

    /* Move first element to last position */
    OrderedIndexItem *first = api.getByIndex(oi, 0);
    OrderedIndexItem *updated = api.updateScore(oi, first, 10.0);
    assertScore(updated, 10.0);
    verifyOI();
    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    ASSERT_EQ(api.prev(&iter), updated); /* now last */
    api.resetIterator(&iter);

    /* Move last element to first position */
    OrderedIndexItem *last = api.getByIndex(oi, api.length(oi) - 1);
    updated = api.updateScore(oi, last, -1.0);
    assertScore(updated, -1.0);
    verifyOI();
    api.initIterator(&iter, oi);
    ASSERT_EQ(api.next(&iter), updated); /* now first */
    api.resetIterator(&iter);

    /* Move middle element backward past multiple */
    OrderedIndexItem *middle = api.getByIndex(oi, 2);
    updated = api.updateScore(oi, middle, 0.5);
    assertScore(updated, 0.5);
    verifyOI();
    ASSERT_EQ(api.getIndex(oi, updated), 1UL); /* moved from index 2 to 1 */

    /* Update score without changing position (stays between neighbors) */
    OrderedIndexItem *node = api.getByIndex(oi, 3);
    unsigned long idx_before = api.getIndex(oi, node);
    updated = api.updateScore(oi, node, api.getScore(node) + 0.1);
    ASSERT_EQ(api.getIndex(oi, updated), idx_before);
    verifyOI();
}

TEST_P(OrderedIndexTest, RangeDeleteEdgeCases) {
    populateSequential(10);

    /* Delete empty range (min > max) */
    unsigned long deleted = api.deleteRangeByScore(oi, 5.0, 4.0, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(api.length(oi), 10UL);

    /* Delete range with no matches */
    deleted = api.deleteRangeByScore(oi, 10.5, 11.5, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(api.length(oi), 10UL);

    /* Delete first elements by index */
    deleted = api.deleteRangeByIndex(oi, 0, 1, NULL, NULL);
    ASSERT_EQ(deleted, 2UL);
    verifyOI();
    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    assertNextScore(&iter, 2.0);
    api.resetIterator(&iter);

    /* Delete last elements by index */
    unsigned long len = api.length(oi);
    deleted = api.deleteRangeByIndex(oi, len - 2, len - 1, NULL, NULL);
    ASSERT_EQ(deleted, 2UL);
    verifyOI();
    api.initIterator(&iter, oi);
    assertPrevScore(&iter, 7.0);
    api.resetIterator(&iter);

    /* Delete entire remaining index by score */
    deleted = api.deleteRangeByScore(oi, -100.0, 100.0, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 6UL);
    ASSERT_EQ(api.length(oi), 0UL);
    verifyOI();
}

TEST_P(OrderedIndexTest, TraversalEdgeCases) {
    insert(1.0, "single");

    OrderedIndexIterator iter;
    api.initIterator(&iter, oi);
    assertNextScore(&iter, 1.0);
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    assertPrevScore(&iter, 1.0);
    ASSERT_EQ(api.prev(&iter), nullptr);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, SeekToIndex) {
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert((double)i, buf);
    }

    OrderedIndexIterator iter;

    /* Seek to index 0 (first element) */
    api.initIterator(&iter, oi);
    api.seekToIndex(&iter, 0);
    assertNextScore(&iter, 2.0); /* next after first = second */
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    api.seekToIndex(&iter, 0);
    assertPrevScore(&iter, 1.0); /* prev at first = first itself */
    api.resetIterator(&iter);

    /* Seek to index 1 (second element) */
    api.initIterator(&iter, oi);
    api.seekToIndex(&iter, 1);
    assertNextScore(&iter, 3.0);
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    api.seekToIndex(&iter, 1);
    assertPrevScore(&iter, 2.0);
    api.resetIterator(&iter);

    /* Seek to index 2 (middle) */
    api.initIterator(&iter, oi);
    api.seekToIndex(&iter, 2);
    assertNextScore(&iter, 4.0);
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    api.seekToIndex(&iter, 2);
    assertPrevScore(&iter, 3.0);
    api.resetIterator(&iter);

    /* Seek to index 4 (last) */
    api.initIterator(&iter, oi);
    api.seekToIndex(&iter, 4);
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);

    api.initIterator(&iter, oi);
    api.seekToIndex(&iter, 4);
    assertPrevScore(&iter, 5.0);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, ReverseIteration) {
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert((double)i, buf);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Full reverse traversal */
    api.initIterator(&iter, oi);
    int count = 0;
    double expected = 5.0;
    while (((pos = api.prev(&iter)) != NULL)) {
        assertScore(pos, expected);
        expected -= 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    api.resetIterator(&iter);

    /* Reverse then forward */
    api.initIterator(&iter, oi);
    assertPrevScore(&iter, 5.0);
    assertNextScore(&iter, 5.0);
    api.resetIterator(&iter);

    /* Forward then reverse */
    api.initIterator(&iter, oi);
    assertNextScore(&iter, 1.0);
    assertPrevScore(&iter, 1.0);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, SeekToScoreRange) {
    /* Insert elements with scores 0,2,4,6,8 */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert((double)(i * 2), buf);
    }

    OrderedIndexIterator iter;

    /* Seek to first in range [2, 6] with offset 0 */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, 0);
    assertNextScore(&iter, 2.0);
    api.resetIterator(&iter);

    /* Seek to second in range [2, 6] with offset 1 */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, 1);
    assertNextScore(&iter, 4.0);
    api.resetIterator(&iter);

    /* Seek to last in range [2, 6] with offset -1, positioned for prev() */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, -1);
    assertPrevScore(&iter, 6.0);
    api.resetIterator(&iter);

    /* Seek with exclusive bounds (2, 6) - should start at 4 */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 1, 1, 0);
    assertNextScore(&iter, 4.0);
    api.resetIterator(&iter);

    /* Seek to empty range above all elements */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 10.0, 20.0, 0, 0, 0);
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);

    /* Seek to empty range below all elements */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, -20.0, -10.0, 0, 0, 0);
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);

    /* Out of range positive offset */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, 10);
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);

    /* Negative offset beyond range */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, -10);
    ASSERT_EQ(api.prev(&iter), nullptr);
    api.resetIterator(&iter);

    /* Second from last with offset -2, positioned for prev() */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 6.0, 0, 0, -2);
    assertPrevScore(&iter, 4.0);
    api.resetIterator(&iter);

    /* Empty range where min > max */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 6.0, 2.0, 0, 0, 0);
    ASSERT_EQ(api.next(&iter), nullptr);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, SeekToScoreRangeIteration) {
    populateSequential(10);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Seek to range [3, 7] and iterate forward */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 3.0, 7.0, 0, 0, 0);
    int count = 0;
    double expected = 3.0;
    while (((pos = api.next(&iter)) != NULL) && api.getScore(pos) <= 7.0) {
        assertScore(pos, expected);
        expected += 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    api.resetIterator(&iter);

    /* Seek to last in range and iterate backward */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 3.0, 7.0, 0, 0, -1);
    count = 0;
    expected = 7.0;
    while (((pos = api.prev(&iter)) != NULL) && api.getScore(pos) >= 3.0) {
        assertScore(pos, expected);
        expected -= 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    api.resetIterator(&iter);

    /* Seek with offset and continue iteration */
    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, 2.0, 8.0, 0, 0, 2);
    assertNextScore(&iter, 4.0);
    assertNextScore(&iter, 5.0);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, SeekInfReverseIteration) {
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert((double)i, buf);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, NEG_INF, POS_INF, 0, 0, -1);
    int count = 0;
    double expected = 5.0;
    while (((pos = api.prev(&iter)) != NULL)) {
        assertScore(pos, expected);
        expected -= 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, SeekInfForwardIteration) {
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert((double)i, buf);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    api.initIterator(&iter, oi);
    api.seekToScoreRange(&iter, NEG_INF, POS_INF, 0, 0, 0);
    int count = 0;
    double expected = 1.0;
    while (((pos = api.next(&iter)) != NULL)) {
        assertScore(pos, expected);
        expected += 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    api.resetIterator(&iter);
}

TEST_P(OrderedIndexTest, SeekToLexRange) {
    for (int i = 0; i < FRUITS_COUNT; i++) insert(1.0, FRUITS[i]);

    OrderedIndexIterator it;
    OrderedIndexItem *pos;

    /* Seek to first in lex range [banana, date] with offset 0 */
    api.initIterator(&it, oi);
    seekToLexRange(&it, "banana", "date", 0, 0, 0);
    ASSERT_NE((pos = api.next(&it)), nullptr);
    assertElement(pos, "banana");
    api.resetIterator(&it);

    /* Seek to second in lex range with offset 1 */
    api.initIterator(&it, oi);
    seekToLexRange(&it, "banana", "date", 0, 0, 1);
    ASSERT_NE((pos = api.next(&it)), nullptr);
    assertElement(pos, "cherry");
    api.resetIterator(&it);

    /* Seek to last in lex range with offset -1, positioned for prev() */
    api.initIterator(&it, oi);
    seekToLexRange(&it, "banana", "date", 0, 0, -1);
    ASSERT_NE((pos = api.prev(&it)), nullptr);
    assertElement(pos, "date");
    api.resetIterator(&it);

    /* Seek with exclusive bounds (banana, date) - should start at cherry */
    api.initIterator(&it, oi);
    seekToLexRange(&it, "banana", "date", 1, 1, 0);
    ASSERT_NE((pos = api.next(&it)), nullptr);
    assertElement(pos, "cherry");
    api.resetIterator(&it);

    /* Seek to empty lex range */
    api.initIterator(&it, oi);
    seekToLexRange(&it, "zzz", "zzzz", 0, 0, 0);
    ASSERT_EQ(api.next(&it), nullptr);
    api.resetIterator(&it);

    /* Out of range positive offset */
    api.initIterator(&it, oi);
    seekToLexRange(&it, "banana", "date", 0, 0, 10);
    ASSERT_EQ(api.next(&it), nullptr);
    api.resetIterator(&it);
}

TEST_P(OrderedIndexTest, DeleteRangeByLexInclusive) {
    for (int i = 0; i < FRUITS_COUNT; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("banana", "date", 0, 0, NULL, NULL), 3UL);
    ASSERT_EQ(api.length(oi), 2UL);
    verifyOI();
    ASSERT_ALL_ELEMENTS("apple", "elderberry");
}

TEST_P(OrderedIndexTest, DeleteRangeByLexExclusive) {
    for (int i = 0; i < FRUITS_COUNT; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("banana", "date", 1, 1, NULL, NULL), 1UL);
    ASSERT_EQ(api.length(oi), 4UL);
    ASSERT_ALL_ELEMENTS("apple", "banana", "date", "elderberry");
}

TEST_P(OrderedIndexTest, DeleteRangeByLex_EmptyRange) {
    for (int i = 0; i < 3; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("zzz", "aaa", 0, 0, NULL, NULL), 0UL);
    ASSERT_EQ(api.length(oi), 3UL);
}

TEST_P(OrderedIndexTest, DeleteRangeByLex_All) {
    for (int i = 0; i < 3; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("a", "z", 0, 0, NULL, NULL), 3UL);
    ASSERT_EQ(api.length(oi), 0UL);
}

TEST_P(OrderedIndexTest, DeleteRangeByLex_SingleElement) {
    for (int i = 0; i < 3; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("banana", "banana", 0, 0, NULL, NULL), 1UL);
    ASSERT_EQ(api.length(oi), 2UL);
    ASSERT_ALL_ELEMENTS("apple", "cherry");
}

TEST_P(OrderedIndexTest, DeleteRangeByLexPreservesOutside) {
    for (int i = 0; i < NATO_COUNT; i++) insert(1.0, NATO[i]);

    ASSERT_EQ(deleteLexRange("charlie", "delta", 0, 0, NULL, NULL), 2UL);
    ASSERT_EQ(api.length(oi), 4UL);
    ASSERT_ALL_ELEMENTS("alpha", "bravo", "echo", "foxtrot");

    /* Verify scores are preserved */
    OrderedIndexIterator it;
    api.initIterator(&it, oi);
    OrderedIndexItem *pos;
    while ((pos = api.next(&it)) != NULL) {
        assertScore(pos, 1.0);
    }
    api.resetIterator(&it);

    /* Verify indices are correct after deletion */
    for (unsigned long r = 0; r < 4; r++) {
        OrderedIndexItem *node = api.getByIndex(oi, r);
        ASSERT_NE(node, nullptr);
        ASSERT_EQ(api.getIndex(oi, node), r);
    }
}

TEST_P(OrderedIndexTest, LexRangeSentinels) {
    /* Insert 5 elements at the same score (lex ordering) */
    insert(0.0, "alpha");
    insert(0.0, "bravo");
    insert(0.0, "charlie");
    insert(0.0, "delta");
    insert(0.0, "echo");

    sds charlie = sdsnew("charlie");

    /* Count with sentinels */
    ASSERT_EQ(countLexRange("minstring", "maxstring", 0, 0), 0UL); /* literal strings, not sentinels */
    ASSERT_EQ(api.countLexRange(oi, shared.minstring, shared.maxstring, 0, 0), 5UL);
    ASSERT_EQ(api.countLexRange(oi, shared.minstring, charlie, 0, 0), 3UL);
    ASSERT_EQ(api.countLexRange(oi, charlie, shared.maxstring, 0, 0), 3UL);

    /* Inverted range (max < min sentinel) should return 0 */
    ASSERT_EQ(api.countLexRange(oi, shared.maxstring, shared.minstring, 0, 0), 0UL);
    ASSERT_EQ(api.countLexRange(oi, charlie, shared.minstring, 0, 0), 0UL);

    /* Seek with sentinels - iterate all */
    OrderedIndexIterator it;
    api.initIterator(&it, oi);
    api.seekToLexRange(&it, shared.minstring, shared.maxstring, 0, 0, 0);
    assertNextScore(&it, 0.0); /* alpha */
    assertNextScore(&it, 0.0); /* bravo */
    assertNextScore(&it, 0.0); /* charlie */
    assertNextScore(&it, 0.0); /* delta */
    assertNextScore(&it, 0.0); /* echo */
    ASSERT_EQ(api.next(&it), nullptr);
    api.resetIterator(&it);

    /* Delete with sentinels - delete all */
    ASSERT_EQ(api.deleteRangeByLex(oi, shared.minstring, shared.maxstring, 0, 0, NULL, NULL), 5UL);
    ASSERT_EQ(api.length(oi), 0UL);

    sdsfree(charlie);
}

/* ========== Randomized property tests ========== */

/* Default fuzz seed — overridden by --seed flag if provided. */
extern char *seed;
static uint32_t test_fuzz_seed(void) {
    uint32_t s = seed ? (uint32_t)atoi(seed) : 42;
    printf("  [fuzz seed: %u]\n", s);
    return s;
}

/* Simple xorshift32 PRNG — deterministic and seedable. */
static uint32_t test_rand_next(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static int test_rand_range(uint32_t *state, int min, int max) {
    return min + (int)(test_rand_next(state) % (uint32_t)(max - min + 1));
}

static double test_rand_double(uint32_t *state, double lo, double hi) {
    return lo + (hi - lo) * ((double)test_rand_next(state) / (double)UINT32_MAX);
}

struct RandomIndexEntry {
    OrderedIndexItem *node;
    double score;
    sds element;
};

static sds test_random_element(uint32_t *state, int maxLen) {
    int len = test_rand_range(state, 1, maxLen);
    sds s = sdsnewlen(NULL, len);
    for (int i = 0; i < len; i++) s[i] = (char)test_rand_range(state, 'a', 'z');
    return s;
}

static double test_random_score(uint32_t *state) {
    return test_rand_double(state, -1e6, 1e6);
}

static RandomIndexEntry *test_build_random_index(OrderedIndexTestApi &api, OrderedIndex *oi, uint32_t *state, int count) {
    RandomIndexEntry *entries = (RandomIndexEntry *)zmalloc(sizeof(RandomIndexEntry) * count);
    for (int i = 0; i < count; i++) {
        double score = test_random_score(state);
        sds elem = test_random_element(state, 16);
        /* Append index to ensure uniqueness */
        elem = sdscatfmt(elem, "%i", i);
        OrderedIndexItem *node = api.insert(oi, score, elem, sdslen(elem));
        entries[i] = {node, score, elem};
    }
    return entries;
}

static void freeRandomEntries(RandomIndexEntry *entries, int count) {
    for (int i = 0; i < count; i++) sdsfree(entries[i].element);
    zfree(entries);
}

TEST_P(OrderedIndexTest, RandomizedInsertAndTraversal) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        {
            RandomIndexEntry *_e = test_build_random_index(api, oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        ASSERT_EQ(api.length(oi), (unsigned long)n);
        verifyOI();

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
        oi = api.create();
    }
}

TEST_P(OrderedIndexTest, RandomizedBackwardTraversal) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        {
            RandomIndexEntry *_e = test_build_random_index(api, oi, &rng, n);
            freeRandomEntries(_e, n);
        }

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
        oi = api.create();
    }
}

TEST_P(OrderedIndexTest, RandomizedScoreRetrieval) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        RandomIndexEntry *entries = test_build_random_index(api, oi, &rng, n);

        for (int i = 0; i < n; i++) {
            assertScore(entries[i].node, entries[i].score);
        }
        freeRandomEntries(entries, n);
        api.free(oi);
        oi = api.create();
    }
}

TEST_P(OrderedIndexTest, RandomizedIndexConsistency) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        {
            RandomIndexEntry *_e = test_build_random_index(api, oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        unsigned long expectedIdx = 0;
        while (((pos = api.next(&iter)) != NULL)) {
            unsigned long idx = api.getIndex(oi, pos);
            ASSERT_EQ(idx, expectedIdx);
            OrderedIndexItem *byIdx = api.getByIndex(oi, expectedIdx);
            ASSERT_EQ(byIdx, pos);
            expectedIdx++;
        }
        ASSERT_EQ(expectedIdx, (unsigned long)n);
        api.resetIterator(&iter);
        api.free(oi);
        oi = api.create();
    }
}

TEST_P(OrderedIndexTest, RandomizedDelete) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 2, 30);

        RandomIndexEntry *entries = test_build_random_index(api, oi, &rng, n);

        int delIdx = test_rand_range(&rng, 0, n - 1);
        api.deleteItem(oi, entries[delIdx].node);

        ASSERT_EQ(api.length(oi), (unsigned long)(n - 1));
        verifyOI();

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
        oi = api.create();
    }
}

TEST_P(OrderedIndexTest, RandomizedUpdateScore) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 2, 30);

        RandomIndexEntry *entries = test_build_random_index(api, oi, &rng, n);

        int updIdx = test_rand_range(&rng, 0, n - 1);
        double newScore = test_random_score(&rng);

        OrderedIndexItem *updated = api.updateScore(oi, entries[updIdx].node, newScore);
        ASSERT_NE(updated, nullptr);
        assertScore(updated, newScore);
        ASSERT_EQ(api.length(oi), (unsigned long)n);
        verifyOI();

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
        oi = api.create();
    }
}

TEST_P(OrderedIndexTest, RandomizedPop) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 10; trial++) {
        int n = test_rand_range(&rng, 3, 30);

        {
            RandomIndexEntry *_e = test_build_random_index(api, oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        api.initIterator(&iter, oi);
        ASSERT_NE((pos = api.next(&iter)), nullptr);
        double minScore = api.getScore(pos);
        api.resetIterator(&iter);

        api.initIterator(&iter, oi);
        ASSERT_NE((pos = api.prev(&iter)), nullptr);
        double maxScore = api.getScore(pos);
        api.resetIterator(&iter);

        OrderedIndexItem *first = api.popFirst(oi);
        ASSERT_NE(first, nullptr);
        assertScore(first, minScore);
        ASSERT_EQ(api.length(oi), (unsigned long)(n - 1));
        api.freeItem(first);
        verifyOI();

        OrderedIndexItem *last = api.popLast(oi);
        ASSERT_NE(last, nullptr);
        assertScore(last, maxScore);
        ASSERT_EQ(api.length(oi), (unsigned long)(n - 2));
        api.freeItem(last);
        verifyOI();

        api.initIterator(&iter, oi);
        double prevScore = NEG_INF;
        while (((pos = api.next(&iter)) != NULL)) {
            ASSERT_GE(api.getScore(pos), prevScore);
            prevScore = api.getScore(pos);
        }
        api.resetIterator(&iter);
        api.free(oi);
        oi = api.create();
    }
}

TEST_P(OrderedIndexTest, RandomizedDeleteRangeByScore) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 5, 40);

        RandomIndexEntry *entries = test_build_random_index(api, oi, &rng, n);

        double s1 = test_random_score(&rng), s2 = test_random_score(&rng);
        double lo = TEST_MIN(s1, s2), hi = TEST_MAX(s1, s2);

        int expectedDeleted = 0;
        for (int i = 0; i < n; i++) {
            if (entries[i].score >= lo && entries[i].score <= hi) expectedDeleted++;
        }

        unsigned long deleted = api.deleteRangeByScore(oi, lo, hi, 0, 0, NULL, NULL);
        ASSERT_EQ(deleted, (unsigned long)expectedDeleted);
        ASSERT_EQ(api.length(oi), (unsigned long)(n - expectedDeleted));
        verifyOI();

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
        oi = api.create();
    }
}

TEST_P(OrderedIndexTest, RandomizedDeleteRangeByIndex) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 5, 40);

        {
            RandomIndexEntry *_e = test_build_random_index(api, oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        int r1 = test_rand_range(&rng, 0, n - 1), r2 = test_rand_range(&rng, 0, n - 1);
        unsigned long start = (unsigned long)TEST_MIN(r1, r2);
        unsigned long end = (unsigned long)TEST_MAX(r1, r2);
        unsigned long expectedDeleted = end - start + 1;

        unsigned long deleted = api.deleteRangeByIndex(oi, start, end, NULL, NULL);
        ASSERT_EQ(deleted, expectedDeleted);
        ASSERT_EQ(api.length(oi), (unsigned long)(n)-expectedDeleted);
        verifyOI();

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
        oi = api.create();
    }
}

TEST_P(OrderedIndexTest, RandomizedForwardBackwardMirror) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        {
            RandomIndexEntry *_e = test_build_random_index(api, oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        double *forwardScores = (double *)zmalloc(sizeof(double) * n);
        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        int fi = 0;
        api.initIterator(&iter, oi);
        while (((pos = api.next(&iter)) != NULL)) {
            forwardScores[fi++] = api.getScore(pos);
        }
        api.resetIterator(&iter);

        double *backwardScores = (double *)zmalloc(sizeof(double) * n);
        int bi = 0;
        api.initIterator(&iter, oi);
        while (((pos = api.prev(&iter)) != NULL)) {
            backwardScores[bi++] = api.getScore(pos);
        }
        api.resetIterator(&iter);

        ASSERT_EQ(fi, bi);
        reverseDoubleArray(backwardScores, bi);
        for (int i = 0; i < fi; i++) {
            ASSERT_DOUBLE_EQ(forwardScores[i], backwardScores[i]);
        }
        zfree(forwardScores);
        zfree(backwardScores);
        api.free(oi);
        oi = api.create();
    }
}

/* ========== Count range tests ========== */

TEST_P(OrderedIndexTest, CountScoreRange) {
    populateSequential(10);

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
}

TEST_P(OrderedIndexTest, CountScoreRangeEmpty) {
    ASSERT_EQ(api.countScoreRange(oi, NEG_INF, POS_INF, 0, 0), 0UL);
}

TEST_P(OrderedIndexTest, CountLexRange) {
    for (int i = 0; i < FRUITS_COUNT; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(countLexRange("banana", "date", 0, 0), 3UL);   /* Inclusive [banana, date] */
    ASSERT_EQ(countLexRange("banana", "date", 1, 1), 1UL);   /* Exclusive (banana, date) */
    ASSERT_EQ(countLexRange("cherry", "cherry", 0, 0), 1UL); /* Single element */
    ASSERT_EQ(countLexRange("fig", "grape", 0, 0), 0UL);     /* No match */
    ASSERT_EQ(countLexRange("a", "z", 0, 0), 5UL);           /* All elements */
}

TEST_P(OrderedIndexTest, CountLexRangeEmpty) {
    ASSERT_EQ(countLexRange("a", "z", 0, 0), 0UL);
}

/* ========== Instantiate parameterized tests for all implementations ========== */

INSTANTIATE_TEST_SUITE_P(AllImplementations,
                         OrderedIndexTest,
                         ::testing::Values(&skiplistImpl),
                         orderedIndexTestName);

/* ========== On-Delete Callback Tests ========== */

struct OnDeleteRecord {
    int count;
    int capacity;
    sds *elements; /* Fixed-size array allocated at init */
};

static void initOnDeleteRecord(OnDeleteRecord *rec, int capacity) {
    rec->count = 0;
    rec->capacity = capacity;
    rec->elements = (sds *)zmalloc(sizeof(sds) * capacity);
}

static void freeOnDeleteRecord(OnDeleteRecord *rec) {
    for (int i = 0; i < rec->count; i++) sdsfree(rec->elements[i]);
    zfree(rec->elements);
}

static void testOnDeleteCallback(OrderedIndexItem *item, void *ctx) {
    OnDeleteRecord *rec = (OnDeleteRecord *)ctx;
    const char *ptr;
    size_t len;
    skiplistGetElementRaw(item, &ptr, &len);
    rec->elements[rec->count] = sdsnewlen(ptr, len);
    rec->count++;
    /* Item is freed by the index after this callback returns. */
}

class OnDeleteCallbackTest : public ::testing::Test {
  protected:
    SkiplistOrderedIndex api;
    OrderedIndex *oi = nullptr;

    void SetUp() override {
        oi = api.create();
    }
    void TearDown() override {
        if (oi) api.free(oi);
    }

    void verifyOI() {
        char errmsg[256];
        ASSERT_TRUE(api.verifyIntegrity(oi, errmsg, sizeof(errmsg))) << errmsg;
    }

    void insert(double score, const char *ele) {
        api.insert(oi, score, ele, strlen(ele));
    }

    void insertN(int n) {
        for (int i = 0; i < n; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key%d", i);
            insert((double)i, buf);
        }
    }

    void insertLex(const char *elems[], int count, double score = 1.0) {
        for (int i = 0; i < count; i++) {
            insert(score, elems[i]);
        }
    }

    /* Collect elements into caller-owned sds array. Caller must freeSdsArray(). */
    sds *collectElements(OrderedIndex *idx, size_t *out_n) {
        return collectIndexToSds(api, idx, out_n);
    }
};

/* DeleteRangeByScore */

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_EmptyAndNoMatch) {
    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);

    unsigned long deleted = api.deleteRangeByScore(oi, 0.0, 10.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    api.free(oi);

    oi = api.create();
    insertN(5);
    rec = {0, {}};
    deleted = api.deleteRangeByScore(oi, 10.0, 20.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(api.length(oi), 5UL);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_Subset) {
    insertN(10);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = api.deleteRangeByScore(oi, 3.0, 6.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 4UL);
    ASSERT_EQ(rec.count, 4);
    ASSERT_EQ(api.length(oi), 6UL);
    verifyOI();

    sortSdsArray(rec.elements, rec.count);
    ASSERT_SDS_ARRAY_EQ(rec.elements, rec.count, "key3", "key4", "key5", "key6");

    {
        size_t _rn;
        sds *_r = collectElements(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "key0", "key1", "key2", "key7", "key8", "key9");
        freeSdsArray(_r, _rn);
    }
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_All) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = api.deleteRangeByScore(oi, NEG_INF, POS_INF, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 5UL);
    ASSERT_EQ(rec.count, 5);
    ASSERT_EQ(api.length(oi), 0UL);
    verifyOI();
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_NullCallback) {
    insertN(5);

    unsigned long deleted = api.deleteRangeByScore(oi, 1.0, 3.0, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(api.length(oi), 2UL);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_ExclusiveBounds) {
    insertN(10);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = api.deleteRangeByScore(oi, 3.0, 7.0, 1, 1, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    sortSdsArray(rec.elements, rec.count);
    ASSERT_SDS_ARRAY_EQ(rec.elements, rec.count, "key4", "key5", "key6");
    ASSERT_EQ(api.length(oi), 7UL);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByScore_SingleElement) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = api.deleteRangeByScore(oi, 2.0, 2.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "key2");
    ASSERT_EQ(api.length(oi), 4UL);
}

/* DeleteRangeByIndex */

TEST_F(OnDeleteCallbackTest, DeleteRangeByIndex_EmptyAndNoMatch) {
    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);

    unsigned long deleted = api.deleteRangeByIndex(oi, 0, 4, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    api.free(oi);

    oi = api.create();
    insertN(3);
    rec = {0, {}};
    deleted = api.deleteRangeByIndex(oi, 10, 20, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(api.length(oi), 3UL);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByIndex_Subset) {
    insertN(10);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = api.deleteRangeByIndex(oi, 2, 4, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    ASSERT_EQ(api.length(oi), 7UL);

    sortSdsArray(rec.elements, rec.count);
    ASSERT_SDS_ARRAY_EQ(rec.elements, rec.count, "key2", "key3", "key4");

    {
        size_t _rn;
        sds *_r = collectElements(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "key0", "key1", "key5", "key6", "key7", "key8", "key9");
        freeSdsArray(_r, _rn);
    }
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByIndex_All) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = api.deleteRangeByIndex(oi, 0, 4, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 5UL);
    ASSERT_EQ(rec.count, 5);
    ASSERT_EQ(api.length(oi), 0UL);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByIndex_NullCallback) {
    insertN(5);

    unsigned long deleted = api.deleteRangeByIndex(oi, 2, 4, NULL, NULL);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(api.length(oi), 2UL);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByIndex_ExclusiveBounds) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = api.deleteRangeByIndex(oi, 2, 2, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "key2");

    {
        size_t _rn;
        sds *_r = collectElements(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "key0", "key1", "key3", "key4");
        freeSdsArray(_r, _rn);
    }
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByIndex_SingleElement) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = api.deleteRangeByIndex(oi, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "key0");
    ASSERT_EQ(api.length(oi), 4UL);
}

/* DeleteRangeByLex */

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_EmptyAndNoMatch) {
    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);

    sds min = sdsnew("a");
    sds max = sdsnew("z");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    sdsfree(min);
    sdsfree(max);
    api.free(oi);

    oi = api.create();
    {
        const char *_l[] = {"apple", "banana", "cherry"};
        insertLex(_l, 3);
    }
    rec = {0, {}};
    min = sdsnew("x");
    max = sdsnew("z");
    deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(api.length(oi), 3UL);
    sdsfree(min);
    sdsfree(max);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_Subset) {
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        insertLex(_l, 5);
    }

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    ASSERT_EQ(api.length(oi), 2UL);

    sortSdsArray(rec.elements, rec.count);
    ASSERT_SDS_ARRAY_EQ(rec.elements, rec.count, "banana", "cherry", "date");

    {
        size_t _rn;
        sds *_r = collectElements(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "apple", "elderberry");
        freeSdsArray(_r, _rn);
    }

    sdsfree(min);
    sdsfree(max);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_All) {
    {
        const char *_l[] = {"apple", "banana", "cherry"};
        insertLex(_l, 3);
    }

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    sds min = sdsnew("a");
    sds max = sdsnew("z");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    ASSERT_EQ(api.length(oi), 0UL);

    sdsfree(min);
    sdsfree(max);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_NullCallback) {
    {
        const char *_l[] = {"apple", "banana", "cherry", "date"};
        insertLex(_l, 4);
    }

    sds min = sdsnew("banana");
    sds max = sdsnew("cherry");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 2UL);
    ASSERT_EQ(api.length(oi), 2UL);

    {
        size_t _rn;
        sds *_r = collectElements(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "apple", "date");
        freeSdsArray(_r, _rn);
    }

    sdsfree(min);
    sdsfree(max);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_ExclusiveBounds) {
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        insertLex(_l, 5);
    }

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 1, 1, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "cherry");
    ASSERT_EQ(api.length(oi), 4UL);

    {
        size_t _rn;
        sds *_r = collectElements(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "apple", "banana", "date", "elderberry");
        freeSdsArray(_r, _rn);
    }

    sdsfree(min);
    sdsfree(max);
}

TEST_F(OnDeleteCallbackTest, DeleteRangeByLex_SingleElement) {
    {
        const char *_l[] = {"apple", "banana", "cherry"};
        insertLex(_l, 3);
    }

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    sds min = sdsnew("banana");
    sds max = sdsnew("banana");
    unsigned long deleted = api.deleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "banana");
    ASSERT_EQ(api.length(oi), 2UL);

    {
        size_t _rn;
        sds *_r = collectElements(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "apple", "cherry");
        freeSdsArray(_r, _rn);
    }

    sdsfree(min);
    sdsfree(max);
}

/* ========== Range-Delete Hashtable Consistency Tests ========== */

/* Simulated hashtable: sorted sds array (allows set-equality comparison). */
struct SimHt {
    sds *elems;
    int count;
    int capacity;
};

static void simHtInit(SimHt *ht, int cap) {
    ht->elems = (sds *)zmalloc(sizeof(sds) * cap);
    ht->count = 0;
    ht->capacity = cap;
}

static void simHtFree(SimHt *ht) {
    for (int i = 0; i < ht->count; i++) sdsfree(ht->elems[i]);
    zfree(ht->elems);
}

static void simHtAdd(SimHt *ht, const char *s, size_t len) {
    ht->elems[ht->count++] = sdsnewlen(s, len);
}

static void simHtRemove(SimHt *ht, const char *s, size_t len) {
    for (int i = 0; i < ht->count; i++) {
        if (sdslen(ht->elems[i]) == len && memcmp(ht->elems[i], s, len) == 0) {
            sdsfree(ht->elems[i]);
            ht->elems[i] = ht->elems[--ht->count];
            return;
        }
    }
}

static void simHtSort(SimHt *ht) {
    sortSdsArray(ht->elems, ht->count);
}

static void hashtableConsistencyOnDelete(OrderedIndexItem *item, void *ctx) {
    SimHt *ht = (SimHt *)ctx;
    const char *ptr;
    size_t len;
    skiplistGetElementRaw(item, &ptr, &len);
    simHtRemove(ht, ptr, len);
    /* Item is freed by the index after this callback returns. */
}

class RangeDeleteHashtableConsistencyTest : public ::testing::Test {
  protected:
    SkiplistOrderedIndex api;
    OrderedIndex *oi = nullptr;

    void SetUp() override {
        oi = api.create();
    }
    void TearDown() override {
        if (oi) api.free(oi);
    }

    void insert(double score, const char *ele) {
        api.insert(oi, score, ele, strlen(ele));
    }

    void insertN(SimHt &ht, int n) {
        for (int i = 0; i < n; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key%d", i);
            insert((double)i, buf);
            simHtAdd(&ht, buf, strlen(buf));
        }
    }

    void insertLex(SimHt &ht, const char *elems[], int count, double score = 1.0) {
        for (int i = 0; i < count; i++) {
            insert(score, elems[i]);
            simHtAdd(&ht, elems[i], strlen(elems[i]));
        }
    }

    void assertHtMatchesIndex(SimHt &ht) {
        size_t idx_n;
        sds *idx_elems = collectIndexToSds(api, oi, &idx_n);
        sortSdsArray(idx_elems, idx_n);
        simHtSort(&ht);
        ASSERT_EQ(idx_n, (size_t)ht.count);
        for (size_t i = 0; i < idx_n; i++) {
            ASSERT_STREQ(idx_elems[i], ht.elems[i]);
        }
        freeSdsArray(idx_elems, idx_n);
    }
};

/* ByScore */

TEST_F(RangeDeleteHashtableConsistencyTest, ByScore_PartialDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    insertN(simulatedHt, 10);

    api.deleteRangeByScore(oi, 3.0, 6.0, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(simulatedHt);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByScore_FullDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    insertN(simulatedHt, 10);

    api.deleteRangeByScore(oi, NEG_INF, POS_INF, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(simulatedHt);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByScore_EmptyRange) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    insertN(simulatedHt, 10);

    api.deleteRangeByScore(oi, 20.0, 30.0, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(simulatedHt);
}

/* ByIndex */

TEST_F(RangeDeleteHashtableConsistencyTest, ByIndex_PartialDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    insertN(simulatedHt, 10);

    api.deleteRangeByIndex(oi, 2, 4, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(simulatedHt);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByIndex_FullDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    insertN(simulatedHt, 10);

    api.deleteRangeByIndex(oi, 0, 9, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(simulatedHt);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByIndex_EmptyRange) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    insertN(simulatedHt, 10);

    api.deleteRangeByIndex(oi, 20, 30, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(simulatedHt);
}

/* ByLex */

TEST_F(RangeDeleteHashtableConsistencyTest, ByLex_PartialDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        insertLex(simulatedHt, _l, 5);
    }

    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    api.deleteRangeByLex(oi, min, max, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(simulatedHt);

    sdsfree(min);
    sdsfree(max);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByLex_FullDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        insertLex(simulatedHt, _l, 5);
    }

    sds min = sdsnew("a");
    sds max = sdsnew("z");
    api.deleteRangeByLex(oi, min, max, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(simulatedHt);

    sdsfree(min);
    sdsfree(max);
}

TEST_F(RangeDeleteHashtableConsistencyTest, ByLex_EmptyRange) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        insertLex(simulatedHt, _l, 5);
    }

    sds min = sdsnew("zzz");
    sds max = sdsnew("zzzz");
    api.deleteRangeByLex(oi, min, max, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(simulatedHt);

    sdsfree(min);
    sdsfree(max);
}
