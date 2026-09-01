/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "ordered_index.h"
#include "server.h"
}

/* Undefine min/max macros from server.h to avoid conflicts */
#undef min
#undef max

#include <cmath>
#include <cstring>


/* ---- C-style test helpers ---- */

#define TEST_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TEST_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Collect all elements from an ordered index into a pre-allocated sds array. */
/* Collect all elements from an ordered index into a pre-allocated sds array. */
static sds *collectIndexToSds(OrderedIndex *oi, size_t *out_n) {
    size_t n = orderedIndexLength(oi);
    *out_n = n;
    if (n == 0) return NULL;
    sds *arr = (sds *)zmalloc(sizeof(sds) * n);
    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    for (size_t i = 0; i < n; i++) {
        OrderedIndexItem *pos = orderedIndexNext(&iter);
        const char *ptr;
        size_t len;
        orderedIndexItemGetElement(pos, &ptr, &len);
        arr[i] = sdsnewlen(ptr, len);
    }
    orderedIndexResetIterator(&iter);
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
    if (n == 0) return; /* qsort's array argument is declared non-null */
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
static ::testing::AssertionResult verifyIntegrity(OrderedIndex *oi) {
    char errmsg[256];
    if (orderedIndexVerifyIntegrity(oi, errmsg, sizeof(errmsg)))
        return ::testing::AssertionResult(true);
    return ::testing::AssertionFailure() << errmsg;
}

#define VERIFY_INTEGRITY(idx_ptr) ASSERT_TRUE(verifyIntegrity(idx_ptr))

/* Use double infinity to avoid -Wdouble-promotion on macOS where INFINITY is float */
static const double POS_INF = (double)INFINITY;
static const double NEG_INF = (double)-INFINITY;

/* ========== Shared test data ========== */

static const char *FRUITS[] = {"apple", "banana", "cherry", "date", "elderberry"};
static const int FRUITS_COUNT = 5;
static const char *NATO[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot"};
static const int NATO_COUNT = 6;

/* ========== Test fixture ========== */

class OrderedIndexTest : public ::testing::Test {
  protected:
    OrderedIndex *oi = nullptr;

    static void SetUpTestSuite() {
        shared.minstring = sdsnew("minstring");
        shared.maxstring = sdsnew("maxstring");
    }

    static void TearDownTestSuite() {
        sdsfree(shared.minstring);
        shared.minstring = NULL;
        sdsfree(shared.maxstring);
        shared.maxstring = NULL;
    }

    void SetUp() override {
        oi = orderedIndexCreate();
    }
    void TearDown() override {
        if (oi) orderedIndexFree(oi);
    }

    /* Insert a string literal at given score. */
    OrderedIndexItem *insert(double score, const char *ele) {
        sds s = sdsnew(ele);
        OrderedIndexItem *node = orderedIndexInsert(oi, score, s, sdslen(s));
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
        OrderedIndexItem *pos = orderedIndexNext(iter);
        EXPECT_NE(pos, nullptr);
        if (pos) {
            EXPECT_DOUBLE_EQ(orderedIndexItemGetScore(pos), expected);
        }
        return pos;
    }

    /* Assert prev iterator element has expected score. */
    OrderedIndexItem *assertPrevScore(OrderedIndexIterator *iter, double expected) {
        OrderedIndexItem *pos = orderedIndexPrev(iter);
        EXPECT_NE(pos, nullptr);
        if (pos) {
            EXPECT_DOUBLE_EQ(orderedIndexItemGetScore(pos), expected);
        }
        return pos;
    }

    /* Assert element content matches expected string. */
    void assertElement(OrderedIndexItem *node, const char *expected) {
        const char *ptr;
        size_t len;
        orderedIndexItemGetElement(node, &ptr, &len);
        ASSERT_EQ(len, strlen(expected));
        ASSERT_EQ(memcmp(ptr, expected, len), 0);
    }

    /* Assert node has expected score. */
    void assertScore(OrderedIndexItem *node, double expected) {
        ASSERT_DOUBLE_EQ(orderedIndexItemGetScore(node), expected);
    }

    /* Delete lex range using const char* (handles sds lifecycle). */
    unsigned long deleteLexRange(const char *min_str, const char *max_str, int min_ex, int max_ex, OrderedIndexOnDelete cb, void *ctx) {
        sds min = sdsnew(min_str);
        sds max = sdsnew(max_str);
        unsigned long deleted = orderedIndexDeleteRangeByLex(oi, min, max, min_ex, max_ex, cb, ctx);
        sdsfree(min);
        sdsfree(max);
        return deleted;
    }

    /* Count lex range using const char*. */
    unsigned long countLexRange(const char *min_str, const char *max_str, int min_ex, int max_ex) {
        sds min = sdsnew(min_str);
        sds max = sdsnew(max_str);
        unsigned long count = orderedIndexCountLexRange(oi, min, max, min_ex, max_ex);
        sdsfree(min);
        sdsfree(max);
        return count;
    }

    /* Seek to lex range using const char*. */
    void seekToLexRange(OrderedIndexIterator *it, const char *min_str, const char *max_str, int min_ex, int max_ex, long offset) {
        sds min = sdsnew(min_str);
        sds max = sdsnew(max_str);
        orderedIndexSeekToLexRange(it, min, max, min_ex, max_ex, offset);
        sdsfree(min);
        sdsfree(max);
    }

    /* Assert full forward traversal matches expected element names. */
    void assertAllElements(const char *expected[], size_t count) {
        OrderedIndexIterator it;
        orderedIndexInitIterator(&it, oi);
        OrderedIndexItem *pos;
        size_t i = 0;
        while ((pos = orderedIndexNext(&it)) != NULL) {
            ASSERT_LT(i, count) << "More elements than expected";
            assertElement(pos, expected[i]);
            i++;
        }
        orderedIndexResetIterator(&it);
        ASSERT_EQ(i, count) << "Fewer elements than expected";
    }

    /* Insert N sequential elements ("key0"..."keyN-1") at scores 0..N-1 (no return). */
    void insertN(int n) {
        for (int i = 0; i < n; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key%d", i);
            orderedIndexInsert(oi, (double)i, buf, strlen(buf));
        }
    }

    /* Insert elements at the same score (lex ordering). */
    void insertLex(const char *elems[], int count, double score = 1.0) {
        for (int i = 0; i < count; i++) {
            orderedIndexInsert(oi, score, elems[i], strlen(elems[i]));
        }
    }

    /* Verify structural integrity. */
    void verifyOI() {
        ASSERT_TRUE(verifyIntegrity(oi));
    }
};

/* Variadic macro for clean assertAllElements call sites. */
#define ASSERT_ALL_ELEMENTS(...)                                     \
    do {                                                             \
        const char *_elems[] = {__VA_ARGS__};                        \
        assertAllElements(_elems, sizeof(_elems) / sizeof(*_elems)); \
    } while (0)

/* ========== Basic tests ========== */

TEST_F(OrderedIndexTest, CreateFree) {
    ASSERT_NE(oi, nullptr);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
    verifyOI();

    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, InsertSingle) {
    OrderedIndexItem *node = insert(1.0, "test");
    verifyOI();

    ASSERT_NE(node, nullptr);
    ASSERT_EQ(orderedIndexLength(oi), 1UL);
    assertScore(node, 1.0);
    assertElement(node, "test");

    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    ASSERT_EQ(orderedIndexNext(&iter), node);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, InsertMultipleOrdered) {
    populateSequential(10);

    ASSERT_EQ(orderedIndexLength(oi), 10UL);
    verifyOI();

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Verify forward traversal */
    orderedIndexInitIterator(&iter, oi);
    for (int i = 0; i < 10; i++) {
        ASSERT_NE((pos = orderedIndexNext(&iter)), nullptr);
        ASSERT_DOUBLE_EQ(orderedIndexItemGetScore(pos), (double)i);
    }
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);


    /* Verify backward traversal */
    orderedIndexInitIterator(&iter, oi);
    for (int i = 9; i >= 0; i--) {
        ASSERT_NE((pos = orderedIndexPrev(&iter)), nullptr);
        ASSERT_DOUBLE_EQ(orderedIndexItemGetScore(pos), (double)i);
    }
    ASSERT_EQ(orderedIndexPrev(&iter), nullptr);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, DuplicateScores) {
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert(1.0, buf);
    }

    ASSERT_EQ(orderedIndexLength(oi), 5UL);
    verifyOI();

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Verify lexicographic ordering for same scores */
    orderedIndexInitIterator(&iter, oi);
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        pos = assertNextScore(&iter, 1.0);
        assertElement(pos, buf);
    }
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, IndexOperations) {
    OrderedIndexItem *nodes[10];

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }
    verifyOI();

    for (int i = 0; i < 10; i++) {
        unsigned long idx = orderedIndexGetIndex(oi, nodes[i]);
        ASSERT_EQ(idx, (unsigned long)i);
    }

    for (int i = 0; i < 10; i++) {
        OrderedIndexItem *node = orderedIndexGetByIndex(oi, i);
        ASSERT_EQ(node, nodes[i]);
    }
}

TEST_F(OrderedIndexTest, Delete) {
    OrderedIndexItem *nodes[5];
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }
    ASSERT_EQ(orderedIndexLength(oi), 5UL);

    orderedIndexDelete(oi, nodes[2]);
    ASSERT_EQ(orderedIndexLength(oi), 4UL);
    verifyOI();

    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    assertNextScore(&iter, 0.0);
    assertNextScore(&iter, 1.0);
    assertNextScore(&iter, 3.0); /* Skipped 2.0 */
    assertNextScore(&iter, 4.0);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, PopFirst) {
    ASSERT_EQ(orderedIndexPopFirst(oi), nullptr);

    populateSequential(5);
    ASSERT_EQ(orderedIndexLength(oi), 5UL);

    OrderedIndexItem *item = orderedIndexPopFirst(oi);
    ASSERT_NE(item, nullptr);
    assertScore(item, 0.0);
    assertElement(item, "key0");
    orderedIndexItemFree(item);
    ASSERT_EQ(orderedIndexLength(oi), 4UL);
    verifyOI();

    item = orderedIndexPopFirst(oi);
    assertScore(item, 1.0);
    orderedIndexItemFree(item);
    ASSERT_EQ(orderedIndexLength(oi), 3UL);
    verifyOI();
}

TEST_F(OrderedIndexTest, PopLast) {
    ASSERT_EQ(orderedIndexPopLast(oi), nullptr);

    populateSequential(5);
    ASSERT_EQ(orderedIndexLength(oi), 5UL);

    OrderedIndexItem *item = orderedIndexPopLast(oi);
    ASSERT_NE(item, nullptr);
    assertScore(item, 4.0);
    assertElement(item, "key4");
    orderedIndexItemFree(item);
    ASSERT_EQ(orderedIndexLength(oi), 4UL);
    verifyOI();

    item = orderedIndexPopLast(oi);
    assertScore(item, 3.0);
    orderedIndexItemFree(item);
    ASSERT_EQ(orderedIndexLength(oi), 3UL);
    verifyOI();
}

TEST_F(OrderedIndexTest, GetFirstAndLast) {
    /* Empty index returns NULL */
    ASSERT_EQ(orderedIndexGetFirst(oi), nullptr);
    ASSERT_EQ(orderedIndexGetLast(oi), nullptr);

    insert(2.0, "bravo");
    insert(1.0, "alpha");
    insert(3.0, "charlie");

    assertElement(orderedIndexGetFirst(oi), "alpha");
    assertScore(orderedIndexGetFirst(oi), 1.0);
    assertElement(orderedIndexGetLast(oi), "charlie");
    assertScore(orderedIndexGetLast(oi), 3.0);


    /* Single element: first == last */
    orderedIndexDelete(oi, orderedIndexGetFirst(oi));
    orderedIndexDelete(oi, orderedIndexGetLast(oi));
    ASSERT_EQ(orderedIndexLength(oi), 1UL);
    assertElement(orderedIndexGetFirst(oi), "bravo");
    assertElement(orderedIndexGetLast(oi), "bravo");
}

TEST_F(OrderedIndexTest, UpdateScore) {
    OrderedIndexItem *node1 = insert(1.0, "key1");
    OrderedIndexItem *node2 = insert(2.0, "key2");
    insert(3.0, "key3");

    OrderedIndexItem *updated = orderedIndexUpdateScore(oi, node2, 4.0);
    ASSERT_NE(updated, nullptr);
    assertScore(updated, 4.0);
    verifyOI();
    assertElement(updated, "key2");

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(&iter, oi);
    assertNextScore(&iter, 1.0);
    assertNextScore(&iter, 3.0);
    pos = assertNextScore(&iter, 4.0);
    assertElement(pos, "key2");
    orderedIndexResetIterator(&iter);


    /* Update to same score (no-op) */
    updated = orderedIndexUpdateScore(oi, node1, 1.0);
    assertScore(updated, 1.0);
    verifyOI();
}

TEST_F(OrderedIndexTest, DeleteRangeByScore) {
    populateSequential(10);


    /* Delete range [3, 6] inclusive */
    unsigned long deleted = orderedIndexDeleteRangeByScore(oi, 3.0, 6.0, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 4UL);
    ASSERT_EQ(orderedIndexLength(oi), 6UL);
    verifyOI();

    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    for (int i = 0; i < 3; i++) {
        assertNextScore(&iter, (double)i);
    }
    for (int i = 7; i < 10; i++) {
        assertNextScore(&iter, (double)i);
    }
    orderedIndexResetIterator(&iter);


    /* Delete with exclusive bounds (2, 8) - should delete 7 */
    deleted = orderedIndexDeleteRangeByScore(oi, 2.0, 8.0, 1, 1, NULL, NULL);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(orderedIndexLength(oi), 5UL);
    verifyOI();
}

TEST_F(OrderedIndexTest, DeleteRangeByIndex) {
    populateSequential(10);


    /* Delete indices 2-4 (elements at scores 2,3,4) */
    unsigned long deleted = orderedIndexDeleteRangeByIndex(oi, 2, 4, NULL, NULL);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(orderedIndexLength(oi), 7UL);
    verifyOI();

    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    assertNextScore(&iter, 0.0);
    orderedIndexResetIterator(&iter);


    /* Verify index 2 is now score 5 (was index 5) */
    OrderedIndexItem *node = orderedIndexGetByIndex(oi, 2);
    assertScore(node, 5.0);
}

TEST_F(OrderedIndexTest, MixedOperationsIndexIntegrity) {
    OrderedIndexItem *nodes[100];

    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }

    for (int i = 2; i < 100; i += 3) {
        orderedIndexDelete(oi, nodes[i]);
        nodes[i] = NULL;
    }
    verifyOI();


    /* Update scores of surviving nodes (indices not ≡ 2 mod 3) */
    nodes[10] = orderedIndexUpdateScore(oi, nodes[10], 150.0);
    nodes[19] = orderedIndexUpdateScore(oi, nodes[19], 160.0);
    verifyOI();

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(&iter, oi);
    unsigned long expectedIdx = 0;
    while (((pos = orderedIndexNext(&iter)) != NULL)) {
        unsigned long actualIdx = orderedIndexGetIndex(oi, pos);
        ASSERT_EQ(actualIdx, expectedIdx);
        expectedIdx++;
    }
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, BackwardTraversalAfterDeletions) {
    OrderedIndexItem *nodes[20];

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }

    orderedIndexDelete(oi, nodes[5]);
    orderedIndexDelete(oi, nodes[10]);
    orderedIndexDelete(oi, nodes[15]);
    verifyOI();

    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    int expected_scores[] = {19, 18, 17, 16, 14, 13, 12, 11, 9, 8, 7, 6, 4, 3, 2, 1, 0};

    for (int i = 0; i < 17; i++) {
        assertPrevScore(&iter, (double)expected_scores[i]);
    }
    ASSERT_EQ(orderedIndexPrev(&iter), nullptr);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, LexicographicEdgeCases) {
    insert(1.0, "z");
    insert(1.0, "");
    insert(1.0, "a");

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(&iter, oi);
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, "");
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, "a");
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, "z");
    orderedIndexResetIterator(&iter);
    orderedIndexFree(oi);

    oi = orderedIndexCreate();
    char long_buf[1024];
    memset(long_buf, 'x', 1023);
    long_buf[1023] = '\0';
    insert(1.0, long_buf);
    insert(1.0, "short");

    orderedIndexInitIterator(&iter, oi);
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, "short");
    pos = assertNextScore(&iter, 1.0);
    assertElement(pos, long_buf);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, RangeBoundaryPrecision) {
    double base = 1.0;
    double epsilon = 1e-10;

    insert(base, "at_base");
    insert(base + epsilon, "at_base_plus_epsilon");
    insert(base + 2 * epsilon, "at_base_plus_2epsilon");

    unsigned long deleted = orderedIndexDeleteRangeByScore(oi, base, base + 2 * epsilon, 1, 1, NULL, NULL);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(orderedIndexLength(oi), 2UL);

    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    assertNextScore(&iter, base);
    assertNextScore(&iter, base + 2 * epsilon);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, SpecialDoubleValues) {
    insert(NEG_INF, "neg_inf");
    insert(POS_INF, "pos_inf");
    insert(0.0, "zero");
    insert(1.0, "one");

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;
    orderedIndexInitIterator(&iter, oi);
    assertNextScore(&iter, NEG_INF);
    assertNextScore(&iter, 0.0);
    assertNextScore(&iter, 1.0);
    assertNextScore(&iter, POS_INF);
    orderedIndexResetIterator(&iter);
    orderedIndexFree(oi);

    oi = orderedIndexCreate();

    /* Test +0.0 vs -0.0 */
    insert(0.0, "pos_zero");
    insert(-0.0, "neg_zero");

    /* Both should be in the list, ordered lexicographically since scores are equal */
    ASSERT_EQ(orderedIndexLength(oi), 2UL);
    orderedIndexInitIterator(&iter, oi);
    pos = assertNextScore(&iter, 0.0);
    assertElement(pos, "neg_zero");
    pos = assertNextScore(&iter, 0.0);
    assertElement(pos, "pos_zero");
    orderedIndexResetIterator(&iter);
    orderedIndexFree(oi);

    oi = orderedIndexCreate();

    /* Test denormalized double */
    double denorm = 1e-320;
    insert(denorm, "denorm");
    insert(1.0, "normal");

    ASSERT_EQ(orderedIndexLength(oi), 2UL);
    orderedIndexInitIterator(&iter, oi);
    pos = assertNextScore(&iter, denorm);
    ASSERT_TRUE(orderedIndexItemGetScore(pos) < 1.0);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, SignedZeroScoresShareOneKey) {
    /* IEEE -0.0 and +0.0 must map to the same tree key so that numeric
     * score-range semantics hold (as they did with C double comparison). */
    insert(-0.0, "neg");
    insert(0.0, "pos");

    ASSERT_EQ(orderedIndexCountScoreRange(oi, 0.0, 0.0, 0, 0), 2UL);
    ASSERT_EQ(orderedIndexCountScoreRange(oi, -0.0, -0.0, 0, 0), 2UL);
    ASSERT_EQ(orderedIndexCountScoreRange(oi, -0.0, 0.0, 0, 0), 2UL);

    /* Range iteration over [0, 0] must yield both members. */
    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToScoreRange(&iter, 0.0, 0.0, false, false, 0);
    ASSERT_NE(orderedIndexNext(&iter), nullptr);
    ASSERT_NE(orderedIndexNext(&iter), nullptr);

    /* Deleting by score range 0..0 must remove members stored with -0. */
    ASSERT_EQ(orderedIndexDeleteRangeByScore(oi, 0.0, 0.0, false, false, NULL, NULL), 2UL);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
}

TEST_F(OrderedIndexTest, ScoreDescentWithLongSharedScorePrefix) {
    /* Adjacent representable doubles differ only in their lowest mantissa
     * bits, so the packed anchors of an inner node share most of their
     * 8-byte score prefix. The score-only descent must stay within the
     * 8-byte key when the shared prefix reaches into its final bytes. */
    enum { N = 200 };
    double scores[N];
    double s = 1.0;
    for (int i = 0; i < N; i++) {
        scores[i] = s;
        insert(s, "e");
        s = nextafter(s, 2.0);
    }

    ASSERT_EQ(orderedIndexCountScoreRange(oi, scores[100], scores[100], 0, 0), 1UL);
    ASSERT_EQ(orderedIndexCountScoreRange(oi, scores[0], scores[99], 0, 0), 100UL);

    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToScoreRange(&iter, scores[50], scores[50], false, false, 0);
    OrderedIndexItem *item = orderedIndexNext(&iter);
    ASSERT_NE(item, nullptr);
    assertScore(item, scores[50]);
}

TEST_F(OrderedIndexTest, EmptyIndexOperations) {
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    ASSERT_EQ(orderedIndexPrev(&iter), nullptr);
    orderedIndexResetIterator(&iter);
    ASSERT_EQ(orderedIndexGetByIndex(oi, 0), nullptr);
}

TEST_F(OrderedIndexTest, DeleteEdgeCases) {
    /* Delete only element */
    OrderedIndexItem *node = insert(1.0, "only");
    orderedIndexDelete(oi, node);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
    verifyOI();
    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);

    OrderedIndexItem *nodes[3];
    for (int i = 0; i < 3; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        nodes[i] = insert((double)i, buf);
    }
    orderedIndexDelete(oi, nodes[0]);
    ASSERT_EQ(orderedIndexLength(oi), 2UL);
    verifyOI();
    orderedIndexInitIterator(&iter, oi);
    assertNextScore(&iter, 1.0);
    orderedIndexResetIterator(&iter);


    /* Delete last element */
    orderedIndexDelete(oi, nodes[2]);
    ASSERT_EQ(orderedIndexLength(oi), 1UL);
    verifyOI();
    orderedIndexInitIterator(&iter, oi);
    assertPrevScore(&iter, 1.0);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, IndexEdgeCases) {
    populateSequential(5);

    ASSERT_EQ(orderedIndexGetByIndex(oi, 5), nullptr);
    ASSERT_EQ(orderedIndexGetByIndex(oi, 99), nullptr);
    ASSERT_NE(orderedIndexGetByIndex(oi, 0), nullptr);
    ASSERT_NE(orderedIndexGetByIndex(oi, 4), nullptr);
}

TEST_F(OrderedIndexTest, DuplicateInsert) {
    OrderedIndexItem *node1 = insert(1.0, "duplicate");
    OrderedIndexItem *node2 = insert(1.0, "duplicate");

    /* Should have 2 nodes (duplicates allowed) */
    ASSERT_EQ(orderedIndexLength(oi), 2UL);
    ASSERT_NE(node1, node2);
}

TEST_F(OrderedIndexTest, DuplicateInsert_EachHasDistinctRank) {
    OrderedIndexItem *node1 = insert(1.0, "dup");
    OrderedIndexItem *node2 = insert(1.0, "dup");

    unsigned long r1 = orderedIndexGetIndex(oi, node1);
    unsigned long r2 = orderedIndexGetIndex(oi, node2);
    ASSERT_NE(r1, r2);
    ASSERT_EQ(r1 + r2, 1UL); /* ranks 0 and 1 */
}

TEST_F(OrderedIndexTest, DuplicateInsert_DeleteOne) {
    insert(1.0, "dup");
    insert(1.0, "dup");

    /* Delete the first item encountered by the index (the one at rank 0).
     * With exact duplicates, the index may delete either copy -- the contract
     * only guarantees that exactly one is removed. */
    OrderedIndexItem *first = orderedIndexGetFirst(oi);
    orderedIndexDelete(oi, first);
    ASSERT_EQ(orderedIndexLength(oi), 1UL);
    verifyOI();

    /* The remaining item is accessible */
    OrderedIndexItem *remaining = orderedIndexGetFirst(oi);
    ASSERT_NE(remaining, nullptr);
    assertScore(remaining, 1.0);
}

TEST_F(OrderedIndexTest, DuplicateInsert_ManySpanningLeafSplits) {
    /* Insert enough identical (score, element) pairs to overflow a single
     * leaf and force splits, so equal values span sibling leaves whose
     * anchors are identical. Every instance must remain individually
     * addressable by rank and deletable by pointer, regardless of which
     * sibling leaf it landed in. */
    enum { DUP_COUNT = 200 }; /* several leaves worth of duplicates */
    OrderedIndexItem *items[DUP_COUNT];
    for (int i = 0; i < DUP_COUNT; i++) items[i] = insert(1.0, "dup");
    ASSERT_EQ(orderedIndexLength(oi), (unsigned long)DUP_COUNT);

    /* Every instance is findable and each occupies a distinct rank. */
    bool seen[DUP_COUNT] = {false};
    for (int i = 0; i < DUP_COUNT; i++) {
        unsigned long rank = orderedIndexGetIndex(oi, items[i]);
        ASSERT_LT(rank, (unsigned long)DUP_COUNT) << "instance " << i << " not found by rank lookup";
        ASSERT_FALSE(seen[rank]) << "instances " << i << " share rank " << rank;
        seen[rank] = true;
    }

    /* Delete every instance by pointer, newest first (later instances are
     * the ones routed past by a leftmost-only descent). */
    for (int i = DUP_COUNT - 1; i >= 0; i--) {
        orderedIndexDelete(oi, items[i]);
        ASSERT_EQ(orderedIndexLength(oi), (unsigned long)i) << "delete of instance " << i << " did not remove it";
    }
    verifyOI();
}

TEST_F(OrderedIndexTest, DuplicateInsert_PopRemovesOne) {
    insert(1.0, "dup");
    insert(1.0, "dup");
    insert(2.0, "other");

    OrderedIndexItem *popped = orderedIndexPopFirst(oi);
    assertElement(popped, "dup");
    orderedIndexItemFree(popped);

    /* One "dup" remains plus "other" */
    ASSERT_EQ(orderedIndexLength(oi), 2UL);
    assertElement(orderedIndexGetFirst(oi), "dup");
    verifyOI();
}

TEST_F(OrderedIndexTest, UpdateScoreEdgeCases) {
    populateSequential(5);

    OrderedIndexItem *first = orderedIndexGetByIndex(oi, 0);
    OrderedIndexItem *updated = orderedIndexUpdateScore(oi, first, 10.0);
    assertScore(updated, 10.0);
    verifyOI();
    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    ASSERT_EQ(orderedIndexPrev(&iter), updated);
    orderedIndexResetIterator(&iter);


    /* Move last element to first position */
    OrderedIndexItem *last = orderedIndexGetByIndex(oi, orderedIndexLength(oi) - 1);
    updated = orderedIndexUpdateScore(oi, last, -1.0);
    assertScore(updated, -1.0);
    verifyOI();
    orderedIndexInitIterator(&iter, oi);
    ASSERT_EQ(orderedIndexNext(&iter), updated);
    orderedIndexResetIterator(&iter);


    /* Move middle element backward past multiple */
    OrderedIndexItem *middle = orderedIndexGetByIndex(oi, 2);
    updated = orderedIndexUpdateScore(oi, middle, 0.5);
    assertScore(updated, 0.5);
    verifyOI();
    ASSERT_EQ(orderedIndexGetIndex(oi, updated), 1UL);


    /* Update score without changing position (stays between neighbors) */
    OrderedIndexItem *n = orderedIndexGetByIndex(oi, 3);
    unsigned long idx_before = orderedIndexGetIndex(oi, n);
    updated = orderedIndexUpdateScore(oi, n, orderedIndexItemGetScore(n) + 0.1);
    ASSERT_EQ(orderedIndexGetIndex(oi, updated), idx_before);
    verifyOI();
}

TEST_F(OrderedIndexTest, RangeDeleteEdgeCases) {
    populateSequential(10);


    /* Delete empty range (min > max) */
    unsigned long deleted = orderedIndexDeleteRangeByScore(oi, 5.0, 4.0, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(orderedIndexLength(oi), 10UL);


    /* Delete range with no matches */
    deleted = orderedIndexDeleteRangeByScore(oi, 10.5, 11.5, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(orderedIndexLength(oi), 10UL);


    /* Delete first elements by index */
    deleted = orderedIndexDeleteRangeByIndex(oi, 0, 1, NULL, NULL);
    ASSERT_EQ(deleted, 2UL);
    verifyOI();
    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    assertNextScore(&iter, 2.0);
    orderedIndexResetIterator(&iter);

    unsigned long len = orderedIndexLength(oi);

    /* Delete last elements by index */
    deleted = orderedIndexDeleteRangeByIndex(oi, len - 2, len - 1, NULL, NULL);
    ASSERT_EQ(deleted, 2UL);
    verifyOI();
    orderedIndexInitIterator(&iter, oi);
    assertPrevScore(&iter, 7.0);
    orderedIndexResetIterator(&iter);


    /* Delete entire remaining index by score */
    deleted = orderedIndexDeleteRangeByScore(oi, -100.0, 100.0, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 6UL);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
    verifyOI();
}

TEST_F(OrderedIndexTest, TraversalEdgeCases) {
    insert(1.0, "single");

    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi);
    assertNextScore(&iter, 1.0);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);
    assertPrevScore(&iter, 1.0);
    ASSERT_EQ(orderedIndexPrev(&iter), nullptr);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, SeekToIndex) {
    /* Elements at ranks 0..4 with scores 1.0..5.0 */
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert((double)i, buf);
    }

    /* See ordered_index.h for cursor-between-items model diagram.
     * SeekToIndex(N) places cursor at position N: next() returns rank N,
     * prev() returns rank N-1. */

    OrderedIndexIterator iter;

    /* Seek to index 0: next() returns rank 0 (first element) */
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToIndex(&iter, 0);
    assertNextScore(&iter, 1.0);
    orderedIndexResetIterator(&iter);

    /* Seek to index 0: prev() returns NULL (nothing before first) */
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToIndex(&iter, 0);
    ASSERT_EQ(orderedIndexPrev(&iter), nullptr);
    orderedIndexResetIterator(&iter);

    /* Seek to index 1: next() returns rank 1 */
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToIndex(&iter, 1);
    assertNextScore(&iter, 2.0);
    orderedIndexResetIterator(&iter);

    /* Seek to index 1: prev() returns rank 0 */
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToIndex(&iter, 1);
    assertPrevScore(&iter, 1.0);
    orderedIndexResetIterator(&iter);

    /* Seek to index 2 (middle): next() returns rank 2 */
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToIndex(&iter, 2);
    assertNextScore(&iter, 3.0);
    orderedIndexResetIterator(&iter);

    /* Seek to index 2 (middle): prev() returns rank 1 */
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToIndex(&iter, 2);
    assertPrevScore(&iter, 2.0);
    orderedIndexResetIterator(&iter);

    /* Seek to index 5 (past end): next() returns NULL */
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToIndex(&iter, 5);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);

    /* Seek to index 5 (past end): prev() returns last element */
    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToIndex(&iter, 5);
    assertPrevScore(&iter, 5.0);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, ReverseIteration) {
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert((double)i, buf);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    /* Unseeked iterator: cursor starts at position 5 (past end) for prev(),
     * or position 0 (before start) for next(). */
    orderedIndexInitIterator(&iter, oi);
    int count = 0;
    double expected = 5.0;
    while (((pos = orderedIndexPrev(&iter)) != NULL)) {
        assertScore(pos, expected);
        expected -= 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);
    assertPrevScore(&iter, 5.0);
    assertNextScore(&iter, 5.0);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);
    assertNextScore(&iter, 1.0);
    assertPrevScore(&iter, 1.0);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, SeekToScoreRange) {
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);

        /* Insert elements with scores 0,2,4,6,8 */
        insert((double)(i * 2), buf);
    }

    /* SeekToScoreRange uses the same cursor-between-items model:
     * offset >= 0 places cursor before the offset'th item in range (next returns it).
     * offset < 0 places cursor after the (-offset-1)'th item from end (prev returns it). */

    OrderedIndexIterator iter;

    orderedIndexInitIterator(&iter, oi);

    /* Seek to first in range [2, 6] with offset 0 */
    orderedIndexSeekToScoreRange(&iter, 2.0, 6.0, 0, 0, 0);
    assertNextScore(&iter, 2.0);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Seek to second in range [2, 6] with offset 1 */
    orderedIndexSeekToScoreRange(&iter, 2.0, 6.0, 0, 0, 1);
    assertNextScore(&iter, 4.0);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Seek to last in range [2, 6] with offset -1, positioned for prev() */
    orderedIndexSeekToScoreRange(&iter, 2.0, 6.0, 0, 0, -1);
    assertPrevScore(&iter, 6.0);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Seek with exclusive bounds (2, 6) - should start at 4 */
    orderedIndexSeekToScoreRange(&iter, 2.0, 6.0, 1, 1, 0);
    assertNextScore(&iter, 4.0);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Seek to empty range above all elements */
    orderedIndexSeekToScoreRange(&iter, 10.0, 20.0, 0, 0, 0);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Seek to empty range below all elements */
    orderedIndexSeekToScoreRange(&iter, -20.0, -10.0, 0, 0, 0);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Out of range positive offset */
    orderedIndexSeekToScoreRange(&iter, 2.0, 6.0, 0, 0, 10);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Negative offset beyond range */
    orderedIndexSeekToScoreRange(&iter, 2.0, 6.0, 0, 0, -10);
    ASSERT_EQ(orderedIndexPrev(&iter), nullptr);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Second from last with offset -2, positioned for prev() */
    orderedIndexSeekToScoreRange(&iter, 2.0, 6.0, 0, 0, -2);
    assertPrevScore(&iter, 4.0);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Empty range where min > max */
    orderedIndexSeekToScoreRange(&iter, 6.0, 2.0, 0, 0, 0);
    ASSERT_EQ(orderedIndexNext(&iter), nullptr);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, SeekToScoreRangeIteration) {
    populateSequential(10);

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    orderedIndexInitIterator(&iter, oi);

    /* Seek to range [3, 7] and iterate forward */
    orderedIndexSeekToScoreRange(&iter, 3.0, 7.0, 0, 0, 0);
    int count = 0;
    double expected = 3.0;
    while (((pos = orderedIndexNext(&iter)) != NULL) && orderedIndexItemGetScore(pos) <= 7.0) {
        assertScore(pos, expected);
        expected += 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Seek to last in range and iterate backward */
    orderedIndexSeekToScoreRange(&iter, 3.0, 7.0, 0, 0, -1);
    count = 0;
    expected = 7.0;
    while (((pos = orderedIndexPrev(&iter)) != NULL) && orderedIndexItemGetScore(pos) >= 3.0) {
        assertScore(pos, expected);
        expected -= 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    orderedIndexResetIterator(&iter);

    orderedIndexInitIterator(&iter, oi);

    /* Seek with offset and continue iteration */
    orderedIndexSeekToScoreRange(&iter, 2.0, 8.0, 0, 0, 2);
    assertNextScore(&iter, 4.0);
    assertNextScore(&iter, 5.0);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, SeekInfReverseIteration) {
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert((double)i, buf);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToScoreRange(&iter, NEG_INF, POS_INF, 0, 0, -1);
    int count = 0;
    double expected = 5.0;
    while (((pos = orderedIndexPrev(&iter)) != NULL)) {
        assertScore(pos, expected);
        expected -= 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, SeekInfForwardIteration) {
    for (int i = 1; i <= 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        insert((double)i, buf);
    }

    OrderedIndexIterator iter;
    OrderedIndexItem *pos;

    orderedIndexInitIterator(&iter, oi);
    orderedIndexSeekToScoreRange(&iter, NEG_INF, POS_INF, 0, 0, 0);
    int count = 0;
    double expected = 1.0;
    while (((pos = orderedIndexNext(&iter)) != NULL)) {
        assertScore(pos, expected);
        expected += 1.0;
        count++;
    }
    ASSERT_EQ(count, 5);
    orderedIndexResetIterator(&iter);
}

TEST_F(OrderedIndexTest, SeekToLexRange) {
    for (int i = 0; i < FRUITS_COUNT; i++) insert(1.0, FRUITS[i]);

    OrderedIndexIterator it;
    OrderedIndexItem *pos;

    orderedIndexInitIterator(&it, oi);

    /* Seek to first in lex range [banana, date] with offset 0 */
    seekToLexRange(&it, "banana", "date", 0, 0, 0);
    ASSERT_NE((pos = orderedIndexNext(&it)), nullptr);
    assertElement(pos, "banana");
    orderedIndexResetIterator(&it);

    orderedIndexInitIterator(&it, oi);

    /* Seek to second in lex range with offset 1 */
    seekToLexRange(&it, "banana", "date", 0, 0, 1);
    ASSERT_NE((pos = orderedIndexNext(&it)), nullptr);
    assertElement(pos, "cherry");
    orderedIndexResetIterator(&it);

    orderedIndexInitIterator(&it, oi);

    /* Seek to last in lex range with offset -1, positioned for prev() */
    seekToLexRange(&it, "banana", "date", 0, 0, -1);
    ASSERT_NE((pos = orderedIndexPrev(&it)), nullptr);
    assertElement(pos, "date");
    orderedIndexResetIterator(&it);

    orderedIndexInitIterator(&it, oi);

    /* Seek with exclusive bounds (banana, date) - should start at cherry */
    seekToLexRange(&it, "banana", "date", 1, 1, 0);
    ASSERT_NE((pos = orderedIndexNext(&it)), nullptr);
    assertElement(pos, "cherry");
    orderedIndexResetIterator(&it);

    orderedIndexInitIterator(&it, oi);

    /* Seek to empty lex range */
    seekToLexRange(&it, "zzz", "zzzz", 0, 0, 0);
    ASSERT_EQ(orderedIndexNext(&it), nullptr);
    orderedIndexResetIterator(&it);

    orderedIndexInitIterator(&it, oi);

    /* Out of range positive offset */
    seekToLexRange(&it, "banana", "date", 0, 0, 10);
    ASSERT_EQ(orderedIndexNext(&it), nullptr);
    orderedIndexResetIterator(&it);
}

TEST_F(OrderedIndexTest, DeleteRangeByLexInclusive) {
    for (int i = 0; i < FRUITS_COUNT; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("banana", "date", 0, 0, NULL, NULL), 3UL);
    ASSERT_EQ(orderedIndexLength(oi), 2UL);
    verifyOI();
    ASSERT_ALL_ELEMENTS("apple", "elderberry");
}

TEST_F(OrderedIndexTest, DeleteRangeByLexExclusive) {
    for (int i = 0; i < FRUITS_COUNT; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("banana", "date", 1, 1, NULL, NULL), 1UL);
    ASSERT_EQ(orderedIndexLength(oi), 4UL);
    ASSERT_ALL_ELEMENTS("apple", "banana", "date", "elderberry");
}

TEST_F(OrderedIndexTest, DeleteRangeByLex_EmptyRange) {
    for (int i = 0; i < 3; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("zzz", "aaa", 0, 0, NULL, NULL), 0UL);
    ASSERT_EQ(orderedIndexLength(oi), 3UL);
}

TEST_F(OrderedIndexTest, DeleteRangeByLex_All) {
    for (int i = 0; i < 3; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("a", "z", 0, 0, NULL, NULL), 3UL);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
}

TEST_F(OrderedIndexTest, DeleteRangeByLex_SingleElement) {
    for (int i = 0; i < 3; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(deleteLexRange("banana", "banana", 0, 0, NULL, NULL), 1UL);
    ASSERT_EQ(orderedIndexLength(oi), 2UL);
    ASSERT_ALL_ELEMENTS("apple", "cherry");
}

TEST_F(OrderedIndexTest, DeleteRangeByLexPreservesOutside) {
    for (int i = 0; i < NATO_COUNT; i++) insert(1.0, NATO[i]);

    ASSERT_EQ(deleteLexRange("charlie", "delta", 0, 0, NULL, NULL), 2UL);
    ASSERT_EQ(orderedIndexLength(oi), 4UL);
    ASSERT_ALL_ELEMENTS("alpha", "bravo", "echo", "foxtrot");

    OrderedIndexIterator it;
    orderedIndexInitIterator(&it, oi);
    OrderedIndexItem *pos;
    while ((pos = orderedIndexNext(&it)) != NULL) {
        assertScore(pos, 1.0);
    }
    orderedIndexResetIterator(&it);

    for (unsigned long r = 0; r < 4; r++) {
        OrderedIndexItem *node = orderedIndexGetByIndex(oi, r);
        ASSERT_NE(node, nullptr);
        ASSERT_EQ(orderedIndexGetIndex(oi, node), r);
    }
}

TEST_F(OrderedIndexTest, LexRangeSentinels) {
    insert(0.0, "alpha");
    insert(0.0, "bravo");
    insert(0.0, "charlie");
    insert(0.0, "delta");
    insert(0.0, "echo");

    sds charlie = sdsnew("charlie");

    ASSERT_EQ(countLexRange("minstring", "maxstring", 0, 0), 0UL); /* literal strings, not sentinels */
    ASSERT_EQ(orderedIndexCountLexRange(oi, shared.minstring, shared.maxstring, 0, 0), 5UL);
    ASSERT_EQ(orderedIndexCountLexRange(oi, shared.minstring, charlie, 0, 0), 3UL);
    ASSERT_EQ(orderedIndexCountLexRange(oi, charlie, shared.maxstring, 0, 0), 3UL);


    /* Inverted range (max < min sentinel) should return 0 */
    ASSERT_EQ(orderedIndexCountLexRange(oi, shared.maxstring, shared.minstring, 0, 0), 0UL);
    ASSERT_EQ(orderedIndexCountLexRange(oi, charlie, shared.minstring, 0, 0), 0UL);

    OrderedIndexIterator it;
    orderedIndexInitIterator(&it, oi);
    /* Seek with sentinels - iterate all */
    orderedIndexSeekToLexRange(&it, shared.minstring, shared.maxstring, 0, 0, 0);
    assertNextScore(&it, 0.0); /* alpha */
    assertNextScore(&it, 0.0); /* bravo */
    assertNextScore(&it, 0.0); /* charlie */
    assertNextScore(&it, 0.0); /* delta */
    assertNextScore(&it, 0.0); /* echo */
    ASSERT_EQ(orderedIndexNext(&it), nullptr);
    orderedIndexResetIterator(&it);


    /* Delete with sentinels - delete all */
    ASSERT_EQ(orderedIndexDeleteRangeByLex(oi, shared.minstring, shared.maxstring, 0, 0, NULL, NULL), 5UL);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);

    sdsfree(charlie);
}

/* ========== Batch-insert workflow tests ========== */

/* orderedIndexItemCreate + orderedIndexItemSetScore + orderedIndexInsertItem
 * back the store-aggregation path (ZUNIONSTORE/ZDIFFSTORE/ZINTERSTORE): detached
 * items are created, their scores are adjusted O(1) while aggregating sources,
 * then all are bulk-inserted at the end. ItemSetScore's no-reposition branch is
 * reachable only through this path, so it needs direct coverage. */

TEST_F(OrderedIndexTest, BatchInsertCreateSetScoreInsert) {
    const char *names[] = {"cherry", "apple", "banana", "date"};
    double finalscore[] = {3.0, 1.0, 2.0, 4.0};
    const int n = 4;
    OrderedIndexItem *items[4];

    for (int i = 0; i < n; i++) {
        /* Create detached (not in any index) at a placeholder score. */
        items[i] = orderedIndexItemCreate(-999.0, names[i], strlen(names[i]));
        ASSERT_NE(items[i], nullptr);
        /* Several O(1) score updates, as aggregation across sources would do. */
        orderedIndexItemSetScore(items[i], finalscore[i] + 10.0);
        orderedIndexItemSetScore(items[i], finalscore[i]);
        assertScore(items[i], finalscore[i]);
    }
    /* Nothing is inserted yet. */
    ASSERT_EQ(orderedIndexLength(oi), 0UL);

    /* Bulk-insert; the index takes ownership and returns the same pointer. */
    for (int i = 0; i < n; i++) {
        ASSERT_EQ(orderedIndexInsertItem(oi, items[i]), items[i]);
    }
    ASSERT_EQ(orderedIndexLength(oi), (unsigned long)n);
    verifyOI();

    /* Ordered by the final scores. */
    ASSERT_ALL_ELEMENTS("apple", "banana", "cherry", "date");
    assertScore(orderedIndexGetByIndex(oi, 0), 1.0);
    assertScore(orderedIndexGetByIndex(oi, 1), 2.0);
    assertScore(orderedIndexGetByIndex(oi, 2), 3.0);
    assertScore(orderedIndexGetByIndex(oi, 3), 4.0);
}

TEST_F(OrderedIndexTest, BatchInsertInterleavesWithExistingItems) {
    /* Items already present via the normal insert path. */
    insert(2.0, "banana");
    insert(4.0, "date");

    /* Batch-created items whose scores interleave with the existing ones. */
    OrderedIndexItem *a = orderedIndexItemCreate(0.0, "apple", 5);
    orderedIndexItemSetScore(a, 1.0);
    OrderedIndexItem *c = orderedIndexItemCreate(0.0, "cherry", 6);
    orderedIndexItemSetScore(c, 3.0);
    orderedIndexInsertItem(oi, a);
    orderedIndexInsertItem(oi, c);

    ASSERT_EQ(orderedIndexLength(oi), 4UL);
    verifyOI();
    ASSERT_ALL_ELEMENTS("apple", "banana", "cherry", "date");
}

TEST_F(OrderedIndexTest, BatchInsertManyBuildsValidTree) {
    /* Enough batch-inserted items to force a multi-level tree, so InsertItem
     * exercises the full descent/split path rather than a single leaf. */
    enum { N = 2000 };
    for (int i = 0; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%05d", i);
        OrderedIndexItem *it = orderedIndexItemCreate(0.0, buf, strlen(buf));
        orderedIndexItemSetScore(it, (double)i);
        orderedIndexInsertItem(oi, it);
    }
    ASSERT_EQ(orderedIndexLength(oi), (unsigned long)N);
    verifyOI();

    /* Rank order follows the scores set before insertion. */
    for (int i = 0; i < N; i += 137) {
        OrderedIndexItem *it = orderedIndexGetByIndex(oi, i);
        char buf[32];
        snprintf(buf, sizeof(buf), "key%05d", i);
        assertScore(it, (double)i);
        assertElement(it, buf);
    }
    ASSERT_EQ(orderedIndexCountScoreRange(oi, NEG_INF, POS_INF, 0, 0), (unsigned long)N);
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

static RandomIndexEntry *test_build_random_index(OrderedIndex *oi, uint32_t *state, int count) {
    RandomIndexEntry *entries = (RandomIndexEntry *)zmalloc(sizeof(RandomIndexEntry) * count);
    for (int i = 0; i < count; i++) {
        double score = test_random_score(state);
        sds elem = test_random_element(state, 16);
        elem = sdscatfmt(elem, "%i", i); /* Append index to ensure uniqueness */
        OrderedIndexItem *node = orderedIndexInsert(oi, score, elem, sdslen(elem));
        entries[i] = {node, score, elem};
    }
    return entries;
}

static void freeRandomEntries(RandomIndexEntry *entries, int count) {
    for (int i = 0; i < count; i++) sdsfree(entries[i].element);
    zfree(entries);
}

TEST_F(OrderedIndexTest, RandomizedInsertAndTraversal) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        {
            RandomIndexEntry *_e = test_build_random_index(oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        ASSERT_EQ(orderedIndexLength(oi), (unsigned long)n);
        verifyOI();

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        orderedIndexInitIterator(&iter, oi);
        int count = 0;
        double prevScore = NEG_INF;
        while (((pos = orderedIndexNext(&iter)) != NULL)) {
            double s = orderedIndexItemGetScore(pos);
            ASSERT_GE(s, prevScore);
            prevScore = s;
            count++;
        }
        ASSERT_EQ(count, n);
        orderedIndexResetIterator(&iter);
        orderedIndexFree(oi);
        oi = orderedIndexCreate();
    }
}

TEST_F(OrderedIndexTest, RandomizedBackwardTraversal) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        {
            RandomIndexEntry *_e = test_build_random_index(oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        orderedIndexInitIterator(&iter, oi);
        int count = 0;
        double prevScore = POS_INF;
        while (((pos = orderedIndexPrev(&iter)) != NULL)) {
            double s = orderedIndexItemGetScore(pos);
            ASSERT_LE(s, prevScore);
            prevScore = s;
            count++;
        }
        ASSERT_EQ(count, n);
        orderedIndexResetIterator(&iter);
        orderedIndexFree(oi);
        oi = orderedIndexCreate();
    }
}

TEST_F(OrderedIndexTest, RandomizedScoreRetrieval) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        RandomIndexEntry *entries = test_build_random_index(oi, &rng, n);

        for (int i = 0; i < n; i++) {
            assertScore(entries[i].node, entries[i].score);
        }
        freeRandomEntries(entries, n);
        orderedIndexFree(oi);
        oi = orderedIndexCreate();
    }
}

TEST_F(OrderedIndexTest, RandomizedIndexConsistency) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        {
            RandomIndexEntry *_e = test_build_random_index(oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        orderedIndexInitIterator(&iter, oi);
        unsigned long expectedIdx = 0;
        while (((pos = orderedIndexNext(&iter)) != NULL)) {
            unsigned long idx = orderedIndexGetIndex(oi, pos);
            ASSERT_EQ(idx, expectedIdx);
            OrderedIndexItem *byIdx = orderedIndexGetByIndex(oi, expectedIdx);
            ASSERT_EQ(byIdx, pos);
            expectedIdx++;
        }
        ASSERT_EQ(expectedIdx, (unsigned long)n);
        orderedIndexResetIterator(&iter);
        orderedIndexFree(oi);
        oi = orderedIndexCreate();
    }
}

TEST_F(OrderedIndexTest, RandomizedDelete) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 2, 30);

        RandomIndexEntry *entries = test_build_random_index(oi, &rng, n);

        int delIdx = test_rand_range(&rng, 0, n - 1);
        orderedIndexDelete(oi, entries[delIdx].node);

        ASSERT_EQ(orderedIndexLength(oi), (unsigned long)(n - 1));
        verifyOI();

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        orderedIndexInitIterator(&iter, oi);
        int count = 0;
        double prevScore = NEG_INF;
        while (((pos = orderedIndexNext(&iter)) != NULL)) {
            ASSERT_GE(orderedIndexItemGetScore(pos), prevScore);
            prevScore = orderedIndexItemGetScore(pos);
            count++;
        }
        ASSERT_EQ(count, n - 1);
        orderedIndexResetIterator(&iter);
        freeRandomEntries(entries, n);
        orderedIndexFree(oi);
        oi = orderedIndexCreate();
    }
}

TEST_F(OrderedIndexTest, RandomizedUpdateScore) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 2, 30);

        RandomIndexEntry *entries = test_build_random_index(oi, &rng, n);

        int updIdx = test_rand_range(&rng, 0, n - 1);
        double newScore = test_random_score(&rng);

        OrderedIndexItem *updated = orderedIndexUpdateScore(oi, entries[updIdx].node, newScore);
        ASSERT_NE(updated, nullptr);
        assertScore(updated, newScore);
        ASSERT_EQ(orderedIndexLength(oi), (unsigned long)n);
        verifyOI();

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        orderedIndexInitIterator(&iter, oi);
        double prevScore = NEG_INF;
        while (((pos = orderedIndexNext(&iter)) != NULL)) {
            ASSERT_GE(orderedIndexItemGetScore(pos), prevScore);
            prevScore = orderedIndexItemGetScore(pos);
        }
        orderedIndexResetIterator(&iter);
        orderedIndexFree(oi);
        freeRandomEntries(entries, n);
        oi = orderedIndexCreate();
    }
}

TEST_F(OrderedIndexTest, RandomizedPop) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 10; trial++) {
        int n = test_rand_range(&rng, 3, 30);

        {
            RandomIndexEntry *_e = test_build_random_index(oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        orderedIndexInitIterator(&iter, oi);
        ASSERT_NE((pos = orderedIndexNext(&iter)), nullptr);
        double minScore = orderedIndexItemGetScore(pos);
        orderedIndexResetIterator(&iter);

        orderedIndexInitIterator(&iter, oi);
        ASSERT_NE((pos = orderedIndexPrev(&iter)), nullptr);
        double maxScore = orderedIndexItemGetScore(pos);
        orderedIndexResetIterator(&iter);

        OrderedIndexItem *first = orderedIndexPopFirst(oi);
        ASSERT_NE(first, nullptr);
        assertScore(first, minScore);
        ASSERT_EQ(orderedIndexLength(oi), (unsigned long)(n - 1));
        orderedIndexItemFree(first);
        verifyOI();

        OrderedIndexItem *last = orderedIndexPopLast(oi);
        ASSERT_NE(last, nullptr);
        assertScore(last, maxScore);
        ASSERT_EQ(orderedIndexLength(oi), (unsigned long)(n - 2));
        orderedIndexItemFree(last);
        verifyOI();

        orderedIndexInitIterator(&iter, oi);
        double prevScore = NEG_INF;
        while (((pos = orderedIndexNext(&iter)) != NULL)) {
            ASSERT_GE(orderedIndexItemGetScore(pos), prevScore);
            prevScore = orderedIndexItemGetScore(pos);
        }
        orderedIndexResetIterator(&iter);
        orderedIndexFree(oi);
        oi = orderedIndexCreate();
    }
}

TEST_F(OrderedIndexTest, RandomizedDeleteRangeByScore) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 5, 40);

        RandomIndexEntry *entries = test_build_random_index(oi, &rng, n);

        double s1 = test_random_score(&rng), s2 = test_random_score(&rng);
        double lo = TEST_MIN(s1, s2), hi = TEST_MAX(s1, s2);

        int expectedDeleted = 0;
        for (int i = 0; i < n; i++) {
            if (entries[i].score >= lo && entries[i].score <= hi) expectedDeleted++;
        }

        unsigned long deleted = orderedIndexDeleteRangeByScore(oi, lo, hi, 0, 0, NULL, NULL);
        ASSERT_EQ(deleted, (unsigned long)expectedDeleted);
        ASSERT_EQ(orderedIndexLength(oi), (unsigned long)(n - expectedDeleted));
        verifyOI();

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        orderedIndexInitIterator(&iter, oi);
        double prevScore = NEG_INF;
        while (((pos = orderedIndexNext(&iter)) != NULL)) {
            double s = orderedIndexItemGetScore(pos);
            ASSERT_TRUE(s < lo || s > hi);
            ASSERT_GE(s, prevScore);
            prevScore = s;
        }
        orderedIndexResetIterator(&iter);
        orderedIndexFree(oi);
        freeRandomEntries(entries, n);
        oi = orderedIndexCreate();
    }
}

TEST_F(OrderedIndexTest, RandomizedDeleteRangeByIndex) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 5, 40);

        {
            RandomIndexEntry *_e = test_build_random_index(oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        int r1 = test_rand_range(&rng, 0, n - 1), r2 = test_rand_range(&rng, 0, n - 1);
        unsigned long start = (unsigned long)TEST_MIN(r1, r2);
        unsigned long end = (unsigned long)TEST_MAX(r1, r2);
        unsigned long expectedDeleted = end - start + 1;

        unsigned long deleted = orderedIndexDeleteRangeByIndex(oi, start, end, NULL, NULL);
        ASSERT_EQ(deleted, expectedDeleted);
        ASSERT_EQ(orderedIndexLength(oi), (unsigned long)(n)-expectedDeleted);
        verifyOI();

        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        orderedIndexInitIterator(&iter, oi);
        int remaining = 0;
        double prevScore = NEG_INF;
        while (((pos = orderedIndexNext(&iter)) != NULL)) {
            ASSERT_GE(orderedIndexItemGetScore(pos), prevScore);
            prevScore = orderedIndexItemGetScore(pos);
            remaining++;
        }
        ASSERT_EQ(remaining, n - (int)expectedDeleted);
        orderedIndexResetIterator(&iter);
        orderedIndexFree(oi);
        oi = orderedIndexCreate();
    }
}

TEST_F(OrderedIndexTest, RandomizedForwardBackwardMirror) {
    uint32_t rng = test_fuzz_seed();
    for (int trial = 0; trial < 20; trial++) {
        int n = test_rand_range(&rng, 1, 50);

        {
            RandomIndexEntry *_e = test_build_random_index(oi, &rng, n);
            freeRandomEntries(_e, n);
        }

        double *forwardScores = (double *)zmalloc(sizeof(double) * n);
        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        int fi = 0;
        orderedIndexInitIterator(&iter, oi);
        while (((pos = orderedIndexNext(&iter)) != NULL)) {
            forwardScores[fi++] = orderedIndexItemGetScore(pos);
        }
        orderedIndexResetIterator(&iter);

        double *backwardScores = (double *)zmalloc(sizeof(double) * n);
        int bi = 0;
        orderedIndexInitIterator(&iter, oi);
        while (((pos = orderedIndexPrev(&iter)) != NULL)) {
            backwardScores[bi++] = orderedIndexItemGetScore(pos);
        }
        orderedIndexResetIterator(&iter);

        ASSERT_EQ(fi, bi);
        reverseDoubleArray(backwardScores, bi);
        for (int i = 0; i < fi; i++) {
            ASSERT_DOUBLE_EQ(forwardScores[i], backwardScores[i]);
        }
        zfree(forwardScores);
        zfree(backwardScores);
        orderedIndexFree(oi);
        oi = orderedIndexCreate();
    }
}

/* ========== Count range tests ========== */

TEST_F(OrderedIndexTest, CountScoreRange) {
    populateSequential(10);

    /* Full range */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, NEG_INF, POS_INF, 0, 0), 10UL);

    /* Inclusive [3, 6] */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, 3.0, 6.0, 0, 0), 4UL);

    /* Exclusive (3, 6) */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, 3.0, 6.0, 1, 1), 2UL);

    /* Single element [5, 5] */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, 5.0, 5.0, 0, 0), 1UL);

    /* Empty exclusive (5, 5) */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, 5.0, 5.0, 1, 0), 0UL);

    /* No match above */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, 10.0, 20.0, 0, 0), 0UL);

    /* No match below */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, -20.0, -10.0, 0, 0), 0UL);

    /* Min > max */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, 6.0, 3.0, 0, 0), 0UL);

    /* First element only [0, 0] */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, 0.0, 0.0, 0, 0), 1UL);

    /* Last element only [9, 9] */
    ASSERT_EQ(orderedIndexCountScoreRange(oi, 9.0, 9.0, 0, 0), 1UL);
}

TEST_F(OrderedIndexTest, CountScoreRangeEmpty) {
    ASSERT_EQ(orderedIndexCountScoreRange(oi, NEG_INF, POS_INF, 0, 0), 0UL);
}

TEST_F(OrderedIndexTest, CountLexRange) {
    for (int i = 0; i < FRUITS_COUNT; i++) insert(1.0, FRUITS[i]);

    ASSERT_EQ(countLexRange("banana", "date", 0, 0), 3UL);   /* Inclusive [banana, date] */
    ASSERT_EQ(countLexRange("banana", "date", 1, 1), 1UL);   /* Exclusive (banana, date) */
    ASSERT_EQ(countLexRange("cherry", "cherry", 0, 0), 1UL); /* Single element */
    ASSERT_EQ(countLexRange("fig", "grape", 0, 0), 0UL);     /* No match */
    ASSERT_EQ(countLexRange("a", "z", 0, 0), 5UL);           /* All elements */
}

TEST_F(OrderedIndexTest, CountLexRangeEmpty) {
    ASSERT_EQ(countLexRange("a", "z", 0, 0), 0UL);
}

TEST_F(OrderedIndexTest, LexRangeUnboundedIncludesHighBytes) {
    /* Members whose bytes sort above any single-byte suffix (e.g. leading
     * 0xFF with continuations) must still fall inside the unbounded
     * [minstring, maxstring] lex range. */
    orderedIndexInsert(oi, 0.0, "alpha", 5);
    orderedIndexInsert(oi, 0.0, "\xff", 1);
    orderedIndexInsert(oi, 0.0, "\xff\x00tail", 6);
    orderedIndexInsert(oi, 0.0, "\xff\xff\xff", 3);

    ASSERT_EQ(orderedIndexCountLexRange(oi, shared.minstring, shared.maxstring, 0, 0), 4UL);

    ASSERT_EQ(orderedIndexDeleteRangeByLex(oi, shared.minstring, shared.maxstring, 0, 0, NULL, NULL), 4UL);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
}

/* ========== Memory tests ========== */

TEST_F(OrderedIndexTest, PointerDeleteCollapsesSpilledPrefixSubtree) {
    /* Members long enough that inner-node shared prefixes exceed the
     * embedded capacity and spill to a heap buffer, and enough of them to
     * build a three-level tree so emptied children include inner nodes.
     * Deleting every item must release the spilled prefix buffers
     * (LeakSanitizer verifies). */
    enum { N = 5000,
           PREFIX = 300 };
    static OrderedIndexItem *items[N];
    char buf[PREFIX + 8];
    memset(buf, 'p', PREFIX);
    for (int i = 0; i < N; i++) {
        snprintf(buf + PREFIX, 8, "%05d", i);
        items[i] = orderedIndexInsert(oi, 1.0, buf, PREFIX + 5);
    }
    ASSERT_EQ(orderedIndexLength(oi), (unsigned long)N);

    for (int i = 0; i < N; i++) orderedIndexDelete(oi, items[i]);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
}

TEST_F(OrderedIndexTest, RangeDeleteCollapsesSpilledPrefixSubtree) {
    /* Same construction, collapsed through the lex range-delete boundary
     * paths instead of item-by-item deletion. */
    enum { N = 5000,
           PREFIX = 300 };
    char buf[PREFIX + 8];
    memset(buf, 'p', PREFIX);
    for (int i = 0; i < N; i++) {
        snprintf(buf + PREFIX, 8, "%05d", i);
        orderedIndexInsert(oi, 1.0, buf, PREFIX + 5);
    }

    snprintf(buf + PREFIX, 8, "%05d", 500);
    sds min = sdsnewlen(buf, PREFIX + 5);
    snprintf(buf + PREFIX, 8, "%05d", 4000);
    sds max = sdsnewlen(buf, PREFIX + 5);
    ASSERT_EQ(orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, NULL, NULL), 3501UL);
    ASSERT_EQ(orderedIndexDeleteRangeByLex(oi, shared.minstring, shared.maxstring, 0, 0, NULL, NULL), (unsigned long)(N - 3501));
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
    sdsfree(min);
    sdsfree(max);
}

TEST_F(OrderedIndexTest, SameLeafRangeDeleteCollapsesSpilledPrefixSubtree) {
    /* Same construction, collapsed through single-member lex range deletes.
     * Each delete resolves both boundaries to the same leaf, so emptied
     * ancestors are released by the same-leaf shared-path walk. Deleting
     * every member this way must release the spilled prefix buffers of the
     * inner nodes that empty along the way (LeakSanitizer verifies). */
    enum { N = 5000,
           PREFIX = 300 };
    char buf[PREFIX + 8];
    memset(buf, 'p', PREFIX);
    for (int i = 0; i < N; i++) {
        snprintf(buf + PREFIX, 8, "%05d", i);
        orderedIndexInsert(oi, 1.0, buf, PREFIX + 5);
    }
    ASSERT_EQ(orderedIndexLength(oi), (unsigned long)N);

    for (int i = 0; i < N; i++) {
        snprintf(buf + PREFIX, 8, "%05d", i);
        sds member = sdsnewlen(buf, PREFIX + 5);
        ASSERT_EQ(orderedIndexDeleteRangeByLex(oi, member, member, 0, 0, NULL, NULL), 1UL);
        sdsfree(member);
    }
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
}

TEST_F(OrderedIndexTest, EstimateStructureMemoryTracksTreeShape) {
    /* The structure estimate covers nodes only: it grows with item count
     * and is independent of member payload size. */
    enum { N = 200,
           BIG = 1024 };
    for (int i = 0; i < N; i++) {
        char b[16];
        snprintf(b, sizeof(b), "s%05d", i);
        orderedIndexInsert(oi, 1.0, b, 6);
    }
    size_t small_members = orderedIndexEstimateStructureMemory(oi);
    ASSERT_GT(small_members, 0UL);

    orderedIndexFree(oi);
    oi = orderedIndexCreate();
    static char big[BIG];
    memset(big, 'b', BIG);
    for (int i = 0; i < N; i++) {
        snprintf(big, 8, "%06d", i);
        big[7] = 'x';
        orderedIndexInsert(oi, 1.0, big, BIG);
    }
    size_t big_members = orderedIndexEstimateStructureMemory(oi);

    /* Same shape, same structural cost — payload bytes are not included. */
    ASSERT_EQ(big_members, small_members);
    ASSERT_LT(big_members, (size_t)N * BIG);

    /* Ten times the items needs roughly ten times the leaves. The band is
     * loose at the low end because the single root inner node is a fixed
     * cost that dominates small trees. */
    for (int i = N; i < N * 10; i++) {
        snprintf(big, 8, "%06d", i);
        big[7] = 'x';
        orderedIndexInsert(oi, 1.0, big, BIG);
    }
    size_t tenfold = orderedIndexEstimateStructureMemory(oi);
    ASSERT_GT(tenfold, big_members * 3);
    ASSERT_LT(tenfold, big_members * 20);
}

TEST_F(OrderedIndexTest, DismissMemoryWalksItems) {
    /* Dismissal hints memory to the OS for contents this process will not
     * read again (fork child after serialization). At this layer the index
     * is opaque, so the observable contract is coverage: at least one hint
     * per item (plus the index's own nodes). */
    enum { N = 200 };
    populateSequential(N);
    MockValkey mock;
    EXPECT_CALL(mock, zmadvise_dontneed(_, _)).Times(AtLeast(N));
    orderedIndexDismissMemory(oi);
}

/* ========== Defrag tests (OrderedIndex layer) ========== */

/* These exercise orderedIndexDefragInternals + orderedIndexScanDefrag  -- the
 * wrappers defrag.c actually calls. The fbtree-layer tests cover node/leaf/item
 * relocation mechanics; here we pin the interface seam: struct-pointer
 * reassignment, cursor plumbing, and the item-relocation callback that defrag.c
 * uses to repoint the companion hashtable.
 *
 * A defragfn that unconditionally relocates every allocation (copy to a fresh
 * block, free the original) lets ASAN flag any stale reference the real
 * jemalloc-hinted path would only expose under fragmentation. */

static void *oiDefragForceRelocate(void *ptr) {
    size_t sz = zmalloc_usable_size(ptr);
    void *newptr = zmalloc(sz);
    memcpy(newptr, ptr, sz);
    zfree(ptr);
    return newptr;
}

/* A defragfn that never relocates: the sweep must still terminate cleanly. */
static void *oiDefragNoop(void *ptr) {
    (void)ptr;
    return NULL;
}

/* Counts item relocations reported to the scan. The pointers are recorded, not
 * dereferenced: oiDefragForceRelocate frees the old block before the callback
 * runs, so reading it would be a use-after-free -- which mirrors how defrag.c
 * uses the old pointer only as a hashtable lookup key. */
struct OIDefragRecord {
    int count;
};
static void oiDefragCountingCallback(OrderedIndexItem *old_item, OrderedIndexItem *new_item, void *privdata) {
    (void)old_item;
    (void)new_item;
    ((OIDefragRecord *)privdata)->count++;
}

/* Full defrag with everything relocating: the struct, inner nodes, leaves,
 * and items all move. The callback must fire once per item (the bridge
 * defrag.c relies on), and the index must stay valid and fully readable  -- which
 * it can only be if every relocated pointer was patched through. */
TEST_F(OrderedIndexTest, DefragRelocatesStructNodesLeavesAndItems) {
    enum { N = 3000 };
    for (int i = 0; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%05d", i);
        orderedIndexInsert(oi, (double)i, buf, strlen(buf));
    }
    verifyOI();

    /* Relocate the index's top-level struct. Reassign because the struct
     * pointer itself may move. */
    oi = orderedIndexDefragInternals(oi, oiDefragForceRelocate);
    ASSERT_NE(oi, nullptr);

    /* Sweep: one leaf per call, with inner nodes relocated by the call that
     * visits their leftmost descendant leaf. */
    OIDefragRecord rec = {0};
    unsigned long cursor = 0;
    do {
        cursor = orderedIndexScanDefrag(oi, cursor, oiDefragCountingCallback, &rec, oiDefragForceRelocate);
    } while (cursor != 0);

    ASSERT_EQ((unsigned long)rec.count, (unsigned long)N); /* every item reported */
    verifyOI();
    ASSERT_EQ(orderedIndexLength(oi), (unsigned long)N);

    /* Content intact after everything moved. */
    for (int i = 0; i < N; i++) {
        OrderedIndexItem *it = orderedIndexGetByIndex(oi, i);
        char buf[32];
        snprintf(buf, sizeof(buf), "key%05d", i);
        assertScore(it, (double)i);
        assertElement(it, buf);
    }
    ASSERT_EQ(orderedIndexCountScoreRange(oi, NEG_INF, POS_INF, 0, 0), (unsigned long)N);
}

/* A no-op defragfn moves nothing: the sweep must still traverse and terminate,
 * the callback must never fire, and content must be untouched. */
TEST_F(OrderedIndexTest, DefragNoRelocationLeavesIndexUntouched) {
    populateSequential(500);
    verifyOI();

    oi = orderedIndexDefragInternals(oi, oiDefragNoop);
    ASSERT_NE(oi, nullptr);

    OIDefragRecord rec = {0};
    unsigned long cursor = 0;
    do {
        cursor = orderedIndexScanDefrag(oi, cursor, oiDefragCountingCallback, &rec, oiDefragNoop);
    } while (cursor != 0);

    ASSERT_EQ(rec.count, 0); /* nothing moved -> callback never fires */
    verifyOI();
    ASSERT_EQ(orderedIndexLength(oi), 500UL);
    ASSERT_EQ(orderedIndexCountScoreRange(oi, NEG_INF, POS_INF, 0, 0), 500UL);
}

/* Defrag on an empty index must be a clean no-op: the struct pass returns a
 * usable index and the scan reports completion immediately. */
TEST_F(OrderedIndexTest, DefragEmptyIndexIsNoop) {
    oi = orderedIndexDefragInternals(oi, oiDefragForceRelocate);
    ASSERT_NE(oi, nullptr);

    OIDefragRecord rec = {0};
    unsigned long cursor = orderedIndexScanDefrag(oi, 0, oiDefragCountingCallback, &rec, oiDefragForceRelocate);
    ASSERT_EQ(cursor, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
    verifyOI();
}

/* ========== On-Delete Callback Tests ========== */

struct OnDeleteRecord {
    int count;
    int capacity;
    sds *elements;
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

static void testOnDeleteCallback(const OrderedIndexItem *item, void *ctx) {
    OnDeleteRecord *rec = (OnDeleteRecord *)ctx;
    const char *ptr;
    size_t len;
    orderedIndexItemGetElement(item, &ptr, &len);
    rec->elements[rec->count] = sdsnewlen(ptr, len);
    rec->count++;
    /* Item is freed by the index after this callback returns. */
}


/* DeleteRangeByScore */

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByScore_EmptyAndNoMatch) {
    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);

    unsigned long deleted = orderedIndexDeleteRangeByScore(oi, 0.0, 10.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    orderedIndexFree(oi);

    oi = orderedIndexCreate();
    insertN(5);
    rec.count = 0;
    deleted = orderedIndexDeleteRangeByScore(oi, 10.0, 20.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(orderedIndexLength(oi), 5UL);
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByScore_Subset) {
    insertN(10);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = orderedIndexDeleteRangeByScore(oi, 3.0, 6.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 4UL);
    ASSERT_EQ(rec.count, 4);
    ASSERT_EQ(orderedIndexLength(oi), 6UL);
    verifyOI();

    sortSdsArray(rec.elements, rec.count);
    ASSERT_SDS_ARRAY_EQ(rec.elements, rec.count, "key3", "key4", "key5", "key6");

    {
        size_t _rn;
        sds *_r = collectIndexToSds(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "key0", "key1", "key2", "key7", "key8", "key9");
        freeSdsArray(_r, _rn);
    }
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByScore_All) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = orderedIndexDeleteRangeByScore(oi, NEG_INF, POS_INF, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 5UL);
    ASSERT_EQ(rec.count, 5);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
    verifyOI();
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByScore_NullCallback) {
    insertN(5);

    unsigned long deleted = orderedIndexDeleteRangeByScore(oi, 1.0, 3.0, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(orderedIndexLength(oi), 2UL);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByScore_ExclusiveBounds) {
    insertN(10);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = orderedIndexDeleteRangeByScore(oi, 3.0, 7.0, 1, 1, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    sortSdsArray(rec.elements, rec.count);
    ASSERT_SDS_ARRAY_EQ(rec.elements, rec.count, "key4", "key5", "key6");
    ASSERT_EQ(orderedIndexLength(oi), 7UL);
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByScore_SingleElement) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = orderedIndexDeleteRangeByScore(oi, 2.0, 2.0, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "key2");
    ASSERT_EQ(orderedIndexLength(oi), 4UL);
    freeOnDeleteRecord(&rec);
}

/* DeleteRangeByIndex */

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByIndex_EmptyAndNoMatch) {
    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);

    unsigned long deleted = orderedIndexDeleteRangeByIndex(oi, 0, 4, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    orderedIndexFree(oi);

    oi = orderedIndexCreate();
    insertN(3);
    rec.count = 0;
    deleted = orderedIndexDeleteRangeByIndex(oi, 10, 20, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(orderedIndexLength(oi), 3UL);
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByIndex_Subset) {
    insertN(10);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = orderedIndexDeleteRangeByIndex(oi, 2, 4, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    ASSERT_EQ(orderedIndexLength(oi), 7UL);

    sortSdsArray(rec.elements, rec.count);
    ASSERT_SDS_ARRAY_EQ(rec.elements, rec.count, "key2", "key3", "key4");

    {
        size_t _rn;
        sds *_r = collectIndexToSds(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "key0", "key1", "key5", "key6", "key7", "key8", "key9");
        freeSdsArray(_r, _rn);
    }
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByIndex_All) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = orderedIndexDeleteRangeByIndex(oi, 0, 4, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 5UL);
    ASSERT_EQ(rec.count, 5);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByIndex_NullCallback) {
    insertN(5);

    unsigned long deleted = orderedIndexDeleteRangeByIndex(oi, 2, 4, NULL, NULL);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(orderedIndexLength(oi), 2UL);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByIndex_ExclusiveBounds) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = orderedIndexDeleteRangeByIndex(oi, 2, 2, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "key2");

    {
        size_t _rn;
        sds *_r = collectIndexToSds(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "key0", "key1", "key3", "key4");
        freeSdsArray(_r, _rn);
    }
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByIndex_SingleElement) {
    insertN(5);

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    unsigned long deleted = orderedIndexDeleteRangeByIndex(oi, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "key0");
    ASSERT_EQ(orderedIndexLength(oi), 4UL);
    freeOnDeleteRecord(&rec);
}

/* DeleteRangeByLex */

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByLex_EmptyAndNoMatch) {
    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);

    sds min = sdsnew("a");
    sds max = sdsnew("z");
    unsigned long deleted = orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    sdsfree(min);
    sdsfree(max);
    orderedIndexFree(oi);

    oi = orderedIndexCreate();
    {
        const char *_l[] = {"apple", "banana", "cherry"};
        insertLex(_l, 3);
    }
    rec.count = 0;
    min = sdsnew("x");
    max = sdsnew("z");
    deleted = orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 0UL);
    ASSERT_EQ(rec.count, 0);
    ASSERT_EQ(orderedIndexLength(oi), 3UL);
    sdsfree(min);
    sdsfree(max);
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByLex_Subset) {
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        insertLex(_l, 5);
    }

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    unsigned long deleted = orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    ASSERT_EQ(orderedIndexLength(oi), 2UL);

    sortSdsArray(rec.elements, rec.count);
    ASSERT_SDS_ARRAY_EQ(rec.elements, rec.count, "banana", "cherry", "date");

    {
        size_t _rn;
        sds *_r = collectIndexToSds(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "apple", "elderberry");
        freeSdsArray(_r, _rn);
    }

    sdsfree(min);
    sdsfree(max);
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByLex_All) {
    {
        const char *_l[] = {"apple", "banana", "cherry"};
        insertLex(_l, 3);
    }

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    sds min = sdsnew("a");
    sds max = sdsnew("z");
    unsigned long deleted = orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 3UL);
    ASSERT_EQ(rec.count, 3);
    ASSERT_EQ(orderedIndexLength(oi), 0UL);

    sdsfree(min);
    sdsfree(max);
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByLex_NullCallback) {
    {
        const char *_l[] = {"apple", "banana", "cherry", "date"};
        insertLex(_l, 4);
    }

    sds min = sdsnew("banana");
    sds max = sdsnew("cherry");
    unsigned long deleted = orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, NULL, NULL);
    ASSERT_EQ(deleted, 2UL);
    ASSERT_EQ(orderedIndexLength(oi), 2UL);

    {
        size_t _rn;
        sds *_r = collectIndexToSds(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "apple", "date");
        freeSdsArray(_r, _rn);
    }

    sdsfree(min);
    sdsfree(max);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByLex_ExclusiveBounds) {
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        insertLex(_l, 5);
    }

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    unsigned long deleted = orderedIndexDeleteRangeByLex(oi, min, max, 1, 1, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "cherry");
    ASSERT_EQ(orderedIndexLength(oi), 4UL);

    {
        size_t _rn;
        sds *_r = collectIndexToSds(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "apple", "banana", "date", "elderberry");
        freeSdsArray(_r, _rn);
    }

    sdsfree(min);
    sdsfree(max);
    freeOnDeleteRecord(&rec);
}

TEST_F(OrderedIndexTest, OnDelete_DeleteRangeByLex_SingleElement) {
    {
        const char *_l[] = {"apple", "banana", "cherry"};
        insertLex(_l, 3);
    }

    OnDeleteRecord rec;
    initOnDeleteRecord(&rec, 10);
    sds min = sdsnew("banana");
    sds max = sdsnew("banana");
    unsigned long deleted = orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, testOnDeleteCallback, &rec);
    ASSERT_EQ(deleted, 1UL);
    ASSERT_EQ(rec.count, 1);
    ASSERT_STREQ(rec.elements[0], "banana");
    ASSERT_EQ(orderedIndexLength(oi), 2UL);

    {
        size_t _rn;
        sds *_r = collectIndexToSds(oi, &_rn);
        ASSERT_SDS_ARRAY_EQ(_r, _rn, "apple", "cherry");
        freeSdsArray(_r, _rn);
    }

    sdsfree(min);
    sdsfree(max);
    freeOnDeleteRecord(&rec);
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

static void hashtableConsistencyOnDelete(const OrderedIndexItem *item, void *ctx) {
    SimHt *ht = (SimHt *)ctx;
    const char *ptr;
    size_t len;
    orderedIndexItemGetElement(item, &ptr, &len);
    simHtRemove(ht, ptr, len);
}


/* Helper: populate index + SimHt with N sequential elements. */
static void populateIndexAndHt(OrderedIndex *oi, SimHt *ht, int n) {
    for (int i = 0; i < n; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key%d", i);
        orderedIndexInsert(oi, (double)i, buf, strlen(buf));
        simHtAdd(ht, buf, strlen(buf));
    }
}

/* Helper: populate index + SimHt with lex elements at same score. */
static void populateIndexAndHtLex(OrderedIndex *oi, SimHt *ht, const char *elems[], int count, double score) {
    for (int i = 0; i < count; i++) {
        orderedIndexInsert(oi, score, elems[i], strlen(elems[i]));
        simHtAdd(ht, elems[i], strlen(elems[i]));
    }
}

/* Helper: assert SimHt contents match index contents. */
static void assertHtMatchesIndex(OrderedIndex *oi, SimHt *ht) {
    size_t idx_n;
    sds *idx_elems = collectIndexToSds(oi, &idx_n);
    sortSdsArray(idx_elems, idx_n);
    simHtSort(ht);
    ASSERT_EQ(idx_n, (size_t)ht->count);
    for (size_t i = 0; i < idx_n; i++) {
        ASSERT_STREQ(idx_elems[i], ht->elems[i]);
    }
    freeSdsArray(idx_elems, idx_n);
}

/* ByScore */

TEST_F(OrderedIndexTest, HtConsistency_ByScore_PartialDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    populateIndexAndHt(oi, &simulatedHt, 10);

    orderedIndexDeleteRangeByScore(oi, 3.0, 6.0, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(oi, &simulatedHt);
    simHtFree(&simulatedHt);
}

TEST_F(OrderedIndexTest, HtConsistency_ByScore_FullDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    populateIndexAndHt(oi, &simulatedHt, 10);

    orderedIndexDeleteRangeByScore(oi, NEG_INF, POS_INF, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(oi, &simulatedHt);
    simHtFree(&simulatedHt);
}

TEST_F(OrderedIndexTest, HtConsistency_ByScore_EmptyRange) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    populateIndexAndHt(oi, &simulatedHt, 10);

    orderedIndexDeleteRangeByScore(oi, 20.0, 30.0, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(oi, &simulatedHt);
    simHtFree(&simulatedHt);
}

/* ByIndex */

TEST_F(OrderedIndexTest, HtConsistency_ByIndex_PartialDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    populateIndexAndHt(oi, &simulatedHt, 10);

    orderedIndexDeleteRangeByIndex(oi, 2, 4, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(oi, &simulatedHt);
    simHtFree(&simulatedHt);
}

TEST_F(OrderedIndexTest, HtConsistency_ByIndex_FullDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    populateIndexAndHt(oi, &simulatedHt, 10);

    orderedIndexDeleteRangeByIndex(oi, 0, 9, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(oi, &simulatedHt);
    simHtFree(&simulatedHt);
}

TEST_F(OrderedIndexTest, HtConsistency_ByIndex_EmptyRange) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    populateIndexAndHt(oi, &simulatedHt, 10);

    orderedIndexDeleteRangeByIndex(oi, 20, 30, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(oi, &simulatedHt);
    simHtFree(&simulatedHt);
}

/* ByLex */

TEST_F(OrderedIndexTest, HtConsistency_ByLex_PartialDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        populateIndexAndHtLex(oi, &simulatedHt, _l, 5, 1.0);
    }

    sds min = sdsnew("banana");
    sds max = sdsnew("date");
    orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(oi, &simulatedHt);

    sdsfree(min);
    sdsfree(max);
    simHtFree(&simulatedHt);
}

TEST_F(OrderedIndexTest, HtConsistency_ByLex_FullDelete) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        populateIndexAndHtLex(oi, &simulatedHt, _l, 5, 1.0);
    }

    sds min = sdsnew("a");
    sds max = sdsnew("z");
    orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(oi, &simulatedHt);

    sdsfree(min);
    sdsfree(max);
    simHtFree(&simulatedHt);
}

TEST_F(OrderedIndexTest, HtConsistency_ByLex_EmptyRange) {
    SimHt simulatedHt;
    simHtInit(&simulatedHt, 20);
    {
        const char *_l[] = {"apple", "banana", "cherry", "date", "elderberry"};
        populateIndexAndHtLex(oi, &simulatedHt, _l, 5, 1.0);
    }

    sds min = sdsnew("zzz");
    sds max = sdsnew("zzzz");
    orderedIndexDeleteRangeByLex(oi, min, max, 0, 0, hashtableConsistencyOnDelete, &simulatedHt);

    assertHtMatchesIndex(oi, &simulatedHt);

    sdsfree(min);
    sdsfree(max);
    simHtFree(&simulatedHt);
}
