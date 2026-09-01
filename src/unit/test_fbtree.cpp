/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

extern "C" {
#include "fbtree.h"
#include "fbtree_internal.h"
#include "sds.h"
#include "zmalloc.h"
}

/* Derived constants for test readability */
#define TEST_TWO_LEVEL_ITEMS (NODE_SIZE * NODE_SIZE)
#define TEST_THREE_LEVEL_ITEMS (TEST_TWO_LEVEL_ITEMS + 200)

/* ========== Test Helpers ========== */

/* Create a null-terminated sds from a C string. */
static sds createString(const char *str) {
    size_t len = strlen(str) + 1;
    return sdsnewlen(str, len);
}

/* Create a string like "prefix" + base-26 encoded value + "suffix".
 * E.g., value=0 -> "AAA", value=1 -> "AAB", value=26 -> "ABA". */
static sds createBase26TestString(const char *prefix, const char *suffix, size_t value, size_t value_width) {
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    size_t len = prefix_len + suffix_len + value_width + 1;
    sds s = sdsnewlen(NULL, len);
    memcpy(s, prefix, prefix_len);

    for (size_t i = 0; i < value_width; i++) {
        char c = (char)('A' + (value % 26));
        s[prefix_len + value_width - 1 - i] = c;
        value /= 26;
    }

    memcpy(s + prefix_len + value_width, suffix, suffix_len);
    s[len - 1] = '\0';
    return s;
}

static sds createPrefixString(const char *prefix_char, size_t prefix_len, const char *suffix) {
    size_t suffix_len = strlen(suffix) + 1; /* include null terminator */
    sds s = sdsnewlen(NULL, prefix_len + suffix_len);
    memset(s, prefix_char[0], prefix_len);
    memcpy(s + prefix_len, suffix, suffix_len);
    return s;
}

/* ========== RAII Fixture ========== */

/* Base fixture that handles tree lifecycle and memory-leak detection. */
class FbtreeTest : public ::testing::Test {
  protected:
    fbtreeIndex *fbt = nullptr;
    size_t mem_before = 0;

    void SetUp() override {
        mem_before = zmalloc_used_memory();
        fbt = fbtreeCreate();
    }

    void TearDown() override {
        if (fbt) fbtreeFree(fbt);
        EXPECT_EQ(zmalloc_used_memory(), mem_before) << "Memory leak detected";
    }

    /* Validate tree invariants. */
    void expectValid() {
        ASSERT_TRUE(fbtreeDebugValidate(fbt, false, NULL, 0)) << "Tree invariant violation";
    }

    /* Insert a null-terminated C string, return the stored pointer. */
    sds insert(const char *str) {
        return fbtreeInsert(fbt, createString(str));
    }

    /* Collect all elements via forward iteration into a vector. */
    std::vector<std::string> collectForward() {
        std::vector<std::string> result;
        fbtreeIterator it;
        fbtreeInitIterator(&it, fbt);
        const_sds pos;
        while ((pos = fbtreeNext(&it)) != nullptr) {
            result.emplace_back(pos, sdslen(pos));
        }
        return result;
    }

    /* Collect all elements via backward iteration into a vector. */
    std::vector<std::string> collectBackward() {
        std::vector<std::string> result;
        fbtreeIterator it;
        fbtreeInitIterator(&it, fbt);
        const_sds pos;
        while ((pos = fbtreePrev(&it)) != nullptr) {
            result.emplace_back(pos, sdslen(pos));
        }
        return result;
    }
};

/* ========== Basic Lifecycle Tests ========== */

TEST_F(FbtreeTest, CreateAndFree) {
    ASSERT_NE(fbt, nullptr);
    expectValid();
    /* TearDown verifies no leak */
}

/* Verify node sizes fit expected jemalloc size classes.
 * innerNode should fit in 2048-byte class, leafNode in 512-byte class.
 * This catches accidental struct bloat that wastes memory. */
TEST_F(FbtreeTest, NodeAllocationSizes) {
#ifndef USE_JEMALLOC
    GTEST_SKIP() << "Node size-class check is jemalloc-specific";
#endif
    void *inner_test = zmalloc(2048); /* sizeof(innerNode) rounds up to 2048 */
    void *leaf_test = zmalloc(512);   /* sizeof(leafNode) */

    EXPECT_EQ(zmalloc_usable_size(inner_test), 2048u);
    EXPECT_EQ(zmalloc_usable_size(leaf_test), 512u);

    zfree(inner_test);
    zfree(leaf_test);
}

/* ========== Insert & Lookup Tests ========== */

TEST_F(FbtreeTest, InsertAndLookup) {
    sds inserted = insert("hello");
    expectValid();

    EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted), 0);

    sds missing = createString("world");
    EXPECT_LT(fbtreeGetIndexOfItem(fbt, missing), 0);
    sdsfree(missing);
}

TEST_F(FbtreeTest, InsertMultiple) {
    const char *strings[] = {"apple", "banana", "cherry", "date", "elderberry", "elder"};
    sds inserted[6];
    for (int i = 0; i < 6; i++) {
        inserted[i] = insert(strings[i]);
    }
    expectValid();

    for (int i = 0; i < 6; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }

    sds missing = createString("fig");
    EXPECT_LT(fbtreeGetIndexOfItem(fbt, missing), 0);
    sdsfree(missing);
}

TEST_F(FbtreeTest, LookupEmptyTree) {
    expectValid();
    sds s = createString("anything");
    EXPECT_LT(fbtreeGetIndexOfItem(fbt, s), 0);
    sdsfree(s);
}

TEST_F(FbtreeTest, LengthIncrements) {
    EXPECT_EQ(fbtreeLength(fbt), 0u);
    insert("first");
    EXPECT_EQ(fbtreeLength(fbt), 1u);
    insert("second");
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    insert("third");
    EXPECT_EQ(fbtreeLength(fbt), 3u);
    expectValid();
}

TEST_F(FbtreeTest, DuplicateInsert) {
    sds ins1 = insert("key");
    EXPECT_EQ(fbtreeLength(fbt), 1u);

    sds ins2 = insert("key");
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    expectValid();

    EXPECT_GE(fbtreeGetIndexOfItem(fbt, ins1), 0);
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, ins2), 0);
}

TEST_F(FbtreeTest, EmptyString) {
    sds inserted = insert("");
    expectValid();
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted), 0);
}

TEST_F(FbtreeTest, LongStrings) {
    char long_str[256];
    memset(long_str, 'a', 255);
    long_str[255] = '\0';

    sds inserted = insert(long_str);
    expectValid();
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted), 0);
}

/* Binary data with embedded null bytes - sds handles this, tree should too. */
TEST_F(FbtreeTest, BinaryDataWithNullBytes) {
    /* Create sds with embedded nulls: "ab\0cd\0ef" */
    char data1[] = {'a', 'b', '\0', 'c', 'd', '\0', 'e', 'f'};
    char data2[] = {'a', 'b', '\0', 'c', 'e', '\0', 'e', 'f'}; /* differs at byte 4 */
    char data3[] = {'a', 'b', '\0', 'c', 'd', '\0', 'e', 'g'}; /* differs at byte 7 */

    sds s1 = fbtreeInsert(fbt, sdsnewlen(data1, sizeof(data1)));
    sds s2 = fbtreeInsert(fbt, sdsnewlen(data2, sizeof(data2)));
    sds s3 = fbtreeInsert(fbt, sdsnewlen(data3, sizeof(data3)));
    expectValid();

    EXPECT_EQ(fbtreeLength(fbt), 3u);
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, s1), 0);
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, s2), 0);
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, s3), 0);

    /* Verify sorted order via iteration */
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    const_sds prev = nullptr;
    int count = 0;
    while ((pos = fbtreeNext(&it)) != nullptr) {
        if (prev) {
            EXPECT_LT(sdscmp(prev, pos), 0);
        }
        prev = pos;
        count++;
    }
    EXPECT_EQ(count, 3);
}

/* ========== Forward Iterator Tests ========== */

TEST_F(FbtreeTest, IteratorSmall) {
    const char *strings[] = {"dog", "cat", "ant", "bat"};
    for (auto s : strings) insert(s);
    expectValid();

    auto items = collectForward();
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0], std::string("ant\0", 4));
    EXPECT_EQ(items[1], std::string("bat\0", 4));
    EXPECT_EQ(items[2], std::string("cat\0", 4));
    EXPECT_EQ(items[3], std::string("dog\0", 4));
}

TEST_F(FbtreeTest, IteratorFullLeaf) {
    const int count = NODE_SIZE + 1;
    char buf[8];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "k%02d", i);
        insert(buf);
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "k%02d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(sdslen(pos), 4u);
        EXPECT_EQ(memcmp(pos, buf, 4), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, IteratorReverseInsert) {
    const int count = NODE_SIZE + 1;
    char buf[8];
    for (int i = count - 1; i >= 0; i--) {
        snprintf(buf, sizeof(buf), "k%02d", i);
        insert(buf);
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "k%02d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, 4), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, IteratorEmpty) {
    expectValid();
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, IteratorResetInvalidates) {
    for (auto s : {"a", "b", "c"}) insert(s);
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);

    fbtreeResetIterator(&it);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

/* ========== Ordering Tests ========== */

TEST_F(FbtreeTest, MultilevelReverseInsert) {
    char buf[8];
    for (int i = 95; i >= 0; i--) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        insert(buf);
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i <= 95; i++) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, 5), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    fbtreeInitIterator(&it, fbt);
    for (int i = 95; i >= 0; i--) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        ASSERT_NE(pos = fbtreePrev(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, 5), 0);
    }
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, PrefixOrdering) {
    const char *strings[] = {"elderberry", "elder", "e", "elderly"};
    for (auto s : strings) insert(s);
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "e", 2), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "elder", 6), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "elderberry", 11), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "elderly", 8), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, SameLengthOrdering) {
    const char *strings[] = {"zoo", "abc", "xyz", "def"};
    for (auto s : strings) insert(s);
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "abc", 4), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "def", 4), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "xyz", 4), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "zoo", 4), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, CommonPrefixOrdering) {
    const char *keys[] = {"prefix_aaa", "prefix_aab", "prefix_aac", "prefix_aad",
                          "prefix_baa", "prefix_bab", "prefix_bac", "prefix_bad",
                          "prefix_caa", "prefix_cab", "prefix_cac", "prefix_cad"};
    sds inserted[12];
    for (int i = 0; i < 12; i++) {
        inserted[i] = insert(keys[i]);
    }
    expectValid();

    for (int i = 0; i < 12; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < 12; i++) {
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        size_t len = strlen(keys[i]) + 1;
        EXPECT_EQ(memcmp(pos, keys[i], len), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, InsertBatchesSorted) {
    for (auto s : {"dog", "cat", "ant"}) insert(s);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "ant", 4), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "cat", 4), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "dog", 4), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    for (auto s : {"bat", "elk"}) insert(s);
    expectValid();

    fbtreeInitIterator(&it, fbt);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "ant", 4), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "bat", 4), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "cat", 4), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "dog", 4), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "elk", 4), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, InsertAtBoundaries) {
    insert("m");
    for (auto s : {"a", "z", "n"}) insert(s);
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "a", 2), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "m", 2), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "n", 2), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "z", 2), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

/* ========== Backward Iterator (Prev) Tests ========== */

TEST_F(FbtreeTest, PrevSmall) {
    const char *strings[] = {"dog", "cat", "ant", "bat"};
    for (auto s : strings) insert(s);
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "dog", 4), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "cat", 4), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "bat", 4), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "ant", 4), 0);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, PrevFullLeaf) {
    const int count = NODE_SIZE + 1;
    char buf[8];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "k%02d", i);
        insert(buf);
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = count - 1; i >= 0; i--) {
        snprintf(buf, sizeof(buf), "k%02d", i);
        ASSERT_NE(pos = fbtreePrev(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, 4), 0);
    }
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, PrevEmpty) {
    expectValid();
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, PrevNextMixed) {
    for (auto s : {"a", "b", "c", "d", "e"}) insert(s);
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "a", 2), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "b", 2), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "b", 2), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "a", 2), 0);

    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "a", 2), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "b", 2), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "c", 2), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "c", 2), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "b", 2), 0);
}

TEST_F(FbtreeTest, PrevSingle) {
    insert("only");
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "only", 5), 0);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, IteratorExhaustedStaysInvalid) {
    insert("x");
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    /* Exhaust forward - repeated calls stay false */
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    /* Can reverse from exhausted-forward state */
    EXPECT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_STREQ(pos, "x");

    /* Exhaust backward - repeated calls stay false */
    fbtreeInitIterator(&it, fbt);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
    /* Can reverse from exhausted-backward state */
    EXPECT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_STREQ(pos, "x");

    /* Can reverse from last item */
    fbtreeInitIterator(&it, fbt);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    /* Reverse still works after exhaustion */
    EXPECT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_STREQ(pos, "x");

    /* Same for first item */
    fbtreeInitIterator(&it, fbt);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
    /* Reverse still works after exhaustion */
    EXPECT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_STREQ(pos, "x");
}

/* ========== Multi-Level Tree Tests ========== */

TEST_F(FbtreeTest, MultilevelLookup) {
    const int count = NODE_SIZE + 1;
    char buf[8];
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "k%02d", i);
        inserted[i] = insert(buf);
    }
    expectValid();

    for (int i = 0; i < count; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }
    zfree(inserted);

    sds s1 = createString("k99");
    EXPECT_LT(fbtreeGetIndexOfItem(fbt, s1), 0);
    sdsfree(s1);
    sds s2 = createString("x00");
    EXPECT_LT(fbtreeGetIndexOfItem(fbt, s2), 0);
    sdsfree(s2);
}

TEST_F(FbtreeTest, MultilevelForwardIteration) {
    const int count = NODE_SIZE * 7 / 2;
    char buf[8];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        insert(buf);
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, 5), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, MultilevelBackwardIteration) {
    const int count = NODE_SIZE * 7 / 2;
    char buf[8];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        insert(buf);
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = count - 1; i >= 0; i--) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        ASSERT_NE(pos = fbtreePrev(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, 5), 0);
    }
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, MultilevelMixedIteration) {
    const int total = NODE_SIZE * 7 / 2;
    char buf[8];
    for (int i = 0; i < total; i++) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        insert(buf);
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), (size_t)total);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    for (int i = 0; i < 10; i++) {
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    }
    EXPECT_EQ(memcmp(pos, "k009", 5), 0);

    for (int i = 0; i < 5; i++) {
        ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    }
    EXPECT_EQ(memcmp(pos, "k005", 5), 0);

    int count = 1;
    const_sds last = pos;
    while ((pos = fbtreeNext(&it)) != nullptr) {
        last = pos;
        count++;
    }
    EXPECT_EQ(count, total - 5 + 1);
    snprintf(buf, sizeof(buf), "k%03d", total - 1);
    EXPECT_EQ(memcmp(last, buf, 5), 0);

    fbtreeResetIterator(&it);
}

TEST_F(FbtreeTest, MultilevelCrossLeafIteration) {
    char buf[8];
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        insert(buf);
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    const int boundary = NODE_SIZE;
    for (int i = 0; i < boundary - 5; i++) {
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    }

    for (int i = boundary - 5; i < boundary + 5; i++) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, 5), 0);
    }
}

/* ========== Inner Node Split Tests (3+ Level Trees) ========== */

TEST_F(FbtreeTest, InnerSplitSequential) {
    const int count = TEST_THREE_LEVEL_ITEMS;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        sds str = createBase26TestString("key_", "", i, 3);
        inserted[i] = fbtreeInsert(fbt, str);
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), (size_t)count);

    for (int i = 0; i < count; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }
    zfree(inserted);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count; i++) {
        sds expected = createBase26TestString("key_", "", i, 3);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(sdscmp(pos, expected), 0);
        sdsfree(expected);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, InnerSplitReverse) {
    const int count = TEST_THREE_LEVEL_ITEMS;
    char buf[16];
    for (int i = count - 1; i >= 0; i--) {
        snprintf(buf, sizeof(buf), "key_%05d", i);
        insert(buf);
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), (size_t)count);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%05d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    fbtreeInitIterator(&it, fbt);
    for (int i = count - 1; i >= 0; i--) {
        snprintf(buf, sizeof(buf), "key_%05d", i);
        ASSERT_NE(pos = fbtreePrev(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, InnerSplitShuffled) {
    int *indices = (int *)zmalloc(5000 * sizeof(int));
    for (int i = 0; i < 5000; i++) indices[i] = i;
    for (int i = 4999; i > 0; i--) {
        int j = i % 1000;
        int temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }

    char buf[16];
    for (int i = 0; i < 5000; i++) {
        snprintf(buf, sizeof(buf), "rnd_%05d", indices[i]);
        insert(buf);
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), 5000u);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < 5000; i++) {
        snprintf(buf, sizeof(buf), "rnd_%05d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    zfree(indices);
}

/* ========== Deep Tree Tests (4+ Levels) ========== */

TEST_F(FbtreeTest, DeepTree4Levels) {
    const int count = NODE_SIZE * NODE_SIZE * NODE_SIZE + 10000;
    char buf[16];
    sds first_item = NULL, middle_item = NULL, last_item = NULL;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "deep_%06d", i);
        sds inserted = insert(buf);
        if (i == 0)
            first_item = inserted;
        else if (i == count / 2)
            middle_item = inserted;
        else if (i == count - 1)
            last_item = inserted;
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), (size_t)count);

    EXPECT_GE(fbtreeGetIndexOfItem(fbt, first_item), 0);
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, middle_item), 0);
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, last_item), 0);

    sds search_str = createString("deep_300000");
    EXPECT_LT(fbtreeGetIndexOfItem(fbt, search_str), 0);
    sdsfree(search_str);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count / 2; i++) {
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    }
    for (int i = count / 2; i < count / 2 + 100; i++) {
        snprintf(buf, sizeof(buf), "deep_%06d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
}

TEST_F(FbtreeTest, DeepTreeMixedInsertPatterns) {
    char buf[16];

    for (int i = 0; i < 10000; i++) {
        snprintf(buf, sizeof(buf), "mix_%06d", i * 3);
        insert(buf);
    }
    for (int i = 9999; i >= 0; i--) {
        snprintf(buf, sizeof(buf), "mix_%06d", i * 3 + 1);
        insert(buf);
    }

    int *indices = (int *)zmalloc(10000 * sizeof(int));
    for (int i = 0; i < 10000; i++) indices[i] = i * 3 + 2;
    for (int i = 9999; i > 0; i--) {
        int j = (i * 17) % (i + 1);
        int temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
    for (int i = 0; i < 10000; i++) {
        snprintf(buf, sizeof(buf), "mix_%06d", indices[i]);
        insert(buf);
    }
    zfree(indices);
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), 30000u);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < 30000; i++) {
        snprintf(buf, sizeof(buf), "mix_%06d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    fbtreeInitIterator(&it, fbt);
    for (int i = 0; i < 1000; i++) {
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    }
    for (int i = 0; i < 500; i++) {
        ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    }
    snprintf(buf, sizeof(buf), "mix_%06d", 500);
    EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
}

/* ========== String Pattern Tests ========== */

TEST_F(FbtreeTest, VariedStringPatterns) {
    char buf[32];
    sds *inserted = (sds *)zmalloc(4000 * sizeof(sds));
    int idx = 0;

    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "common_prefix_%04d_suffix", i);
        inserted[idx++] = insert(buf);
    }
    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "prefix_%04d_common_suffix", i);
        inserted[idx++] = insert(buf);
    }
    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "pal_%04d_%04d_lap", i, i);
        inserted[idx++] = insert(buf);
    }
    for (int i = 0; i < 1000; i++) {
        char c = 'a' + (i % 26);
        snprintf(buf, sizeof(buf), "repeat_%c%c%c%c_%04d", c, c, c, c, i);
        inserted[idx++] = insert(buf);
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), 4000u);

    for (int i = 0; i < 4000; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }
    zfree(inserted);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    const_sds prev_pos = NULL;
    int count = 0;
    while ((pos = fbtreeNext(&it)) != nullptr) {
        if (prev_pos) {
            EXPECT_LE(sdscmp(prev_pos, pos), 0);
        }
        prev_pos = pos;
        count++;
    }
    EXPECT_EQ(count, 4000);
}

/* ========== Split Boundary Tests ========== */

TEST_F(FbtreeTest, SplitAtExactBoundary) {
    char buf[16];
    const int count = TEST_TWO_LEVEL_ITEMS;

    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "bound_%05d", i);
        insert(buf);
    }

    snprintf(buf, sizeof(buf), "bound_%05d", count);
    sds boundary_item = insert(buf);
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), (size_t)(count + 1));
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, boundary_item), 0);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count - 6; i++) {
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    }
    for (int i = count - 6; i <= count; i++) {
        snprintf(buf, sizeof(buf), "bound_%05d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, AlternatingMinMaxInsert) {
    const int count = TEST_TWO_LEVEL_ITEMS;
    char buf[16];
    int min_val = 0, max_val = count - 1;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));

    while (min_val <= max_val) {
        snprintf(buf, sizeof(buf), "mid_%06d", min_val);
        inserted[min_val] = insert(buf);
        min_val++;
        if (min_val > max_val) break;
        snprintf(buf, sizeof(buf), "mid_%06d", max_val);
        inserted[max_val] = insert(buf);
        max_val--;
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), (size_t)count);

    for (int i = 0; i < count; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }
    zfree(inserted);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count; i++) {
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        snprintf(buf, sizeof(buf), "mid_%06d", i);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    fbtreeInitIterator(&it, fbt);
    for (int i = count - 1; i >= 0; i--) {
        ASSERT_NE(pos = fbtreePrev(&it), nullptr);
        snprintf(buf, sizeof(buf), "mid_%06d", i);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, SequentialMiddleInsert) {
    char buf[16];

    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "seq_%06d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    for (int i = 3000; i < 4000; i++) {
        snprintf(buf, sizeof(buf), "seq_%06d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), 2000u);

    for (int i = 1000; i < 2000; i++) {
        snprintf(buf, sizeof(buf), "seq_%06d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    for (int i = 2999; i >= 2000; i--) {
        snprintf(buf, sizeof(buf), "seq_%06d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), 4000u);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < 4000; i++) {
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        snprintf(buf, sizeof(buf), "seq_%06d", i);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

/* ========== Delete Tests ========== */

TEST_F(FbtreeTest, DeleteSingleItem) {
    sds inserted = insert("test");
    EXPECT_EQ(fbtreeLength(fbt), 1u);

    EXPECT_TRUE(fbtreeDelete(fbt, inserted));
    EXPECT_EQ(fbtreeLength(fbt), 0u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteNonexistent) {
    sds inserted = insert("exists");

    sds other = createString("missing");
    EXPECT_FALSE(fbtreeDelete(fbt, other));
    sdsfree(other);

    EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted), 0);
}

TEST_F(FbtreeTest, DeleteFromEmpty) {
    sds dummy = createString("anything");
    EXPECT_FALSE(fbtreeDelete(fbt, dummy));
    sdsfree(dummy);
}

TEST_F(FbtreeTest, DeleteAllItems) {
    char buf[8];
    sds inserted[10];
    for (int i = 0; i < 10; i++) {
        snprintf(buf, sizeof(buf), "k%02d", i);
        inserted[i] = insert(buf);
    }
    EXPECT_EQ(fbtreeLength(fbt), 10u);

    for (int i = 0; i < 10; i++) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
        EXPECT_EQ(fbtreeLength(fbt), 10u - i - 1u);
    }
    EXPECT_EQ(fbtreeLength(fbt), 0u);
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    fbtreeInitIterator(&it, fbt);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, DeleteMiddleItem) {
    char buf[8];
    sds inserted[50];
    for (int i = 0; i < 50; i++) {
        snprintf(buf, sizeof(buf), "k%02d", i);
        inserted[i] = insert(buf);
    }

    EXPECT_TRUE(fbtreeDelete(fbt, inserted[25]));
    EXPECT_EQ(fbtreeLength(fbt), 49u);
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[24]), 0);
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[26]), 0);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    int count = 0;
    while ((pos = fbtreeNext(&it)) != nullptr) count++;
    EXPECT_EQ(count, 49);
    expectValid();
}

TEST_F(FbtreeTest, DeleteMaxUpdatesAnchor) {
    char buf[16];
    sds inserted[200];
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        inserted[i] = insert(buf);
    }
    expectValid();

    EXPECT_TRUE(fbtreeDelete(fbt, inserted[199]));
    EXPECT_EQ(fbtreeLength(fbt), 199u);
    expectValid();
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[198]), 0);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    int count = 0;
    while ((pos = fbtreePrev(&it)) != nullptr) count++;
    EXPECT_EQ(count, 199);
}

TEST_F(FbtreeTest, DeleteAllMultilevel) {
    char buf[16];
    const int count = NODE_SIZE * 3;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    for (int i = 0; i < count; i++) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
        expectValid();
    }
    EXPECT_EQ(fbtreeLength(fbt), 0u);

    zfree(inserted);
}

TEST_F(FbtreeTest, DeleteRootCollapse) {
    char buf[16];
    const int overflow = 10;
    const int count = NODE_SIZE + overflow;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    for (int i = 0; i < NODE_SIZE; i++) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), (size_t)overflow);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    int remaining = 0;
    while ((pos = fbtreeNext(&it)) != nullptr) remaining++;
    EXPECT_EQ(remaining, overflow);

    zfree(inserted);
}

TEST_F(FbtreeTest, DeleteLeftmostLeafUpdatesCache) {
    char buf[16];
    const int count = NODE_SIZE * 3;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }

    for (int i = 0; i < NODE_SIZE; i++) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    snprintf(buf, sizeof(buf), "key_%03d", NODE_SIZE);
    EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);

    zfree(inserted);
}

TEST_F(FbtreeTest, DeleteRightmostLeafUpdatesCache) {
    char buf[16];
    const int count = NODE_SIZE * 3;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }

    for (int i = count - 1; i >= count - NODE_SIZE; i--) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    snprintf(buf, sizeof(buf), "key_%03d", count - NODE_SIZE - 1);
    EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);

    zfree(inserted);
}

/* Delete in reverse order - stresses different anchor-update paths than forward delete. */
TEST_F(FbtreeTest, DeleteReverseOrder) {
    char buf[16];
    const int count = NODE_SIZE * 3;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    /* Delete from highest to lowest */
    for (int i = count - 1; i >= 0; i--) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
        expectValid();
    }
    EXPECT_EQ(fbtreeLength(fbt), 0u);

    zfree(inserted);
}

/* Interleaved insert and delete - exercises tree after structural changes from deletion. */
TEST_F(FbtreeTest, InterleavedInsertDelete) {
    char buf[16];
    const int batch = 100;
    sds *inserted = (sds *)zmalloc(batch * sizeof(sds));

    /* Insert first batch */
    for (int i = 0; i < batch; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    /* Delete even-indexed items */
    for (int i = 0; i < batch; i += 2) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    EXPECT_EQ(fbtreeLength(fbt), 50u);
    expectValid();

    /* Insert new items into the gaps */
    sds *new_inserted = (sds *)zmalloc(batch * sizeof(sds));
    for (int i = 0; i < batch; i++) {
        snprintf(buf, sizeof(buf), "new_%03d", i);
        new_inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    EXPECT_EQ(fbtreeLength(fbt), 150u);
    expectValid();

    /* Verify all remaining items are findable */
    for (int i = 1; i < batch; i += 2) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }
    for (int i = 0; i < batch; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, new_inserted[i]), 0);
    }

    /* Verify sorted iteration */
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    const_sds prev_pos = nullptr;
    int count = 0;
    while ((pos = fbtreeNext(&it)) != nullptr) {
        if (prev_pos) {
            EXPECT_LT(sdscmp(prev_pos, pos), 0);
        }
        prev_pos = pos;
        count++;
    }
    EXPECT_EQ(count, 150);

    zfree(inserted);
    zfree(new_inserted);
}

/* ========== Rank Tests ========== */

TEST_F(FbtreeTest, RankSingleLeaf) {
    const char *items[] = {"cherry", "apple", "banana", "date"};
    for (auto s : items) insert(s);

    /* Sorted order: apple(0), banana(1), cherry(2), date(3) */
    const_sds result = fbtreeGetAtRank(fbt, 0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(memcmp(result, "apple", 6), 0);

    result = fbtreeGetAtRank(fbt, 1);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(memcmp(result, "banana", 7), 0);

    result = fbtreeGetAtRank(fbt, 2);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(memcmp(result, "cherry", 7), 0);

    result = fbtreeGetAtRank(fbt, 3);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(memcmp(result, "date", 5), 0);

    EXPECT_EQ(fbtreeGetAtRank(fbt, 4), nullptr);
}

TEST_F(FbtreeTest, RankMultilevel) {
    char buf[16];
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        insert(buf);
    }
    expectValid();

    EXPECT_EQ(memcmp(fbtreeGetAtRank(fbt, 0), "key_000", 8), 0);
    EXPECT_EQ(memcmp(fbtreeGetAtRank(fbt, 50), "key_050", 8), 0);
    EXPECT_EQ(memcmp(fbtreeGetAtRank(fbt, 100), "key_100", 8), 0);
    EXPECT_EQ(memcmp(fbtreeGetAtRank(fbt, 199), "key_199", 8), 0);
    EXPECT_EQ(fbtreeGetAtRank(fbt, 200), nullptr);
}

TEST_F(FbtreeTest, RankAfterDelete) {
    char buf[16];
    sds inserted[100];
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        inserted[i] = insert(buf);
    }

    EXPECT_TRUE(fbtreeDelete(fbt, inserted[50]));

    const_sds result = fbtreeGetAtRank(fbt, 50);
    EXPECT_EQ(memcmp(result, "key_051", 8), 0);
    result = fbtreeGetAtRank(fbt, 49);
    EXPECT_EQ(memcmp(result, "key_049", 8), 0);
}

TEST_F(FbtreeTest, GetRankOfItem) {
    char buf[16];
    sds inserted[100];
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        inserted[i] = insert(buf);
    }

    EXPECT_EQ(fbtreeGetIndexOfItem(fbt, inserted[0]), 0);
    EXPECT_EQ(fbtreeGetIndexOfItem(fbt, inserted[50]), 50);
    EXPECT_EQ(fbtreeGetIndexOfItem(fbt, inserted[99]), 99);

    sds search = createString("key_100");
    EXPECT_EQ(fbtreeGetIndexOfItem(fbt, search), -1);
    sdsfree(search);
}

TEST_F(FbtreeTest, GetAtRankEmptyTree) {
    EXPECT_EQ(fbtreeGetAtRank(fbt, 0), nullptr);
    EXPECT_EQ(fbtreeGetAtRank(fbt, 1), nullptr);
}

TEST_F(FbtreeTest, SeekToRank) {
    char buf[16];
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        insert(buf);
    }

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    fbtreeSeekToRank(&it, 50);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_050", 8), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_051", 8), 0);

    fbtreeSeekToRank(&it, 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_000", 8), 0);

    fbtreeSeekToRank(&it, 99);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_099", 8), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

/* SeekToRank then iterate backward. */
TEST_F(FbtreeTest, SeekToRankThenPrev) {
    char buf[16];
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        insert(buf);
    }

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    /* Seek to rank 100, then iterate backward */
    fbtreeSeekToRank(&it, 100);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_099", 8), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_098", 8), 0);

    /* Seek to rank 0, prev should fail (nothing before first element) */
    fbtreeSeekToRank(&it, 0);
    /* Next returns rank 0 */
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_000", 8), 0);
}

/* SeekToRank out of bounds. */
TEST_F(FbtreeTest, SeekToRankOutOfBounds) {
    char buf[16];
    for (int i = 0; i < 10; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        insert(buf);
    }

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    /* Seek past end */
    fbtreeSeekToRank(&it, 100);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    /* Prev recovers from past-end (single-leaf tree) */
    fbtreeSeekToRank(&it, 100);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_009", 8), 0);

    /* Seek to exact length (one past last valid rank) */
    fbtreeSeekToRank(&it, 10);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_009", 8), 0);

    /* Seek to last valid rank */
    fbtreeSeekToRank(&it, 9);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "key_009", 8), 0);
}

TEST_F(FbtreeTest, SeekToRankOutOfBoundsMultilevel) {
    const int count = NODE_SIZE * 3;
    char buf[16];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%05d", i);
        insert(buf);
    }

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    /* Seek past end */
    fbtreeSeekToRank(&it, count + 100);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    /* Prev recovers from past-end */
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    snprintf(buf, sizeof(buf), "key_%05d", count - 1);
    EXPECT_EQ(memcmp(pos, buf, strlen(buf)), 0);

    /* Seek to exact length */
    fbtreeSeekToRank(&it, count);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, SeekToRankEmptyTree) {
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    fbtreeSeekToRank(&it, 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, RankDeepTree) {
    const int count = TEST_THREE_LEVEL_ITEMS;
    char buf[16];
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%05d", i);
        inserted[i] = insert(buf);
    }
    expectValid();

    EXPECT_EQ(memcmp(fbtreeGetAtRank(fbt, 0), "key_00000", 10), 0);

    snprintf(buf, sizeof(buf), "key_%05d", count - 1);
    EXPECT_EQ(memcmp(fbtreeGetAtRank(fbt, count - 1), buf, 10), 0);

    for (int i = 0; i < count; i += count / 10) {
        EXPECT_EQ(fbtreeGetAtRank(fbt, i), inserted[i]);
    }
    for (int i = 0; i < count; i += count / 10) {
        EXPECT_EQ(fbtreeGetIndexOfItem(fbt, inserted[i]), i);
    }
    zfree(inserted);
}

/* ========== fbtreeSeekToScore Tests ========== */

TEST_F(FbtreeTest, SeekToScoreExact) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("BBBBBBBB", &it);

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "BBBBBBBB", 8), 0);
}

TEST_F(FbtreeTest, SeekToScoreBetween) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_1"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("BBBBBBBB", &it);

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "CCCCCCCC", 8), 0);
}

TEST_F(FbtreeTest, SeekToScorePastEnd) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("ZZZZZZZZ", &it);

    const_sds pos;
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, SeekToScorePastEndThenPrev) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("ZZZZZZZZ", &it);

    const_sds pos;
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "CCCCCCCC", 8), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "BBBBBBBB", 8), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "AAAAAAAA", 8), 0);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);

    /* Forward from past-end — Next fails, but Prev can be used */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("ZZZZZZZZ", &it);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "CCCCCCCC", 8), 0);
}

TEST_F(FbtreeTest, SeekToScoreBeforeStartThenNext) {
    fbtreeInsert(fbt, createString("MMMMMMMMelem_0"));
    fbtreeInsert(fbt, createString("NNNNNNNNelem_1"));
    fbtreeInsert(fbt, createString("OOOOOOOOelem_2"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("AAAAAAAA", &it);

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "MMMMMMMM", 8), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "NNNNNNNN", 8), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "OOOOOOOO", 8), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    /* Backward from before-start — Prev fails, but Next can be used */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("AAAAAAAA", &it);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "MMMMMMMM", 8), 0);
}

TEST_F(FbtreeTest, SeekToScoreBeforeStart) {
    fbtreeInsert(fbt, createString("MMMMMMMMelem"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("AAAAAAAA", &it);

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "MMMMMMMM", 8), 0);
}

TEST_F(FbtreeTest, SeekToScoreEmpty) {
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("AAAAAAAA", &it);

    const_sds pos;
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

/* ========== Seek-then-reverse boundary state transitions ==========
 * A seek that lands on the first element leaves the iterator in BEFORE_START;
 * fbtreeNext() must clear that state so a following fbtreePrev() returns the
 * element rather than early-returning NULL. Symmetric for PAST_END with
 * Prev-then-Next. These cover the ZREVRANGEBYLEX-of-the-smallest-element bug. */

TEST_F(FbtreeTest, SeekToScoreRank0NextThenPrev) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("AAAAAAAA", &it); /* exact rank-0 element -> BEFORE_START */

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "AAAAAAAA", 8), 0);
    /* Next consumed rank 0; Prev must return it, not NULL. */
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "AAAAAAAA", 8), 0);
    /* Now genuinely before the start. */
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, SeekToScoreBelowAllNextThenPrev) {
    fbtreeInsert(fbt, createString("MMMMMMMMelem_0"));
    fbtreeInsert(fbt, createString("NNNNNNNNelem_1"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("AAAAAAAA", &it); /* below all -> rank 0 / BEFORE_START */

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "MMMMMMMM", 8), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "MMMMMMMM", 8), 0);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, SeekToScoreSingleElementRank0Reverse) {
    fbtreeInsert(fbt, createString("KKKKKKKKonly"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("KKKKKKKK", &it); /* sole element, rank 0 */

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "KKKKKKKK", 8), 0);
    /* The reported ZREVRANGEBYLEX-of-smallest bug: Prev must return it. */
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "KKKKKKKK", 8), 0);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

TEST_F(FbtreeTest, SeekToScorePastEndPrevThenNext) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("ZZZZZZZZ", &it); /* past end -> PAST_END */

    const_sds pos;
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "CCCCCCCC", 8), 0);
    /* Prev consumed the last element; Next must return it, not NULL. */
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "CCCCCCCC", 8), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, SeekToValueRank0NextThenPrev) {
    /* Value-level analog of the ZREVRANGEBYLEX rank-0 bug (same score prefix,
     * distinct elements; seek to the exact smallest packed value). */
    sds a = createString("AAAAAAAAaaa");
    fbtreeInsert(fbt, a);
    fbtreeInsert(fbt, createString("AAAAAAAAbbb"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(a, &it); /* exact rank-0 value */

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(sdscmp(pos, a), 0);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(sdscmp(pos, a), 0);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

/* Seek to exact score match then prev - verifies prev returns element before the match. */
TEST_F(FbtreeTest, SeekToScoreExactThenPrev) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("BBBBBBBB", &it);

    const_sds pos;
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "AAAAAAAA", 8), 0);
}

/* Seek to score on single-element tree. */
TEST_F(FbtreeTest, SeekToScoreSingleElement) {
    fbtreeInsert(fbt, createString("MMMMMMMMonly"));

    fbtreeIterator it;
    const_sds pos;

    /* Exact match */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("MMMMMMMM", &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "MMMMMMMM", 8), 0);

    /* Before */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("AAAAAAAA", &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "MMMMMMMM", 8), 0);

    /* After */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("ZZZZZZZZ", &it);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
}

TEST_F(FbtreeTest, SeekToScoreDeepTree) {
    const int count = TEST_THREE_LEVEL_ITEMS;
    char buf[24];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "%08d_elem_%05d", i, i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    snprintf(buf, sizeof(buf), "%08d", count / 2);
    fbtreeSeekToScore(buf, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, buf, 8), 0);

    snprintf(buf, sizeof(buf), "%08d", count - 10);
    fbtreeSeekToScore(buf, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, buf, 8), 0);
}

TEST_F(FbtreeTest, SeekToScoreIterate) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_2"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_3"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_4"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_0"));

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("BBBBBBBB", &it);

    const_sds pos;
    int count = 0;
    while (((pos = fbtreeNext(&it)) != nullptr) && memcmp(pos, "BBBBBBBB", 8) == 0) {
        count++;
    }
    EXPECT_EQ(count, 5);
}

/* Seek to score with duplicate scores - verify boundary positioning. */
TEST_F(FbtreeTest, SeekToScoreDuplicateScores) {
    /* Insert multiple elements with same 8-byte score prefix */
    fbtreeInsert(fbt, createString("AAAAAAAAfirst"));
    fbtreeInsert(fbt, createString("BBBBBBBBa_elem"));
    fbtreeInsert(fbt, createString("BBBBBBBBb_elem"));
    fbtreeInsert(fbt, createString("BBBBBBBBc_elem"));
    fbtreeInsert(fbt, createString("CCCCCCCClast"));

    fbtreeIterator it;
    const_sds pos;

    /* Seek to "BBBBBBBB" - should land at first B element */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("BBBBBBBB", &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "BBBBBBBBa_elem", 15), 0);

    /* Prev from that position should return the A element */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToScore("BBBBBBBB", &it);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "AAAAAAAA", 8), 0);
}

/* ========== Inner Node Binary Search Tests ========== */

TEST_F(FbtreeTest, InnerBsearchIdenticalFeatureBytes) {
    sds outlier = fbtreeInsert(fbt, createString("A_outlier"));

    const int count = NODE_SIZE + 10;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    char buf[16];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "XXXX_%05d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), (size_t)(count + 1));

    for (int i = 0; i < count; i++) {
        EXPECT_EQ(fbtreeGetIndexOfItem(fbt, inserted[i]), i + 1);
    }
    EXPECT_EQ(fbtreeGetIndexOfItem(fbt, outlier), 0);

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "A_outlier", 10), 0);
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "XXXX_%05d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    zfree(inserted);
}

/* ========== Long Prefix Tests ========== */

TEST_F(FbtreeTest, LongPrefixBasic) {
    const size_t prefix_len = EMBED_PREFIX_LEN + 6;
    const int count = TEST_TWO_LEVEL_ITEMS;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    char suffix[8];

    for (int i = 0; i < count; i++) {
        snprintf(suffix, sizeof(suffix), "s%05d", i);
        inserted[i] = fbtreeInsert(fbt, createPrefixString("X", prefix_len, suffix));
    }
    EXPECT_EQ(fbtreeLength(fbt), (size_t)count);
    expectValid();

    for (int i = 0; i < count; i++) {
        EXPECT_EQ(fbtreeGetIndexOfItem(fbt, inserted[i]), i);
    }

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count; i++) {
        snprintf(suffix, sizeof(suffix), "s%05d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos + prefix_len, suffix, 6), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    zfree(inserted);
}

TEST_F(FbtreeTest, VeryLongPrefix) {
    const size_t prefix_len = EMBED_PREFIX_LEN * 10;
    const int count = NODE_SIZE + 10;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    char suffix[8];

    for (int i = 0; i < count; i++) {
        snprintf(suffix, sizeof(suffix), "s%03d", i);
        inserted[i] = fbtreeInsert(fbt, createPrefixString("P", prefix_len, suffix));
    }
    EXPECT_EQ(fbtreeLength(fbt), (size_t)count);
    expectValid();

    for (int i = 0; i < count; i++) {
        EXPECT_EQ(fbtreeGetIndexOfItem(fbt, inserted[i]), i);
    }

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = 0; i < count; i++) {
        snprintf(suffix, sizeof(suffix), "s%03d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos + prefix_len, suffix, 5), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    zfree(inserted);
}

TEST_F(FbtreeTest, LongPrefixMultilevel) {
    const size_t prefix_len = EMBED_PREFIX_LEN + 14;
    const int count = TEST_THREE_LEVEL_ITEMS;
    char suffix[16];

    for (int i = 0; i < count; i++) {
        snprintf(suffix, sizeof(suffix), "item_%05d", i);
        fbtreeInsert(fbt, createPrefixString("L", prefix_len, suffix));
    }
    EXPECT_EQ(fbtreeLength(fbt), (size_t)count);
    expectValid();

    const_sds first = fbtreeGetAtRank(fbt, 0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(memcmp(first + prefix_len, "item_00000", 11), 0);

    const_sds last = fbtreeGetAtRank(fbt, count - 1);
    snprintf(suffix, sizeof(suffix), "item_%05d", count - 1);
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(memcmp(last + prefix_len, suffix, 11), 0);
}

TEST_F(FbtreeTest, LongPrefixDelete) {
    const size_t prefix_len = EMBED_PREFIX_LEN + 4;
    const int count = NODE_SIZE * 2;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    char suffix[8];

    for (int i = 0; i < count; i++) {
        snprintf(suffix, sizeof(suffix), "d%03d", i);
        inserted[i] = fbtreeInsert(fbt, createPrefixString("D", prefix_len, suffix));
    }
    expectValid();

    for (int i = 0; i < count; i += 2) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    EXPECT_EQ(fbtreeLength(fbt), (size_t)(count / 2));
    expectValid();

    for (int i = 1; i < count; i += 2) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }

    zfree(inserted);
}

TEST_F(FbtreeTest, LongPrefixBoundary) {
    const int count = NODE_SIZE + 10;
    char suffix[8];

    /* Case 1: prefix exactly at EMBED_PREFIX_LEN (embedded storage) */
    {
        /* Use a separate tree for this sub-case */
        fbtreeIndex *fbt1 = fbtreeCreate();
        sds *inserted = (sds *)zmalloc(count * sizeof(sds));
        for (int i = 0; i < count; i++) {
            snprintf(suffix, sizeof(suffix), "%c%04d", 'a' + (i % 26), i);
            inserted[i] = fbtreeInsert(fbt1, createPrefixString("X", EMBED_PREFIX_LEN, suffix));
        }
        EXPECT_TRUE(fbtreeDebugValidate(fbt1, false, NULL, 0));
        for (int i = 0; i < count; i++)
            EXPECT_GE(fbtreeGetIndexOfItem(fbt1, inserted[i]), 0);
        zfree(inserted);
        fbtreeFree(fbt1);
    }

    /* Case 2: prefix at EMBED_PREFIX_LEN+1 (long prefix storage) */
    {
        fbtreeIndex *fbt2 = fbtreeCreate();
        sds *inserted = (sds *)zmalloc(count * sizeof(sds));
        for (int i = 0; i < count; i++) {
            snprintf(suffix, sizeof(suffix), "%c%04d", 'a' + (i % 26), i);
            inserted[i] = fbtreeInsert(fbt2, createPrefixString("X", EMBED_PREFIX_LEN + 1, suffix));
        }
        EXPECT_TRUE(fbtreeDebugValidate(fbt2, false, NULL, 0));
        for (int i = 0; i < count; i++)
            EXPECT_GE(fbtreeGetIndexOfItem(fbt2, inserted[i]), 0);
        zfree(inserted);
        fbtreeFree(fbt2);
    }
}

TEST_F(FbtreeTest, LongPrefixShrinkToShort) {
    const size_t long_prefix = EMBED_PREFIX_LEN + 14;
    const int count = NODE_SIZE + 10;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    char suffix[8];

    for (int i = 0; i < count; i++) {
        snprintf(suffix, sizeof(suffix), "s%03d", i);
        inserted[i] = fbtreeInsert(fbt, createPrefixString("A", long_prefix, suffix));
    }
    expectValid();

    sds outlier = fbtreeInsert(fbt, createPrefixString("B", long_prefix, "zzz"));
    expectValid();

    for (int i = 0; i < count; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, outlier), 0);

    EXPECT_TRUE(fbtreeDelete(fbt, outlier));
    expectValid();

    for (int i = 0; i < count; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }

    zfree(inserted);
}

TEST_F(FbtreeTest, LongPrefixRealloc) {
    const size_t long_prefix = EMBED_PREFIX_LEN * 2;
    const int count = NODE_SIZE + 10;
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    char suffix[8];

    for (int i = 0; i < count; i++) {
        snprintf(suffix, sizeof(suffix), "s%03d", i);
        inserted[i] = fbtreeInsert(fbt, createPrefixString("X", long_prefix, suffix));
    }
    expectValid();

    const size_t mid_prefix = EMBED_PREFIX_LEN + 4;
    sds s_mid = sdsnewlen(NULL, mid_prefix + 5);
    memset(s_mid, 'X', mid_prefix);
    memcpy(s_mid + mid_prefix, "Ymid", 5);
    sds item_mid = fbtreeInsert(fbt, s_mid);
    expectValid();

    const size_t longer_prefix = EMBED_PREFIX_LEN + 14;
    sds s_longer = sdsnewlen(NULL, longer_prefix + 5);
    memset(s_longer, 'X', longer_prefix);
    memcpy(s_longer + longer_prefix, "Zlng", 5);
    sds item_longer = fbtreeInsert(fbt, s_longer);
    expectValid();

    for (int i = 0; i < count; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, item_mid), 0);
    EXPECT_GE(fbtreeGetIndexOfItem(fbt, item_longer), 0);

    EXPECT_TRUE(fbtreeDelete(fbt, item_mid));
    EXPECT_TRUE(fbtreeDelete(fbt, item_longer));
    expectValid();

    for (int i = 0; i < count; i++) {
        EXPECT_GE(fbtreeGetIndexOfItem(fbt, inserted[i]), 0);
    }

    zfree(inserted);
}

/* ========== Pop Min/Max Tests ========== */

TEST_F(FbtreeTest, PopMinSingle) {
    insert("only");
    EXPECT_EQ(fbtreeLength(fbt), 1u);

    sds popped = fbtreePopMin(fbt);
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(memcmp(popped, "only", 5), 0);
    EXPECT_EQ(fbtreeLength(fbt), 0u);
    expectValid();
    sdsfree(popped);

    EXPECT_EQ(fbtreePopMin(fbt), nullptr);
}

TEST_F(FbtreeTest, PopMaxSingle) {
    insert("only");
    EXPECT_EQ(fbtreeLength(fbt), 1u);

    sds popped = fbtreePopMax(fbt);
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(memcmp(popped, "only", 5), 0);
    EXPECT_EQ(fbtreeLength(fbt), 0u);
    expectValid();
    sdsfree(popped);

    EXPECT_EQ(fbtreePopMax(fbt), nullptr);
}

TEST_F(FbtreeTest, PopMinMultiple) {
    const char *items[] = {"cherry", "apple", "banana", "date"};
    for (auto s : items) fbtreeInsert(fbt, createString(s));

    const char *expected[] = {"apple", "banana", "cherry", "date"};
    size_t expected_lens[] = {6, 7, 7, 5};
    for (int i = 0; i < 4; i++) {
        sds popped = fbtreePopMin(fbt);
        ASSERT_NE(popped, nullptr);
        EXPECT_EQ(memcmp(popped, expected[i], expected_lens[i]), 0);
        sdsfree(popped);
        expectValid();
    }
    EXPECT_EQ(fbtreeLength(fbt), 0u);
}

TEST_F(FbtreeTest, PopMaxMultiple) {
    const char *items[] = {"cherry", "apple", "banana", "date"};
    for (auto s : items) fbtreeInsert(fbt, createString(s));

    const char *expected[] = {"date", "cherry", "banana", "apple"};
    size_t expected_lens[] = {5, 7, 7, 6};
    for (int i = 0; i < 4; i++) {
        sds popped = fbtreePopMax(fbt);
        ASSERT_NE(popped, nullptr);
        EXPECT_EQ(memcmp(popped, expected[i], expected_lens[i]), 0);
        sdsfree(popped);
        expectValid();
    }
    EXPECT_EQ(fbtreeLength(fbt), 0u);
}

TEST_F(FbtreeTest, PopMinMultilevel) {
    char buf[16];
    const int count = NODE_SIZE * 3;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    for (int i = 0; i < count; i++) {
        sds popped = fbtreePopMin(fbt);
        ASSERT_NE(popped, nullptr);
        snprintf(buf, sizeof(buf), "key_%03d", i);
        EXPECT_EQ(memcmp(popped, buf, strlen(buf) + 1), 0);
        sdsfree(popped);
        expectValid();
    }
    EXPECT_EQ(fbtreeLength(fbt), 0u);
}

TEST_F(FbtreeTest, PopMaxMultilevel) {
    char buf[16];
    const int count = NODE_SIZE * 3;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    for (int i = count - 1; i >= 0; i--) {
        sds popped = fbtreePopMax(fbt);
        ASSERT_NE(popped, nullptr);
        snprintf(buf, sizeof(buf), "key_%03d", i);
        EXPECT_EQ(memcmp(popped, buf, strlen(buf) + 1), 0);
        sdsfree(popped);
        expectValid();
    }
    EXPECT_EQ(fbtreeLength(fbt), 0u);
}

TEST_F(FbtreeTest, PopAlternating) {
    char buf[16];
    const int count = 100;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        fbtreeInsert(fbt, createString(buf));
    }

    int min_idx = 0, max_idx = count - 1;
    for (int i = 0; i < count; i++) {
        sds popped;
        if (i % 2 == 0) {
            popped = fbtreePopMin(fbt);
            snprintf(buf, sizeof(buf), "key_%03d", min_idx++);
        } else {
            popped = fbtreePopMax(fbt);
            snprintf(buf, sizeof(buf), "key_%03d", max_idx--);
        }
        ASSERT_NE(popped, nullptr);
        EXPECT_EQ(memcmp(popped, buf, strlen(buf) + 1), 0);
        sdsfree(popped);
        expectValid();
    }
    EXPECT_EQ(fbtreeLength(fbt), 0u);
}

TEST_F(FbtreeTest, PopEmpty) {
    EXPECT_EQ(fbtreePopMin(fbt), nullptr);
    EXPECT_EQ(fbtreePopMax(fbt), nullptr);
}

/* Pop from deep (3+ level) tree - exercises pop through multiple inner node levels. */
TEST_F(FbtreeTest, PopMinDeepTree) {
    const int count = TEST_THREE_LEVEL_ITEMS;
    char buf[16];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%05d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    /* Pop first 100 items from deep tree */
    for (int i = 0; i < 100; i++) {
        sds popped = fbtreePopMin(fbt);
        ASSERT_NE(popped, nullptr);
        snprintf(buf, sizeof(buf), "key_%05d", i);
        EXPECT_EQ(memcmp(popped, buf, strlen(buf) + 1), 0);
        sdsfree(popped);
    }
    EXPECT_EQ(fbtreeLength(fbt), (size_t)(count - 100));
    expectValid();
}

TEST_F(FbtreeTest, PopMaxDeepTree) {
    const int count = TEST_THREE_LEVEL_ITEMS;
    char buf[16];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%05d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    /* Pop last 100 items from deep tree */
    for (int i = count - 1; i >= count - 100; i--) {
        sds popped = fbtreePopMax(fbt);
        ASSERT_NE(popped, nullptr);
        snprintf(buf, sizeof(buf), "key_%05d", i);
        EXPECT_EQ(memcmp(popped, buf, strlen(buf) + 1), 0);
        sdsfree(popped);
    }
    EXPECT_EQ(fbtreeLength(fbt), (size_t)(count - 100));
    expectValid();
}

TEST_F(FbtreeTest, PopWithIterationAndInsert) {
    char buf[16];
    sds popped;
    const_sds pos;

    /* Insert ascending: k100-k199 */
    for (int i = 100; i < 200; i++) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    EXPECT_EQ(fbtreeLength(fbt), 100u);

    /* Pop min a few times */
    for (int i = 100; i < 110; i++) {
        popped = fbtreePopMin(fbt);
        snprintf(buf, sizeof(buf), "k%03d", i);
        EXPECT_EQ(memcmp(popped, buf, strlen(buf) + 1), 0);
        sdsfree(popped);
        expectValid();
    }
    EXPECT_EQ(fbtreeLength(fbt), 90u);

    /* Insert descending: k099 down to k050 */
    for (int i = 99; i >= 50; i--) {
        snprintf(buf, sizeof(buf), "k%03d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    EXPECT_EQ(fbtreeLength(fbt), 140u);

    /* Pop max a few times */
    for (int i = 199; i >= 190; i--) {
        popped = fbtreePopMax(fbt);
        snprintf(buf, sizeof(buf), "k%03d", i);
        EXPECT_EQ(memcmp(popped, buf, strlen(buf) + 1), 0);
        sdsfree(popped);
        expectValid();
    }
    EXPECT_EQ(fbtreeLength(fbt), 130u);

    /* Iterate forward: k050-k189 (excluding popped k100-k109) */
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    for (int i = 50; i < 190; i++) {
        if (i >= 100 && i < 110) continue;
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        snprintf(buf, sizeof(buf), "k%03d", i);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    /* Iterate backward */
    fbtreeInitIterator(&it, fbt);
    for (int i = 189; i >= 50; i--) {
        if (i >= 100 && i < 110) continue;
        ASSERT_NE(pos = fbtreePrev(&it), nullptr);
        snprintf(buf, sizeof(buf), "k%03d", i);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
}

/* ========== fbtreeSeekToValue Tests ========== */

TEST_F(FbtreeTest, SeekToValueExact) {
    insert("apple");
    insert("banana");
    insert("cherry");

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    sds seek_val = createString("banana");
    fbtreeSeekToValue(seek_val, &it);

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "banana", 7), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "cherry", 7), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    sdsfree(seek_val);
}

TEST_F(FbtreeTest, SeekToValueBetween) {
    insert("apple");
    insert("cherry");
    insert("elderberry");

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    sds seek_val = createString("banana");
    fbtreeSeekToValue(seek_val, &it);

    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "cherry", 7), 0);

    sdsfree(seek_val);
}

TEST_F(FbtreeTest, SeekToValuePastEnd) {
    insert("apple");
    insert("banana");
    insert("cherry");

    fbtreeIterator it;
    sds seek_val = createString("zzz");
    const_sds pos;

    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    /* Prev from past-end returns last element */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "cherry", 7), 0);

    /* Forward from past-end — Next fails, but Prev can be used */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "cherry", 7), 0);

    sdsfree(seek_val);
}

TEST_F(FbtreeTest, SeekToValueBeforeStart) {
    insert("banana");
    insert("cherry");
    insert("date");

    fbtreeIterator it;
    sds seek_val = createString("apple");
    const_sds pos;

    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "banana", 7), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "cherry", 7), 0);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "date", 5), 0);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    /* Backward from before-start — Prev fails, but Next can be used */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "banana", 7), 0);

    sdsfree(seek_val);
}

TEST_F(FbtreeTest, SeekToValueEmpty) {
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    sds seek_val = createString("anything");
    fbtreeSeekToValue(seek_val, &it);

    const_sds pos;
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);

    sdsfree(seek_val);
}

/* Seek to value on single-element tree. */
TEST_F(FbtreeTest, SeekToValueSingleElement) {
    insert("middle");

    fbtreeIterator it;
    const_sds pos;

    /* Exact match */
    fbtreeInitIterator(&it, fbt);
    sds exact = createString("middle");
    fbtreeSeekToValue(exact, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "middle", 7), 0);
    sdsfree(exact);

    /* Before */
    fbtreeInitIterator(&it, fbt);
    sds before = createString("aaa");
    fbtreeSeekToValue(before, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "middle", 7), 0);
    sdsfree(before);

    /* After */
    fbtreeInitIterator(&it, fbt);
    sds after = createString("zzz");
    fbtreeSeekToValue(after, &it);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    sdsfree(after);
}

TEST_F(FbtreeTest, SeekToValueSharedPrefix) {
    fbtreeInsert(fbt, createString("XXXXXXXXalpha"));
    fbtreeInsert(fbt, createString("XXXXXXXXbravo"));
    fbtreeInsert(fbt, createString("XXXXXXXXcharlie"));
    fbtreeInsert(fbt, createString("XXXXXXXXdelta"));
    fbtreeInsert(fbt, createString("XXXXXXXXecho"));

    fbtreeIterator it;
    const_sds pos;

    /* Exact match within shared-prefix group */
    fbtreeInitIterator(&it, fbt);
    sds seek_val = createString("XXXXXXXXcharlie");
    fbtreeSeekToValue(seek_val, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "XXXXXXXXcharlie", 16), 0);
    sdsfree(seek_val);

    /* Between two shared-prefix elements */
    fbtreeInitIterator(&it, fbt);
    seek_val = createString("XXXXXXXXcat");
    fbtreeSeekToValue(seek_val, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "XXXXXXXXcharlie", 16), 0);
    sdsfree(seek_val);

    /* Prev from that position should return bravo */
    fbtreeInitIterator(&it, fbt);
    seek_val = createString("XXXXXXXXcat");
    fbtreeSeekToValue(seek_val, &it);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "XXXXXXXXbravo", 14), 0);
    sdsfree(seek_val);
}

TEST_F(FbtreeTest, SeekToValueDeepTree) {
    const int count = TEST_THREE_LEVEL_ITEMS;
    for (int i = 0; i < count; i++) {
        sds str = createBase26TestString("val_", "", i, 4);
        fbtreeInsert(fbt, str);
    }
    expectValid();

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;

    sds seek_mid = createBase26TestString("val_", "", count / 2, 4);
    fbtreeSeekToValue(seek_mid, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(sdscmp(pos, seek_mid), 0);
    sdsfree(seek_mid);

    sds seek_near_end = createBase26TestString("val_", "", count - 5, 4);
    fbtreeSeekToValue(seek_near_end, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(sdscmp(pos, seek_near_end), 0);
    sdsfree(seek_near_end);

    sds seek_first = createBase26TestString("val_", "", 0, 4);
    fbtreeSeekToValue(seek_first, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(sdscmp(pos, seek_first), 0);
    sdsfree(seek_first);

    sds seek_past = createString("zzz_past_end");
    fbtreeSeekToValue(seek_past, &it);
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);
    sdsfree(seek_past);
}

TEST_F(FbtreeTest, SeekToValueThenIterate) {
    char buf[16];
    const int count = 200;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "item_%03d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    fbtreeIterator it;
    const_sds pos;
    sds seek_val = createString("item_100");

    /* Forward from seek position */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    for (int i = 100; i < count; i++) {
        snprintf(buf, sizeof(buf), "item_%03d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    /* Backward from seek position */
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    for (int i = 99; i >= 0; i--) {
        snprintf(buf, sizeof(buf), "item_%03d", i);
        ASSERT_NE(pos = fbtreePrev(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreePrev(&it), nullptr);

    sdsfree(seek_val);
}

/* ========== Property-Based SeekToValue Tests ========== */

/* Seek positions iterator at first element >= value (forward) */
TEST_F(FbtreeTest, SeekToValuePropertyForwardPositioning) {
    /* Use a separate tree per iteration - free the fixture tree first */
    fbtreeFree(fbt);
    fbt = nullptr;

    const int NUM_ITERATIONS = 150;
    unsigned int seed = 42;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        fbtreeIndex *tree = fbtreeCreate();

        int num_elements;
        if (iter < 50) {
            num_elements = 1 + (rand_r(&seed) % 60);
        } else if (iter < 100) {
            num_elements = NODE_SIZE + 1 + (rand_r(&seed) % 140);
        } else {
            num_elements = 200 + (rand_r(&seed) % 1300);
        }

        std::vector<sds> sorted_values;
        sorted_values.reserve(num_elements);

        for (int i = 0; i < num_elements; i++) {
            size_t val = rand_r(&seed) % 50000;
            const char *prefixes[] = {"aa_", "bb_", "cc_", "dd_", "ee_"};
            const char *prefix = prefixes[rand_r(&seed) % 5];
            sds s = createBase26TestString(prefix, "", val, 4);
            fbtreeInsert(tree, s);
            sorted_values.push_back(sdsnewlen(s, sdslen(s)));
        }

        std::sort(sorted_values.begin(), sorted_values.end(), [](const sds a, const sds b) {
            return sdscmp(a, b) < 0;
        });

        sds seek_val;
        int seek_type = rand_r(&seed) % 5;
        if (seek_type == 0 && num_elements > 0) {
            int idx = rand_r(&seed) % num_elements;
            seek_val = sdsnewlen(sorted_values[idx], sdslen(sorted_values[idx]));
        } else if (seek_type == 1) {
            seek_val = createString("AAAA");
        } else if (seek_type == 2) {
            seek_val = createString("zzzzzzzz");
        } else {
            size_t val = rand_r(&seed) % 50000;
            const char *prefixes[] = {"aa_", "bb_", "cc_", "dd_", "ee_"};
            const char *prefix = prefixes[rand_r(&seed) % 5];
            seek_val = createBase26TestString(prefix, "", val, 4);
        }

        sds expected = NULL;
        for (size_t i = 0; i < sorted_values.size(); i++) {
            if (sdscmp(sorted_values[i], seek_val) >= 0) {
                expected = sorted_values[i];
                break;
            }
        }

        fbtreeIterator it;
        fbtreeInitIterator(&it, tree);
        fbtreeSeekToValue(seek_val, &it);

        const_sds pos;
        bool got_next = ((pos = fbtreeNext(&it)) != nullptr);

        if (expected == NULL) {
            ASSERT_FALSE(got_next) << "Iteration " << iter << ": expected no element >= seek value, but got one"
                                   << " (seek_val=" << seek_val << ", num_elements=" << num_elements << ")";
        } else {
            ASSERT_TRUE(got_next) << "Iteration " << iter << ": expected element >= seek value, but got none"
                                  << " (seek_val=" << seek_val << ", num_elements=" << num_elements << ")";
            ASSERT_EQ(sdscmp(pos, expected), 0)
                << "Iteration " << iter << ": wrong element returned"
                << " (seek_val=" << seek_val << ", got=" << pos << ", expected=" << expected
                << ", num_elements=" << num_elements << ")";
        }

        sdsfree(seek_val);
        for (auto &v : sorted_values) sdsfree(v);
        fbtreeFree(tree);
    }
}

/* Seek positions iterator correctly for reverse iteration */
TEST_F(FbtreeTest, SeekToValuePropertyReversePositioning) {
    fbtreeFree(fbt);
    fbt = nullptr;

    const int NUM_ITERATIONS = 150;
    unsigned int seed = 123;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        fbtreeIndex *tree = fbtreeCreate();

        int num_elements;
        if (iter < 50) {
            num_elements = 1 + (rand_r(&seed) % 60);
        } else if (iter < 100) {
            num_elements = NODE_SIZE + 1 + (rand_r(&seed) % 140);
        } else {
            num_elements = 200 + (rand_r(&seed) % 1300);
        }

        std::vector<sds> sorted_values;
        sorted_values.reserve(num_elements);

        for (int i = 0; i < num_elements; i++) {
            size_t val = rand_r(&seed) % 50000;
            const char *prefixes[] = {"aa_", "bb_", "cc_", "dd_", "ee_"};
            const char *prefix = prefixes[rand_r(&seed) % 5];
            sds s = createBase26TestString(prefix, "", val, 4);
            fbtreeInsert(tree, s);
            sorted_values.push_back(sdsnewlen(s, sdslen(s)));
        }

        std::sort(sorted_values.begin(), sorted_values.end(), [](const sds a, const sds b) {
            return sdscmp(a, b) < 0;
        });

        sds seek_val;
        int seek_type = rand_r(&seed) % 5;
        if (seek_type == 0 && num_elements > 0) {
            int idx = rand_r(&seed) % num_elements;
            seek_val = sdsnewlen(sorted_values[idx], sdslen(sorted_values[idx]));
        } else if (seek_type == 1) {
            seek_val = createString("AAAA");
        } else if (seek_type == 2) {
            seek_val = createString("zzzzzzzz");
        } else {
            size_t val = rand_r(&seed) % 50000;
            const char *prefixes[] = {"aa_", "bb_", "cc_", "dd_", "ee_"};
            const char *prefix = prefixes[rand_r(&seed) % 5];
            seek_val = createBase26TestString(prefix, "", val, 4);
        }

        sds expected_prev = NULL;
        for (int i = (int)sorted_values.size() - 1; i >= 0; i--) {
            if (sdscmp(sorted_values[i], seek_val) < 0) {
                expected_prev = sorted_values[i];
                break;
            }
        }

        fbtreeIterator it;
        fbtreeInitIterator(&it, tree);
        fbtreeSeekToValue(seek_val, &it);

        const_sds pos;
        bool got_prev = ((pos = fbtreePrev(&it)) != nullptr);

        if (expected_prev == NULL) {
            ASSERT_FALSE(got_prev) << "Iteration " << iter << ": expected no element < seek value, but got one"
                                   << " (seek_val=" << seek_val << ", num_elements=" << num_elements << ")";
        } else {
            ASSERT_TRUE(got_prev) << "Iteration " << iter << ": expected element < seek value, but got none"
                                  << " (seek_val=" << seek_val << ", num_elements=" << num_elements << ")";
            ASSERT_EQ(sdscmp(pos, expected_prev), 0)
                << "Iteration " << iter << ": wrong element returned by fbtreePrev"
                << " (seek_val=" << seek_val << ", got=" << pos << ", expected=" << expected_prev
                << ", num_elements=" << num_elements << ")";
        }

        sdsfree(seek_val);
        for (auto &v : sorted_values) sdsfree(v);
        fbtreeFree(tree);
    }
}

/* Shared-prefix discrimination */
TEST_F(FbtreeTest, SeekToValuePropertySharedPrefixDiscrimination) {
    fbtreeFree(fbt);
    fbt = nullptr;

    const int NUM_ITERATIONS = 150;
    unsigned int seed = 777;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        fbtreeIndex *tree = fbtreeCreate();

        char prefix[9];
        for (int j = 0; j < 8; j++) {
            prefix[j] = 'A' + (char)(rand_r(&seed) % 26);
        }
        prefix[8] = '\0';

        int num_elements = 10 + (int)(rand_r(&seed) % 491);

        std::vector<sds> sorted_values;
        sorted_values.reserve(num_elements);

        for (int i = 0; i < num_elements; i++) {
            int suffix_len = 1 + (int)(rand_r(&seed) % 12);
            sds s = sdsnewlen(NULL, 8 + suffix_len + 1);
            memcpy(s, prefix, 8);
            for (int k = 0; k < suffix_len; k++) {
                s[8 + k] = 'a' + (char)(rand_r(&seed) % 26);
            }
            s[8 + suffix_len] = '\0';

            fbtreeInsert(tree, s);
            sorted_values.push_back(sdsnewlen(s, sdslen(s)));
        }

        std::sort(sorted_values.begin(), sorted_values.end(), [](const sds a, const sds b) {
            return sdscmp(a, b) < 0;
        });

        int seek_suffix_len = 1 + (int)(rand_r(&seed) % 12);
        sds seek_val = sdsnewlen(NULL, 8 + seek_suffix_len + 1);
        memcpy(seek_val, prefix, 8);
        for (int k = 0; k < seek_suffix_len; k++) {
            seek_val[8 + k] = 'a' + (char)(rand_r(&seed) % 26);
        }
        seek_val[8 + seek_suffix_len] = '\0';

        sds expected = NULL;
        for (size_t i = 0; i < sorted_values.size(); i++) {
            if (sdscmp(sorted_values[i], seek_val) >= 0) {
                expected = sorted_values[i];
                break;
            }
        }

        fbtreeIterator it;
        fbtreeInitIterator(&it, tree);
        fbtreeSeekToValue(seek_val, &it);

        const_sds pos;
        bool got_next = ((pos = fbtreeNext(&it)) != nullptr);

        if (expected == NULL) {
            ASSERT_FALSE(got_next) << "Iteration " << iter << ": expected no element >= seek value, but got one"
                                   << " (prefix=" << prefix << ", num_elements=" << num_elements << ")";
        } else {
            ASSERT_TRUE(got_next) << "Iteration " << iter << ": expected element >= seek value, but got none"
                                  << " (prefix=" << prefix << ", seek_val=" << seek_val
                                  << ", num_elements=" << num_elements << ")";
            ASSERT_EQ(sdscmp(pos, expected), 0)
                << "Iteration " << iter << ": wrong element returned - full value comparison not used"
                << " (prefix=" << prefix << ", seek_val=" << seek_val << ", got=" << pos
                << ", expected=" << expected << ", num_elements=" << num_elements << ")";

            fbtreeInitIterator(&it, tree);
            fbtreeSeekToValue(seek_val, &it);
            const_sds prev_pos;
            bool got_prev = ((prev_pos = fbtreePrev(&it)) != nullptr);
            if (got_prev) {
                ASSERT_LT(sdscmp(prev_pos, seek_val), 0)
                    << "Iteration " << iter << ": element before seek position is not < seek value"
                    << " (prefix=" << prefix << ", prev=" << prev_pos << ", seek_val=" << seek_val << ")";
            }
        }

        sdsfree(seek_val);
        for (auto &v : sorted_values) sdsfree(v);
        fbtreeFree(tree);
    }
}

/* ========== fbtreeDeleteRangeByRank Tests ========== */

TEST_F(FbtreeTest, DeleteRangeByRankEmpty) {
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 0, 0, NULL, NULL), 0u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByRankSingle) {
    insert("a");
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 0, 0, NULL, NULL), 1u);
    EXPECT_EQ(fbtreeLength(fbt), 0u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByRankAll) {
    for (int i = 0; i < 10; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 0, 9, NULL, NULL), 10u);
    EXPECT_EQ(fbtreeLength(fbt), 0u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByRankMiddle) {
    for (int i = 0; i < 10; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    /* Delete ranks 3..6 (4 elements: AAD, AAE, AAF, AAG) */
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 3, 6, NULL, NULL), 4u);
    EXPECT_EQ(fbtreeLength(fbt), 6u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 6u);
    /* Element just before deleted range (was rank 2) should survive */
    EXPECT_EQ(remaining[2], std::string("AAC\0", 4));
    /* Element just after deleted range (was rank 7) should now be at rank 3 */
    EXPECT_EQ(remaining[3], std::string("AAH\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByRankFirst) {
    for (int i = 0; i < 10; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 0, 2, NULL, NULL), 3u);
    EXPECT_EQ(fbtreeLength(fbt), 7u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 7u);
    /* First surviving element should be what was rank 3 (AAD) */
    EXPECT_EQ(remaining[0], std::string("AAD\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByRankLast) {
    for (int i = 0; i < 10; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 7, 9, NULL, NULL), 3u);
    EXPECT_EQ(fbtreeLength(fbt), 7u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 7u);
    /* Last surviving element should be what was rank 6 (AAG) */
    EXPECT_EQ(remaining[6], std::string("AAG\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByRankOutOfBounds) {
    for (int i = 0; i < 5; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    /* end_rank beyond length - should clamp */
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 3, 100, NULL, NULL), 2u);
    EXPECT_EQ(fbtreeLength(fbt), 3u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 3u);
    /* Last surviving element should be what was rank 2 (AAC) */
    EXPECT_EQ(remaining[2], std::string("AAC\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByRankStartBeyondLength) {
    for (int i = 0; i < 5; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 10, 20, NULL, NULL), 0u);
    EXPECT_EQ(fbtreeLength(fbt), 5u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByRankInvertedRange) {
    for (int i = 0; i < 5; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    /* start > end should delete nothing */
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 3, 1, NULL, NULL), 0u);
    EXPECT_EQ(fbtreeLength(fbt), 5u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByRankMultilevel) {
    /* Build a tree with enough elements to have multiple inner node levels */
    const int N = NODE_SIZE * 4;
    for (int i = 0; i < N; i++) {
        fbtreeInsert(fbt, createBase26TestString("key_", "", i, 5));
    }
    expectValid();

    /* Delete a range spanning multiple leaves */
    unsigned long start = NODE_SIZE / 2;
    unsigned long end = NODE_SIZE * 2 + NODE_SIZE / 2;
    unsigned long expected = end - start + 1;
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, start, end, NULL, NULL), expected);
    EXPECT_EQ(fbtreeLength(fbt), (unsigned long)(N - expected));
    expectValid();

    /* Verify iteration still works */
    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), (size_t)(N - expected));
    auto backward = collectBackward();
    EXPECT_EQ(backward.size(), remaining.size());

    /* Verify boundary elements survived */
    sds expected_before = createBase26TestString("key_", "", start - 1, 5);
    sds expected_after = createBase26TestString("key_", "", end + 1, 5);
    EXPECT_EQ(remaining[start - 1], std::string(expected_before, sdslen(expected_before)));
    EXPECT_EQ(remaining[start], std::string(expected_after, sdslen(expected_after)));
    sdsfree(expected_before);
    sdsfree(expected_after);
}

TEST_F(FbtreeTest, DeleteRangeByRankThenInsert) {
    for (int i = 0; i < 20; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 5, 14, NULL, NULL), 10u);
    EXPECT_EQ(fbtreeLength(fbt), 10u);
    expectValid();

    /* Insert new elements after range delete */
    for (int i = 100; i < 110; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    EXPECT_EQ(fbtreeLength(fbt), 20u);
    expectValid();

    auto all = collectForward();
    EXPECT_EQ(all.size(), 20u);
}

TEST_F(FbtreeTest, DeleteRangeByRankDeepTree) {
    /* Build a 3+ level tree */
    const int N = TEST_THREE_LEVEL_ITEMS;
    for (int i = 0; i < N; i++) {
        fbtreeInsert(fbt, createBase26TestString("deep_", "", i, 6));
    }
    expectValid();

    /* Delete a large chunk from the middle */
    unsigned long mid = N / 3;
    unsigned long end = 2 * N / 3;
    unsigned long expected = end - mid + 1;
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, mid, end, NULL, NULL), expected);
    EXPECT_EQ(fbtreeLength(fbt), (unsigned long)(N - expected));
    expectValid();
}

/* ========== fbtreeDeleteRangeByScore Tests ========== */

TEST_F(FbtreeTest, DeleteRangeByScoreEmpty) {
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "AAAAAAAA", "ZZZZZZZZ", 0, 0, NULL, NULL), 0u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByScoreAll) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));

    /* Range covers all scores */
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "AAAAAAAA", "CCCCCCCC", 0, 0, NULL, NULL), 3u);
    EXPECT_EQ(fbtreeLength(fbt), 0u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByScoreMiddle) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));
    fbtreeInsert(fbt, createString("DDDDDDDDelem_3"));
    fbtreeInsert(fbt, createString("EEEEEEEEelem_4"));

    /* Delete B and C scores */
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "BBBBBBBB", "CCCCCCCC", 0, 0, NULL, NULL), 2u);
    EXPECT_EQ(fbtreeLength(fbt), 3u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 3u);
    EXPECT_EQ(memcmp(remaining[0].data(), "AAAAAAAA", 8), 0);
    EXPECT_EQ(memcmp(remaining[1].data(), "DDDDDDDD", 8), 0);
    EXPECT_EQ(memcmp(remaining[2].data(), "EEEEEEEE", 8), 0);
}

TEST_F(FbtreeTest, DeleteRangeByScoreExclusiveMin) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));

    /* Exclusive min: should skip B, only delete C */
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "BBBBBBBB", "CCCCCCCC", 1, 0, NULL, NULL), 1u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(memcmp(remaining[0].data(), "AAAAAAAA", 8), 0);
    EXPECT_EQ(memcmp(remaining[1].data(), "BBBBBBBB", 8), 0);
}

TEST_F(FbtreeTest, DeleteRangeByScoreExclusiveMax) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));

    /* Exclusive max: should skip C, only delete B */
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "BBBBBBBB", "CCCCCCCC", 0, 1, NULL, NULL), 1u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(memcmp(remaining[0].data(), "AAAAAAAA", 8), 0);
    EXPECT_EQ(memcmp(remaining[1].data(), "CCCCCCCC", 8), 0);
}

TEST_F(FbtreeTest, DeleteRangeByScoreBothExclusive) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));
    fbtreeInsert(fbt, createString("DDDDDDDDelem_3"));

    /* Both exclusive on B..D: should only delete C */
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "BBBBBBBB", "DDDDDDDD", 1, 1, NULL, NULL), 1u);
    EXPECT_EQ(fbtreeLength(fbt), 3u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 3u);
    EXPECT_EQ(memcmp(remaining[0].data(), "AAAAAAAA", 8), 0);
    EXPECT_EQ(memcmp(remaining[1].data(), "BBBBBBBB", 8), 0);
    EXPECT_EQ(memcmp(remaining[2].data(), "DDDDDDDD", 8), 0);
}

TEST_F(FbtreeTest, DeleteRangeByScoreNoMatch) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("DDDDDDDDelem_1"));

    /* Range between existing scores - nothing to delete */
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "BBBBBBBB", "CCCCCCCC", 0, 0, NULL, NULL), 0u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByScorePastEnd) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));

    /* Range entirely beyond all elements */
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "YYYYYYYY", "ZZZZZZZZ", 0, 0, NULL, NULL), 0u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByScoreBeforeStart) {
    fbtreeInsert(fbt, createString("MMMMMMMMelem_0"));
    fbtreeInsert(fbt, createString("NNNNNNNNelem_1"));

    /* Range entirely before all elements */
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "AAAAAAAA", "BBBBBBBB", 0, 0, NULL, NULL), 0u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByScoreDuplicateScores) {
    /* Multiple elements with same score prefix but different suffixes */
    fbtreeInsert(fbt, createString("BBBBBBBBaaa"));
    fbtreeInsert(fbt, createString("BBBBBBBBbbb"));
    fbtreeInsert(fbt, createString("BBBBBBBBccc"));
    fbtreeInsert(fbt, createString("CCCCCCCCddd"));

    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "BBBBBBBB", "BBBBBBBB", 0, 0, NULL, NULL), 3u);
    EXPECT_EQ(fbtreeLength(fbt), 1u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(memcmp(remaining[0].data(), "CCCCCCCC", 8), 0);
}

TEST_F(FbtreeTest, DeleteRangeByScoreMultilevel) {
    /* Build a multilevel tree with score-prefixed elements */
    const int N = NODE_SIZE * 4;
    for (int i = 0; i < N; i++) {
        /* Create 8-byte score prefix from index, then element suffix */
        char score[9];
        snprintf(score, sizeof(score), "%08d", i);
        sds s = sdsnewlen(NULL, 8 + 6 + 1);
        memcpy(s, score, 8);
        snprintf(s + 8, 7, "elem%c", '\0');
        s[8 + 6] = '\0';
        fbtreeInsert(fbt, s);
    }
    expectValid();

    /* Delete a range in the middle */
    char min_score[9], max_score[9];
    snprintf(min_score, sizeof(min_score), "%08d", N / 4);
    snprintf(max_score, sizeof(max_score), "%08d", 3 * N / 4);

    unsigned long before = fbtreeLength(fbt);
    unsigned long deleted = fbtreeDeleteRangeByScore(fbt, min_score, max_score, 0, 0, NULL, NULL);
    EXPECT_GT(deleted, 0u);
    EXPECT_EQ(fbtreeLength(fbt), before - deleted);
    expectValid();

    /* Verify iteration still works correctly */
    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), (size_t)(before - deleted));
}

TEST_F(FbtreeTest, DeleteRangeByScoreThenInsert) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));
    fbtreeInsert(fbt, createString("DDDDDDDDelem_3"));

    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "BBBBBBBB", "CCCCCCCC", 0, 0, NULL, NULL), 2u);
    expectValid();

    /* Insert into the gap */
    fbtreeInsert(fbt, createString("BBBBBBBBnew_elem"));
    EXPECT_EQ(fbtreeLength(fbt), 3u);
    expectValid();

    auto all = collectForward();
    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(memcmp(all[0].data(), "AAAAAAAA", 8), 0);
    EXPECT_EQ(memcmp(all[1].data(), "BBBBBBBB", 8), 0);
    EXPECT_EQ(memcmp(all[2].data(), "DDDDDDDD", 8), 0);
}

/* ========== fbtreeDeleteRangeByValue Tests ========== */

TEST_F(FbtreeTest, DeleteRangeByValueEmpty) {
    sds min_val = createString("aaa");
    sds max_val = createString("zzz");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 0, 0, NULL, NULL), 0u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByValueAll) {
    insert("bbb");
    insert("ccc");
    insert("ddd");

    sds min_val = createString("aaa");
    sds max_val = createString("eee");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 0, 0, NULL, NULL), 3u);
    EXPECT_EQ(fbtreeLength(fbt), 0u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByValueMiddle) {
    insert("aaa");
    insert("bbb");
    insert("ccc");
    insert("ddd");
    insert("eee");

    sds min_val = createString("bbb");
    sds max_val = createString("ddd");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 0, 0, NULL, NULL), 3u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 2u);
    EXPECT_EQ(remaining[0], std::string("aaa\0", 4));
    EXPECT_EQ(remaining[1], std::string("eee\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByValueExclusiveMin) {
    insert("aaa");
    insert("bbb");
    insert("ccc");

    sds min_val = createString("aaa");
    sds max_val = createString("ccc");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 1, 0, NULL, NULL), 2u);
    EXPECT_EQ(fbtreeLength(fbt), 1u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining[0], std::string("aaa\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByValueExclusiveMax) {
    insert("aaa");
    insert("bbb");
    insert("ccc");

    sds min_val = createString("aaa");
    sds max_val = createString("ccc");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 0, 1, NULL, NULL), 2u);
    EXPECT_EQ(fbtreeLength(fbt), 1u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining[0], std::string("ccc\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByValueBothExclusive) {
    insert("aaa");
    insert("bbb");
    insert("ccc");
    insert("ddd");

    sds min_val = createString("aaa");
    sds max_val = createString("ddd");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 1, 1, NULL, NULL), 2u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 2u);
    EXPECT_EQ(remaining[0], std::string("aaa\0", 4));
    EXPECT_EQ(remaining[1], std::string("ddd\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByValueNoMatch) {
    insert("aaa");
    insert("ddd");

    sds min_val = createString("bbb");
    sds max_val = createString("ccc");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 0, 0, NULL, NULL), 0u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByValueExactMatch) {
    insert("aaa");
    insert("bbb");
    insert("ccc");

    /* min == max, inclusive: delete exactly one element */
    sds val = createString("bbb");
    sds val2 = createString("bbb");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, val, val2, 0, 0, NULL, NULL), 1u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    sdsfree(val);
    sdsfree(val2);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 2u);
    EXPECT_EQ(remaining[0], std::string("aaa\0", 4));
    EXPECT_EQ(remaining[1], std::string("ccc\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByValueExactMatchExclusive) {
    insert("aaa");
    insert("bbb");
    insert("ccc");

    /* min == max, both exclusive: delete nothing */
    sds val = createString("bbb");
    sds val2 = createString("bbb");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, val, val2, 1, 1, NULL, NULL), 0u);
    EXPECT_EQ(fbtreeLength(fbt), 3u);
    sdsfree(val);
    sdsfree(val2);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 3u);
    EXPECT_EQ(remaining[0], std::string("aaa\0", 4));
    EXPECT_EQ(remaining[1], std::string("bbb\0", 4));
    EXPECT_EQ(remaining[2], std::string("ccc\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByValueMultilevel) {
    const int N = NODE_SIZE * 4;
    for (int i = 0; i < N; i++) {
        fbtreeInsert(fbt, createBase26TestString("val_", "", i, 5));
    }
    expectValid();

    /* Delete a range in the middle using value comparison */
    sds min_val = createBase26TestString("val_", "", N / 4, 5);
    sds max_val = createBase26TestString("val_", "", 3 * N / 4, 5);

    unsigned long before = fbtreeLength(fbt);
    unsigned long deleted = fbtreeDeleteRangeByValue(fbt, min_val, max_val, 0, 0, NULL, NULL);
    EXPECT_GT(deleted, 0u);
    EXPECT_EQ(fbtreeLength(fbt), before - deleted);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), (size_t)(before - deleted));
}

TEST_F(FbtreeTest, DeleteRangeByValueWithScorePrefix) {
    /* Simulate the adapter pattern: [8-byte score][element] */
    fbtreeInsert(fbt, createString("SCOREAAAbbb"));
    fbtreeInsert(fbt, createString("SCOREAAAccc"));
    fbtreeInsert(fbt, createString("SCOREAAAddd"));
    fbtreeInsert(fbt, createString("SCOREAAAeee"));
    fbtreeInsert(fbt, createString("SCOREAAAfff"));

    /* Delete lex range [ccc, eee] within the same score prefix */
    sds min_val = createString("SCOREAAAccc");
    sds max_val = createString("SCOREAAAeee");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 0, 0, NULL, NULL), 3u);
    EXPECT_EQ(fbtreeLength(fbt), 2u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 2u);
    EXPECT_EQ(remaining[0], std::string("SCOREAAAbbb\0", 12));
    EXPECT_EQ(remaining[1], std::string("SCOREAAAfff\0", 12));
}

TEST_F(FbtreeTest, DeleteRangeByValueThenInsert) {
    insert("aaa");
    insert("bbb");
    insert("ccc");
    insert("ddd");

    sds min_val = createString("bbb");
    sds max_val = createString("ccc");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 0, 0, NULL, NULL), 2u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();

    insert("bbb");
    EXPECT_EQ(fbtreeLength(fbt), 3u);
    expectValid();

    auto all = collectForward();
    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0], std::string("aaa\0", 4));
    EXPECT_EQ(all[1], std::string("bbb\0", 4));
    EXPECT_EQ(all[2], std::string("ddd\0", 4));
}


TEST_F(FbtreeTest, DeleteRangeByRankSingleMiddle) {
    for (int i = 0; i < 5; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    /* Delete exactly one element at rank 2 */
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, 2, 2, NULL, NULL), 1u);
    EXPECT_EQ(fbtreeLength(fbt), 4u);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 4u);
    EXPECT_EQ(remaining[0], std::string("AAA\0", 4));
    EXPECT_EQ(remaining[1], std::string("AAB\0", 4));
    EXPECT_EQ(remaining[2], std::string("AAD\0", 4));
    EXPECT_EQ(remaining[3], std::string("AAE\0", 4));
}

TEST_F(FbtreeTest, DeleteRangeByScoreAdjacentExclusive) {
    fbtreeInsert(fbt, createString("AAAAAAAAelem_0"));
    fbtreeInsert(fbt, createString("BBBBBBBBelem_1"));
    fbtreeInsert(fbt, createString("CCCCCCCCelem_2"));

    /* Both exclusive on adjacent scores B..C: nothing between them */
    EXPECT_EQ(fbtreeDeleteRangeByScore(fbt, "BBBBBBBB", "CCCCCCCC", 1, 1, NULL, NULL), 0u);
    EXPECT_EQ(fbtreeLength(fbt), 3u);
    expectValid();
}

TEST_F(FbtreeTest, DeleteRangeByValueAdjacentExclusive) {
    insert("aaa");
    insert("bbb");
    insert("ccc");

    /* Both exclusive on adjacent values: nothing between bbb and ccc */
    sds min_val = createString("bbb");
    sds max_val = createString("ccc");
    EXPECT_EQ(fbtreeDeleteRangeByValue(fbt, min_val, max_val, 1, 1, NULL, NULL), 0u);
    EXPECT_EQ(fbtreeLength(fbt), 3u);
    sdsfree(min_val);
    sdsfree(max_val);
    expectValid();

    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), 3u);
    EXPECT_EQ(remaining[0], std::string("aaa\0", 4));
    EXPECT_EQ(remaining[1], std::string("bbb\0", 4));
    EXPECT_EQ(remaining[2], std::string("ccc\0", 4));
}


TEST_F(FbtreeTest, DeleteRangeByRankSweep) {
    /* Sweep many start/end combinations on a multilevel tree to exercise
     * every possible split point in the optimized range deletion. */
    const int N = NODE_SIZE * 4;
    unsigned long step = NODE_SIZE / 2;

    /* Build reference set once */
    std::vector<std::string> all_elements;
    for (int i = 0; i < N; i++) {
        sds s = createBase26TestString("k_", "", i, 5);
        all_elements.emplace_back(s, sdslen(s));
        sdsfree(s);
    }

    for (unsigned long start = 0; start < (unsigned long)N; start += step) {
        for (unsigned long end = start; end < (unsigned long)N; end += step) {
            fbtreeIndex *tree = fbtreeCreate();
            for (int i = 0; i < N; i++) {
                fbtreeInsert(tree, createBase26TestString("k_", "", i, 5));
            }

            unsigned long clamped_end = end >= (unsigned long)N ? (unsigned long)(N - 1) : end;
            unsigned long expected = clamped_end - start + 1;

            unsigned long deleted = fbtreeDeleteRangeByRank(tree, start, end, NULL, NULL);
            unsigned long remaining = fbtreeLength(tree);

            EXPECT_EQ(deleted, expected)
                << "start=" << start << " end=" << end;
            EXPECT_EQ(remaining, (unsigned long)N - deleted)
                << "start=" << start << " end=" << end;
            EXPECT_TRUE(fbtreeDebugValidate(tree, false, NULL, 0))
                << "Validation failed: start=" << start << " end=" << end;

            /* Verify surviving elements match expected */
            fbtreeIterator it;
            fbtreeInitIterator(&it, tree);
            const_sds pos;
            size_t idx = 0;
            while ((pos = fbtreeNext(&it)) != nullptr) {
                /* Find the next expected surviving element */
                while (idx >= start && idx <= clamped_end) idx++;
                ASSERT_LT(idx, (size_t)N)
                    << "Too many elements after delete: start=" << start << " end=" << end;
                EXPECT_EQ(std::string(pos, sdslen(pos)), all_elements[idx])
                    << "Wrong element at position: start=" << start << " end=" << end << " idx=" << idx;
                idx++;
            }

            fbtreeFree(tree);
        }
    }
}

/* ========== Edge Range Deletion Tests (start/end of tree, multilevel) ========== */

/* Delete a large range from the start of a multilevel tree. Ensures inner
 * nodes are removed, leftmost_leaf cache is updated, and merge enforcement
 * holds after deleting from the left edge. */


TEST_F(FbtreeTest, DeleteRangeCallbackNull) {
    for (int i = 0; i < 20; i++) {
        fbtreeInsert(fbt, createBase26TestString("", "", i, 3));
    }
    expectValid();

    unsigned long deleted = fbtreeDeleteRangeByRank(fbt, 5, 14, NULL, NULL);
    EXPECT_EQ(deleted, 10u);
    EXPECT_EQ(fbtreeLength(fbt), 10u);
    expectValid();
}

/* ========== Node Merge Unit Tests ========== */

/* Insert enough items to create 2 leaves, then delete from one leaf until
 * underflow triggers a merge back to a single leaf. */


/* Build a 3-level tree, delete enough items to cause inner node underflow
 * and merge. */
TEST_F(FbtreeTest, NodeMergeInner) {
    /* Build a 3-level tree */
    const int count = TEST_THREE_LEVEL_ITEMS;
    char buf[16];
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%05d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();
    EXPECT_EQ(fbtreeLength(fbt), (size_t)count);

    /* Delete a large portion of items from the beginning. This will cause
     * multiple leaf merges, which in turn remove children from inner nodes,
     * eventually causing inner node underflow and merge. */
    int to_delete = count * 3 / 4;
    for (int i = 0; i < to_delete; i++) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    int remaining = count - to_delete;
    EXPECT_EQ(fbtreeLength(fbt), (size_t)remaining);
    expectValid();

    /* Verify iteration produces correct sorted order */
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = to_delete; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%05d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    zfree(inserted);
}

/* Build a tree where deletes trigger merges at multiple levels as the
 * recursion unwinds. */
TEST_F(FbtreeTest, NodeMergeCascading) {
    /* Build a 3-level tree */
    const int count = TEST_THREE_LEVEL_ITEMS;
    char buf[16];
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "cas_%05d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    /* Delete most items, leaving very few. This forces cascading merges:
     * leaf merges → inner node child removal → inner node underflow → inner merge.
     * Delete all but ~20 items spread across the range. */
    for (int i = 0; i < count; i++) {
        /* Keep every (count/20)th item */
        if (i % (count / 20) == 0) continue;
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    expectValid();

    /* Verify remaining items are correct */
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    const_sds prev_pos = nullptr;
    int iter_count = 0;
    while ((pos = fbtreeNext(&it)) != nullptr) {
        if (prev_pos) {
            EXPECT_LT(sdscmp(prev_pos, pos), 0) << "Sorted order violated after cascading merge";
        }
        prev_pos = pos;
        iter_count++;
    }
    EXPECT_EQ((size_t)iter_count, fbtreeLength(fbt));

    zfree(inserted);
}

/* Build a tree where the underflowed node's sibling has NODE_SIZE items,
 * so merge is impossible. */


/* Verify that after merging the leftmost or rightmost leaf, the cache
 * pointers are correct. */
TEST_F(FbtreeTest, NodeMergeLeafCacheUpdate) {
    const int count = NODE_SIZE * 3;
    char buf[16];
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "cache_%04d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    /* Part 1: Delete from leftmost leaf until merge, verify leftmost_leaf cache
     * is correct via forward iteration. */
    for (int i = 0; i < NODE_SIZE; i++) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    expectValid();

    /* Forward iteration should start from the correct leftmost leaf */
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    snprintf(buf, sizeof(buf), "cache_%04d", NODE_SIZE);
    EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0)
        << "leftmost_leaf cache incorrect after merge";

    /* Part 2: Delete from rightmost leaf until merge, verify rightmost_leaf cache
     * is correct via backward iteration. */
    for (int i = count - 1; i >= count - NODE_SIZE; i--) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    expectValid();

    /* Backward iteration should start from the correct rightmost leaf */
    fbtreeInitIterator(&it, fbt);
    ASSERT_NE(pos = fbtreePrev(&it), nullptr);
    snprintf(buf, sizeof(buf), "cache_%04d", count - NODE_SIZE - 1);
    EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0)
        << "rightmost_leaf cache incorrect after merge";

    zfree(inserted);
}

/* Validation failures report which check failed and where. Corrupt an inner
 * node's stored subtree size, expect a detailed message, then restore. */
TEST_F(FbtreeTest, DebugValidateReportsFailureDetail) {
    char buf[16];
    const int count = NODE_SIZE * 2;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    ASSERT_FALSE(fbt->root->is_leaf);

    char errmsg[256];
    ASSERT_TRUE(fbtreeDebugValidate(fbt, false, errmsg, sizeof(errmsg)));
    EXPECT_STREQ(errmsg, "");

    innerNode *root = (innerNode *)(void *)fbt->root;
    root->child_sizes[0] += 1;
    ASSERT_FALSE(fbtreeDebugValidate(fbt, false, errmsg, sizeof(errmsg)));
    EXPECT_NE(strstr(errmsg, "stored subtree size"), nullptr) << "errmsg: " << errmsg;

    root->child_sizes[0] -= 1;
    ASSERT_TRUE(fbtreeDebugValidate(fbt, false, errmsg, sizeof(errmsg)));
    EXPECT_STREQ(errmsg, "");
}

/* Range delete that leaves boundary nodes underflowed, verify merges
 * happen via fbtreeDebugValidate. */
TEST_F(FbtreeTest, NodeMergeRangeDelete) {
    const int count = NODE_SIZE * 4;
    for (int i = 0; i < count; i++) {
        fbtreeInsert(fbt, createBase26TestString("rng_", "", i, 5));
    }
    expectValid();

    /* Delete a range from the middle that spans multiple leaves.
     * The boundary leaves (partially deleted) may underflow and trigger merges. */
    unsigned long start = NODE_SIZE + 5;
    unsigned long end = NODE_SIZE * 3 - 5;
    unsigned long expected_deleted = end - start + 1;
    EXPECT_EQ(fbtreeDeleteRangeByRank(fbt, start, end, NULL, NULL), expected_deleted);
    EXPECT_EQ(fbtreeLength(fbt), (unsigned long)(count - expected_deleted));
    expectValid();

    /* Verify sorted iteration still works */
    auto remaining = collectForward();
    EXPECT_EQ(remaining.size(), (size_t)(count - expected_deleted));

    /* Verify backward iteration matches */
    auto backward = collectBackward();
    EXPECT_EQ(backward.size(), remaining.size());
}

/* Merge reduces root to single child, verify root collapses correctly. */
TEST_F(FbtreeTest, NodeMergeRootCollapse) {
    /* Insert just enough to create a 2-level tree (root inner + 2 leaves).
     * Then delete until one leaf is empty/merged, leaving root with 1 child.
     * Root should collapse to that single child (a leaf). */
    const int count = NODE_SIZE + 1;
    char buf[16];
    sds *inserted = (sds *)zmalloc(count * sizeof(sds));
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "root_%03d", i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    /* Delete all but a handful of items. After merges, the root should collapse
     * from an inner node to a leaf node. */
    int keep = 5;
    for (int i = 0; i < count - keep; i++) {
        EXPECT_TRUE(fbtreeDelete(fbt, inserted[i]));
    }
    EXPECT_EQ(fbtreeLength(fbt), (size_t)keep);
    expectValid();

    /* Verify the remaining items are correct */
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    for (int i = count - keep; i < count; i++) {
        snprintf(buf, sizeof(buf), "root_%03d", i);
        ASSERT_NE(pos = fbtreeNext(&it), nullptr);
        EXPECT_EQ(memcmp(pos, buf, strlen(buf) + 1), 0);
    }
    EXPECT_EQ(pos = fbtreeNext(&it), nullptr);

    zfree(inserted);
}

/* Pop operations that trigger leaf merges, verify tree invariants. */


/* ========== Property-Based Tests for Node Merging ========== */

/* Helper: generate a unique key using a monotonic counter for deterministic uniqueness. */
static sds generateUniqueKey(const char *prefix, int counter, unsigned int *seed) {
    char buf[32];
    (void)seed; /* seed available for future use */
    snprintf(buf, sizeof(buf), "%s%08d", prefix, counter);
    return createString(buf);
}

/* child_num_items consistency: for any FBTree built by any sequence of inserts
 * and deletes, and for any inner node in that tree,
 * child_num_items[i] == children[i]->num_items. */
TEST_F(FbtreeTest, PropertyChildNumItemsConsistency) {
    fbtreeFree(fbt);
    fbt = nullptr;

    const int NUM_ITERATIONS = 150;
    unsigned int seed = 100;
    int key_counter = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        fbtreeIndex *tree = fbtreeCreate();

        /* Vary tree size: single-leaf, 2-level, 3-level */
        int target_size;
        if (iter < 50) {
            target_size = 1 + (rand_r(&seed) % NODE_SIZE);
        } else if (iter < 100) {
            target_size = NODE_SIZE + 1 + (rand_r(&seed) % 300);
        } else {
            target_size = 400 + (rand_r(&seed) % 1600);
        }

        /* Insert phase */
        for (int i = 0; i < target_size; i++) {
            sds s = generateUniqueKey("c1_", key_counter++, &seed);
            fbtreeInsert(tree, s);
        }

        /* Delete phase: delete a random subset using rank-based lookup */
        int num_deletes = (int)(rand_r(&seed) % (target_size + 1));
        for (int d = 0; d < num_deletes && fbtreeLength(tree) > 0; d++) {
            unsigned long len = fbtreeLength(tree);
            unsigned long rank = rand_r(&seed) % len;
            const_sds item = fbtreeGetAtRank(tree, rank);
            ASSERT_NE(item, nullptr);
            fbtreeDelete(tree, item);
        }

        /* Validate child_num_items consistency after all operations */
        if (fbtreeLength(tree) > 0) {
            ASSERT_TRUE(fbtreeDebugValidate(tree, false, NULL, 0))
                << "Validation failed after deletes, iter=" << iter;
        }

        fbtreeFree(tree);
    }
}

/* Sorted order preserved after merges: for any FBTree and any sequence of
 * insert and delete operations, iterating forward produces non-decreasing
 * order, backward produces non-increasing order. */
TEST_F(FbtreeTest, PropertySortedOrderAfterMerges) {
    fbtreeFree(fbt);
    fbt = nullptr;

    const int NUM_ITERATIONS = 150;
    unsigned int seed = 200;
    int key_counter = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        fbtreeIndex *tree = fbtreeCreate();

        /* Vary tree size to trigger merges */
        int target_size;
        if (iter < 50) {
            target_size = 10 + (rand_r(&seed) % 60);
        } else if (iter < 100) {
            target_size = NODE_SIZE + 1 + (rand_r(&seed) % 300);
        } else {
            target_size = 400 + (rand_r(&seed) % 4600);
        }

        /* Insert phase */
        for (int i = 0; i < target_size; i++) {
            sds s = generateUniqueKey("c2_", key_counter++, &seed);
            fbtreeInsert(tree, s);
        }

        /* Delete enough to trigger merges using rank-based lookup */
        int num_deletes = target_size / 2 + (rand_r(&seed) % (target_size / 2 + 1));
        for (int d = 0; d < num_deletes && fbtreeLength(tree) > 0; d++) {
            unsigned long len = fbtreeLength(tree);
            unsigned long rank = rand_r(&seed) % len;
            const_sds item = fbtreeGetAtRank(tree, rank);
            if (item) fbtreeDelete(tree, item);
        }

        /* Verify forward iteration is non-decreasing */
        {
            fbtreeIterator it;
            fbtreeInitIterator(&it, tree);
            const_sds pos;
            const_sds prev = nullptr;
            int count = 0;
            while ((pos = fbtreeNext(&it)) != nullptr) {
                if (prev) {
                    ASSERT_LE(sdscmp(prev, pos), 0)
                        << "Forward order violated at iter=" << iter << " count=" << count;
                }
                prev = pos;
                count++;
            }
            ASSERT_EQ((size_t)count, fbtreeLength(tree))
                << "Forward count mismatch at iter=" << iter;
        }

        /* Verify backward iteration is non-increasing */
        {
            fbtreeIterator it;
            fbtreeInitIterator(&it, tree);
            const_sds pos;
            const_sds prev = nullptr;
            int count = 0;
            while ((pos = fbtreePrev(&it)) != nullptr) {
                if (prev) {
                    ASSERT_GE(sdscmp(prev, pos), 0)
                        << "Backward order violated at iter=" << iter << " count=" << count;
                }
                prev = pos;
                count++;
            }
            ASSERT_EQ((size_t)count, fbtreeLength(tree))
                << "Backward count mismatch at iter=" << iter;
        }

        fbtreeFree(tree);
    }
}

/* Tree invariants hold after any operation: for any FBTree and any sequence
 * of operations (single delete, pop min/max, range delete),
 * fbtreeDebugValidate returns true. */
TEST_F(FbtreeTest, PropertyTreeInvariantsAfterAnyOperation) {
    fbtreeFree(fbt);
    fbt = nullptr;

    const int NUM_ITERATIONS = 150;
    unsigned int seed = 300;
    int key_counter = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        fbtreeIndex *tree = fbtreeCreate();

        /* Vary tree size */
        int target_size;
        if (iter < 50) {
            target_size = 5 + (rand_r(&seed) % 60);
        } else if (iter < 100) {
            target_size = NODE_SIZE + 1 + (rand_r(&seed) % 300);
        } else {
            target_size = 400 + (rand_r(&seed) % 1600);
        }

        /* Insert phase */
        for (int i = 0; i < target_size; i++) {
            sds s = generateUniqueKey("c3_", key_counter++, &seed);
            fbtreeInsert(tree, s);
        }
        ASSERT_TRUE(fbtreeDebugValidate(tree, false, NULL, 0))
            << "Validation failed after inserts, iter=" << iter;

        /* Mixed operation phase: use rank-based operations to avoid pointer tracking */
        int num_ops = target_size / 2 + (rand_r(&seed) % (target_size / 2 + 1));
        for (int op = 0; op < num_ops && fbtreeLength(tree) > 0; op++) {
            int op_type = rand_r(&seed) % 4;
            unsigned long len = fbtreeLength(tree);

            if (op_type == 0 && len > 0) {
                /* Delete by rank: get item at random rank, then delete it */
                unsigned long rank = rand_r(&seed) % len;
                const_sds item = fbtreeGetAtRank(tree, rank);
                if (item) fbtreeDelete(tree, item);
            } else if (op_type == 1 && len > 0) {
                /* Pop min */
                sds popped = fbtreePopMin(tree);
                if (popped) sdsfree(popped);
            } else if (op_type == 2 && len > 0) {
                /* Pop max */
                sds popped = fbtreePopMax(tree);
                if (popped) sdsfree(popped);
            } else if (len >= 2) {
                /* Range delete by rank */
                unsigned long start = rand_r(&seed) % len;
                unsigned long end = start + (rand_r(&seed) % (len - start));
                if (end >= len) end = len - 1;
                fbtreeDeleteRangeByRank(tree, start, end, NULL, NULL);
            }

            ASSERT_TRUE(fbtreeDebugValidate(tree, false, NULL, 0))
                << "Validation failed after op " << op << " type=" << op_type
                << " iter=" << iter;
        }

        fbtreeFree(tree);
    }
}

/* Insert-then-delete-all round trip: for any set of N randomly generated
 * strings, inserting all N then deleting all N results in an empty tree
 * with zero memory leaks. */
TEST_F(FbtreeTest, PropertyInsertDeleteAllRoundTrip) {
    fbtreeFree(fbt);
    fbt = nullptr;

    const int NUM_ITERATIONS = 150;
    unsigned int seed = 400;
    int key_counter = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        size_t mem_before_iter = zmalloc_used_memory();

        int n;
        if (iter < 50) {
            n = 1 + (rand_r(&seed) % 60);
        } else if (iter < 100) {
            n = NODE_SIZE + 1 + (rand_r(&seed) % 300);
        } else {
            n = 400 + (rand_r(&seed) % 4600);
        }

        /* Choose deletion strategy: 0=random delete, 1=pop-min, 2=pop-max */
        int strategy = iter % 3;

        fbtreeIndex *tree = fbtreeCreate();
        std::vector<sds> inserted_items;

        for (int i = 0; i < n; i++) {
            sds s = generateUniqueKey("c4_", key_counter++, &seed);
            sds ins = fbtreeInsert(tree, s);
            inserted_items.push_back(ins);
        }

        if (strategy == 0) {
            /* Delete all in random order by pointer */
            while (!inserted_items.empty()) {
                int idx = rand_r(&seed) % inserted_items.size();
                ASSERT_TRUE(fbtreeDelete(tree, inserted_items[idx]))
                    << "Delete failed at iter=" << iter;
                inserted_items.erase(inserted_items.begin() + idx);
            }
        } else if (strategy == 1) {
            /* Pop min all */
            inserted_items.clear();
            while (fbtreeLength(tree) > 0) {
                sds popped = fbtreePopMin(tree);
                ASSERT_NE(popped, nullptr) << "PopMin returned null, iter=" << iter;
                sdsfree(popped);
            }
        } else {
            /* Pop max all */
            inserted_items.clear();
            while (fbtreeLength(tree) > 0) {
                sds popped = fbtreePopMax(tree);
                ASSERT_NE(popped, nullptr) << "PopMax returned null, iter=" << iter;
                sdsfree(popped);
            }
        }

        ASSERT_EQ(fbtreeLength(tree), 0u) << "Tree not empty after delete-all, iter=" << iter;
        fbtreeFree(tree);
        ASSERT_EQ(zmalloc_used_memory(), mem_before_iter)
            << "Memory leak detected at iter=" << iter << " strategy=" << strategy;
    }
}

/* Merge enforcement — no unnecessarily sparse nodes: for any FBTree after any
 * sequence of inserts and deletes, no non-root node has num_items < MIN_FILL
 * unless all siblings have num_items + node.num_items > NODE_SIZE. */
TEST_F(FbtreeTest, LookupWithParentPrefixExceedingChildPrefix) {
    /* Insert a small number of "59" items, then fill the tree with "60" items.
     * This creates a root whose anchors all share "60" as a prefix, but the
     * leftmost child contains "59" items with a shorter common prefix. */
    char buf[24];

    /* Insert a few items with prefix "59" */
    for (int i = 0; i < 5; i++) {
        snprintf(buf, sizeof(buf), "59_%06d", i);
        fbtreeInsert(fbt, createString(buf));
    }

    /* Insert many items with prefix "60" to force splits and create a
     * multi-level tree where most anchors start with "60" */
    for (int i = 0; i < NODE_SIZE * 4; i++) {
        snprintf(buf, sizeof(buf), "60_%06d", i);
        fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    /* Verify all "59" items are findable via GetRankOfItem (uses validated_len) */
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "59_", 3), 0);

    /* Seek to a "59" value — must route to the correct child despite
     * the parent's prefix being "60" (longer than child's prefix) */
    sds seek_val = createString("59_000000");
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "59_000000", 10), 0);
    sdsfree(seek_val);

    /* Seek to something between "59" and "60" */
    seek_val = createString("59_zzzzzz");
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "60_", 3), 0);
    sdsfree(seek_val);

    /* Verify full iteration order */
    fbtreeInitIterator(&it, fbt);
    const_sds prev = nullptr;
    int count = 0;
    while ((pos = fbtreeNext(&it)) != nullptr) {
        if (prev) {
            EXPECT_LT(sdscmp(prev, pos), 0) << "Order violated at " << count;
        }
        prev = pos;
        count++;
    }
    EXPECT_EQ((size_t)count, fbtreeLength(fbt));
}

/* After bulk deletion, a parent's prefix can exceed a child's prefix.
 * This test constructs that scenario and verifies lookups still work:
 * - Build a tree where all anchors share a long prefix (e.g., "prefix_06...")
 * - But the leftmost child contains items with a shorter common prefix
 *   (e.g., "prefix_05..." through "prefix_06...")
 * - After deleting middle children, the parent's prefix grows
 * - Lookups for keys below the prefix (e.g., "prefix_05...") must still
 *   route to the correct leftmost child via the prefix < comparison */
TEST_F(FbtreeTest, LookupAfterPrefixGrowthFromBulkDelete) {
    /* Build a tree with items spanning prefix_050000 through prefix_069999.
     * The root's children will have anchors like prefix_05XXXX and prefix_06XXXX,
     * giving the root a short prefix ("prefix_0"). After deleting the middle
     * range, the remaining children may all start with "prefix_06", growing
     * the root's prefix to "prefix_06" while the leftmost child still has
     * items starting with "prefix_05". */
    const int N = NODE_SIZE * 6;
    char buf[24];
    sds *inserted = (sds *)zmalloc(N * sizeof(sds));

    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "prefix_%06d", 50000 + i);
        inserted[i] = fbtreeInsert(fbt, createString(buf));
    }
    expectValid();

    /* Delete a range from the middle that removes all "prefix_05XXXX" items
     * except those in the leftmost child. This should cause the parent's
     * prefix to grow past the leftmost child's prefix. */
    unsigned long start = NODE_SIZE;
    unsigned long end = N / 2;
    fbtreeDeleteRangeByRank(fbt, start, end, NULL, NULL);
    expectValid();

    /* Now look up items that are still in the tree — especially the early
     * ones whose prefix diverges from the parent's grown prefix */
    for (unsigned long i = 0; i < start; i++) {
        long rank = fbtreeGetIndexOfItem(fbt, inserted[i]);
        EXPECT_GE(rank, 0) << "Failed to find item at original index " << i;
    }

    /* Look up via SeekToValue for a key below the parent's prefix */
    sds seek_val = createString("prefix_050000");
    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    fbtreeSeekToValue(seek_val, &it);
    const_sds pos;
    ASSERT_NE(pos = fbtreeNext(&it), nullptr);
    EXPECT_EQ(memcmp(pos, "prefix_050000", 14), 0);
    sdsfree(seek_val);

    /* Verify forward iteration produces all remaining elements in order */
    fbtreeInitIterator(&it, fbt);
    const_sds prev = nullptr;
    int count = 0;
    while ((pos = fbtreeNext(&it)) != nullptr) {
        if (prev) {
            EXPECT_LT(sdscmp(prev, pos), 0) << "Sorted order violated at position " << count;
        }
        prev = pos;
        count++;
    }
    EXPECT_EQ((size_t)count, fbtreeLength(fbt));

    zfree(inserted);
}

/* After any range delete, all remaining items must be findable
 * via fbtreeGetIndexOfItem. This catches lookup routing bugs. */
TEST_F(FbtreeTest, PropertyAllItemsFindableAfterRangeDelete) {
    fbtreeFree(fbt);
    fbt = nullptr;

    const int NUM_ITERATIONS = 100;
    unsigned int seed = 9999;
    int key_counter = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        int N;
        if (iter < 30) {
            N = 10 + (rand_r(&seed) % 50);
        } else if (iter < 70) {
            N = NODE_SIZE + 1 + (rand_r(&seed) % 300);
        } else {
            N = 500 + (rand_r(&seed) % 4500);
        }

        fbtreeIndex *tree = fbtreeCreate();
        std::vector<sds> items;
        items.reserve(N);

        for (int i = 0; i < N; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "fi_%08d", key_counter++);
            sds ins = fbtreeInsert(tree, createString(buf));
            items.push_back(ins);
        }

        /* Delete a random range */
        unsigned long start_rank = rand_r(&seed) % N;
        unsigned long end_rank = start_rank + (rand_r(&seed) % (N - start_rank));
        fbtreeDeleteRangeByRank(tree, start_rank, end_rank, NULL, NULL);

        /* Every surviving item must be findable */
        unsigned long remaining = fbtreeLength(tree);
        for (unsigned long r = 0; r < remaining; r++) {
            const_sds at_rank = fbtreeGetAtRank(tree, r);
            ASSERT_NE(at_rank, nullptr) << "iter=" << iter << " rank=" << r;
            long found_rank = fbtreeGetIndexOfItem(tree, at_rank);
            ASSERT_EQ(found_rank, (long)r)
                << "iter=" << iter << " rank=" << r
                << " item found at wrong rank " << found_rank;
        }

        fbtreeFree(tree);
    }
}

/* SeekToValue must find the correct element after range
 * deletion creates parent->prefix_len > child->prefix_len scenarios. */
TEST_F(FbtreeTest, PropertySeekCorrectAfterRangeDelete) {
    fbtreeFree(fbt);
    fbt = nullptr;

    const int NUM_ITERATIONS = 100;
    unsigned int seed = 7777;
    int key_counter = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        int N;
        if (iter < 30) {
            N = 10 + (rand_r(&seed) % 50);
        } else if (iter < 70) {
            N = NODE_SIZE + 1 + (rand_r(&seed) % 300);
        } else {
            N = 500 + (rand_r(&seed) % 4500);
        }

        fbtreeIndex *tree = fbtreeCreate();
        for (int i = 0; i < N; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "sk_%08d", key_counter++);
            fbtreeInsert(tree, createString(buf));
        }

        /* Delete a random range */
        unsigned long start_rank = rand_r(&seed) % N;
        unsigned long end_rank = start_rank + (rand_r(&seed) % (N - start_rank));
        fbtreeDeleteRangeByRank(tree, start_rank, end_rank, NULL, NULL);

        /* Collect remaining elements */
        std::vector<std::string> remaining;
        {
            fbtreeIterator it;
            fbtreeInitIterator(&it, tree);
            const_sds pos;
            while ((pos = fbtreeNext(&it)) != nullptr) {
                remaining.emplace_back(pos, sdslen(pos));
            }
        }

        if (remaining.empty()) {
            fbtreeFree(tree);
            continue;
        }

        /* Seek to each remaining element and verify correct positioning */
        for (size_t i = 0; i < remaining.size(); i += (remaining.size() / 20 > 0 ? remaining.size() / 20 : 1)) {
            sds seek_val = sdsnewlen(remaining[i].data(), remaining[i].size());
            fbtreeIterator it;
            fbtreeInitIterator(&it, tree);
            fbtreeSeekToValue(seek_val, &it);
            const_sds pos;
            ASSERT_NE(pos = fbtreeNext(&it), nullptr)
                << "iter=" << iter << " seek failed for element " << i;
            ASSERT_EQ(sdscmp(pos, seek_val), 0)
                << "iter=" << iter << " seek returned wrong element at " << i;
            sdsfree(seek_val);
        }

        /* Also seek for a value before the first element */
        sds before = createString("A_before_everything");
        {
            fbtreeIterator it;
            fbtreeInitIterator(&it, tree);
            fbtreeSeekToValue(before, &it);
            const_sds pos;
            ASSERT_NE(pos = fbtreeNext(&it), nullptr)
                << "iter=" << iter << " seek before first element failed";
            /* Should return the first remaining element */
            ASSERT_EQ(std::string(pos, sdslen(pos)), remaining[0])
                << "iter=" << iter << " seek before first returned wrong element";
        }
        sdsfree(before);

        fbtreeFree(tree);
    }
}

TEST_F(FbtreeTest, Height) {
    char buf[16];

    /* Empty tree has no root: height 0. */
    EXPECT_EQ(fbtreeHeight(fbt), 0u);

    /* Single element lives in a single leaf root: height 1. */
    insert("k00000");
    EXPECT_EQ(fbtreeHeight(fbt), 1u);

    /* Fill that leaf to capacity (NODE_SIZE items): still a single leaf. */
    for (int i = 1; i < NODE_SIZE; i++) {
        snprintf(buf, sizeof(buf), "k%05d", i);
        insert(buf);
    }
    EXPECT_EQ(fbtreeHeight(fbt), 1u);

    /* One more item overflows the leaf and forces an inner root: height 2. */
    snprintf(buf, sizeof(buf), "k%05d", NODE_SIZE);
    insert(buf);
    EXPECT_EQ(fbtreeHeight(fbt), 2u);

    /* Enough items to fill a two-level tree and force a third level. */
    for (int i = NODE_SIZE + 1; i <= TEST_TWO_LEVEL_ITEMS + 1; i++) {
        snprintf(buf, sizeof(buf), "k%05d", i);
        insert(buf);
    }
    EXPECT_GT(fbtreeHeight(fbt), 2u);
}

/* ==========================================================================
 * Active-defrag scan tests. A defragfn that unconditionally relocates every
 * allocation (copy to a fresh block, free the original) lets LeakSanitizer and
 * AddressSanitizer catch stale references the real jemalloc-hinted path would
 * only expose under fragmentation.
 * ========================================================================== */

/* Relocate every allocation: copy to a new block and free the original, so any
 * surviving reference to the old block is a use-after-free ASAN will flag. */
static void *fbtreeDefragForceRelocate(void *ptr) {
    size_t sz = zmalloc_usable_size(ptr);
    void *newptr = zmalloc(sz);
    memcpy(newptr, ptr, sz);
    zfree(ptr);
    return newptr;
}

static void fbtreeDefragNoopItemCallback(sds old_item, sds new_item, void *ctx) {
    (void)old_item;
    (void)new_item;
    (void)ctx;
}

/* Counts item relocations reported to the scan. */
static void fbtreeDefragCountingItemCallback(sds old_item, sds new_item, void *ctx) {
    (void)old_item;
    (void)new_item;
    (*(int *)ctx)++;
}

/* Relocate only every Nth allocation (the rest stay put), to exercise the
 * partial-relocation paths a realistic jemalloc-hinted defragfn takes. */
static int g_fbtreeDefragCounter = 0;
static int g_fbtreeDefragEveryN = 1;
static void *fbtreeDefragEveryN(void *ptr) {
    if ((g_fbtreeDefragCounter++ % g_fbtreeDefragEveryN) != 0) return NULL;
    size_t sz = zmalloc_usable_size(ptr);
    void *newptr = zmalloc(sz);
    memcpy(newptr, ptr, sz);
    zfree(ptr);
    return newptr;
}

/* A defragfn that never relocates: the sweep must still traverse and terminate. */
static void *fbtreeDefragNoop(void *ptr) {
    (void)ptr;
    return NULL;
}

/* Force-relocate variant that counts invocations, for exactly-once accounting
 * across a full sweep. */
static int g_fbtreeDefragRelocations = 0;
static void *fbtreeDefragCountingForceRelocate(void *ptr) {
    g_fbtreeDefragRelocations++;
    return fbtreeDefragForceRelocate(ptr);
}

/* Count the inner nodes and leaves of a tree by structural walk, so tests can
 * assert how many allocations a sweep must visit. */
static void countTreeNodes(node *n, int *inner_count, int *leaf_count) {
    if (n->is_leaf) {
        (*leaf_count)++;
        return;
    }
    (*inner_count)++;
    innerNode *inner = (innerNode *)(void *)n;
    for (int i = 0; i < inner->header.num_items; i++) {
        countTreeNodes(inner->children[i], inner_count, leaf_count);
    }
}

/* Count inner nodes whose shared prefix spilled to a separate heap block
 * (prefix_len > EMBED_PREFIX_LEN); each is one extra allocation a sweep must
 * relocate. */
static int countSpilledPrefixNodes(node *n) {
    if (n->is_leaf) return 0;
    innerNode *inner = (innerNode *)(void *)n;
    int count = (inner->prefix_len > EMBED_PREFIX_LEN) ? 1 : 0;
    for (int i = 0; i < inner->header.num_items; i++) {
        count += countSpilledPrefixNodes(inner->children[i]);
    }
    return count;
}

/* Verify forward iteration yields exactly n keys formatted "key_%08d" for i in
 * [0, n), each stored with its trailing NUL (as insert()/createString do). */
static void verifyForwardKeys(fbtreeIndex *tree, int n) {
    char buf[32];
    fbtreeIterator it;
    fbtreeInitIterator(&it, tree);
    for (int i = 0; i < n; i++) {
        const_sds pos = fbtreeNext(&it);
        ASSERT_NE(pos, nullptr) << "missing element at index " << i;
        int len = snprintf(buf, sizeof(buf), "key_%08d", i) + 1;
        ASSERT_EQ((int)sdslen(pos), len) << "wrong length at index " << i;
        ASSERT_EQ(memcmp(pos, buf, len), 0) << "content changed at index " << i;
    }
    ASSERT_EQ(fbtreeNext(&it), nullptr) << "extra elements past index " << (n - 1);
}

/* Same as verifyForwardKeys but walking backward from the tail. */
static void verifyBackwardKeys(fbtreeIndex *tree, int n) {
    char buf[32];
    fbtreeIterator it;
    fbtreeInitIterator(&it, tree);
    for (int i = n - 1; i >= 0; i--) {
        const_sds pos = fbtreePrev(&it);
        ASSERT_NE(pos, nullptr) << "missing element at index " << i;
        int len = snprintf(buf, sizeof(buf), "key_%08d", i) + 1;
        ASSERT_EQ((int)sdslen(pos), len) << "wrong length at index " << i;
        ASSERT_EQ(memcmp(pos, buf, len), 0) << "content changed at index " << i;
    }
    ASSERT_EQ(fbtreePrev(&it), nullptr) << "extra elements before index 0";
}

/* Relocating a leaf's high-key item must update every ancestor anchor that
 * aliases it. The rightmost item of the whole tree is the high key at every
 * level, so a full sweep that relocates it exercises multi-level propagation.
 * fbtreeDebugValidate compares each anchor against its child's high key by
 * pointer and reads the anchor bytes, so a stale anchor fails it (and trips
 * ASAN on the freed block). */
TEST_F(FbtreeTest, DefragScanFixesAncestorAnchors) {
    enum { N = 5000 };
    char buf[32];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "key_%08d", i);
        insert(buf);
    }
    ASSERT_FALSE(fbt->root->is_leaf) << "test needs a multi-level tree";

    unsigned long cursor = 0;
    int guard = 0;
    do {
        cursor = fbtreeDefragScan(fbt, cursor, fbtreeDefragNoopItemCallback, NULL, fbtreeDefragForceRelocate);
        ASSERT_LT(++guard, N + 100) << "sweep did not terminate";
    } while (cursor != 0);

    char err[256];
    ASSERT_TRUE(fbtreeDebugValidate(fbt, false, err, sizeof(err))) << err;

    verifyForwardKeys(fbt, N);
}

/* Relocating every leaf struct must repoint the parent child links, the
 * leaf-chain neighbors, and the leftmost/rightmost caches. A full sweep with a
 * force-relocate defragfn moves every leaf; the tree must stay valid and
 * iterate identically both directions, and rank seeks must still land right.
 * fbtreeDebugValidate cross-checks the caches against the real end leaves. */
TEST_F(FbtreeTest, DefragScanRelocatesLeafStructs) {
    enum { N = 5000 };
    char buf[32];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "key_%08d", i);
        insert(buf);
    }
    ASSERT_FALSE(fbt->root->is_leaf) << "test needs a multi-level tree";

    unsigned long cursor = 0;
    int guard = 0;
    do {
        cursor = fbtreeDefragScan(fbt, cursor, fbtreeDefragNoopItemCallback, NULL, fbtreeDefragForceRelocate);
        ASSERT_LT(++guard, N + 100) << "sweep did not terminate";
    } while (cursor != 0);

    char err[256];
    ASSERT_TRUE(fbtreeDebugValidate(fbt, false, err, sizeof(err))) << err;

    verifyForwardKeys(fbt, N);
    verifyBackwardKeys(fbt, N);

    /* Rank seeks must still land on the right elements after relocation. */
    unsigned long ranks[] = {0, 1, N / 2, N - 1};
    for (size_t k = 0; k < sizeof(ranks) / sizeof(ranks[0]); k++) {
        unsigned long r = ranks[k];
        fbtreeIterator it;
        fbtreeInitIterator(&it, fbt);
        fbtreeSeekToRank(&it, r);
        const_sds pos = fbtreeNext(&it);
        ASSERT_NE(pos, nullptr) << "seek to rank " << r << " found nothing";
        int len = snprintf(buf, sizeof(buf), "key_%08lu", r) + 1;
        ASSERT_EQ(memcmp(pos, buf, len), 0) << "wrong element at rank " << r;
    }
}

/* The scan must relocate every inner node and repoint parent child
 * links (and the root), across a single-leaf tree (no inner nodes: a no-op),
 * a two-level tree (inner root over leaves), and a three-level tree (inner
 * root over inner nodes). The scan relocates each inner node in the call that
 * visits its leftmost descendant leaf, so a full force-relocate sweep must
 * touch every allocation exactly once: every inner node, every leaf, and
 * every item. The tree must then validate and iterate identically. */
TEST_F(FbtreeTest, DefragScanRelocatesInnerNodes) {
    int shapes[] = {5, 100, 5000};
    for (size_t s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
        int n = shapes[s];
        fbtreeIndex *tree = fbtreeCreate();
        char buf[32];
        for (int i = 0; i < n; i++) {
            snprintf(buf, sizeof(buf), "key_%08d", i);
            fbtreeInsert(tree, createString(buf));
        }

        int inner_count = 0, leaf_count = 0;
        countTreeNodes(tree->root, &inner_count, &leaf_count);

        g_fbtreeDefragRelocations = 0;
        unsigned long cursor = 0;
        int guard = 0;
        do {
            cursor = fbtreeDefragScan(tree, cursor, fbtreeDefragNoopItemCallback, NULL, fbtreeDefragCountingForceRelocate);
            ASSERT_LT(++guard, n + 100) << "n=" << n << ": sweep did not terminate";
        } while (cursor != 0);

        EXPECT_EQ(g_fbtreeDefragRelocations, inner_count + leaf_count + n)
            << "n=" << n << ": every inner node (" << inner_count << "), leaf ("
            << leaf_count << "), and item should relocate exactly once";

        char err[256];
        ASSERT_TRUE(fbtreeDebugValidate(tree, false, err, sizeof(err))) << "n=" << n << ": " << err;
        ASSERT_EQ(fbtreeLength(tree), (unsigned long)n) << "n=" << n;
        verifyForwardKeys(tree, n);

        fbtreeFree(tree);
    }
}

/* An inner node whose shared prefix exceeds the embedded capacity stores the
 * prefix in a separate heap block. The sweep must relocate that block along
 * with its owning node and re-store the pointer: with a force-relocate
 * defragfn the old block is freed, so a stale pointer inside the node copy
 * is a use-after-free on the next prefix-guided descent (ASAN verifies). */
TEST_F(FbtreeTest, DefragScanRelocatesSpilledPrefixBlocks) {
    const size_t prefix_len = EMBED_PREFIX_LEN + 46;
    enum { N = 5000 };
    char suffix[8];
    for (int i = 0; i < N; i++) {
        snprintf(suffix, sizeof(suffix), "s%05d", i);
        fbtreeInsert(fbt, createPrefixString("P", prefix_len, suffix));
    }

    int inner_count = 0, leaf_count = 0;
    countTreeNodes(fbt->root, &inner_count, &leaf_count);
    int spilled = countSpilledPrefixNodes(fbt->root);
    ASSERT_GT(spilled, 0) << "construction must produce spilled-prefix inner nodes";

    g_fbtreeDefragRelocations = 0;
    unsigned long cursor = 0;
    int guard = 0;
    do {
        cursor = fbtreeDefragScan(fbt, cursor, fbtreeDefragNoopItemCallback, NULL, fbtreeDefragCountingForceRelocate);
        ASSERT_LT(++guard, N + 100) << "sweep did not terminate";
    } while (cursor != 0);

    EXPECT_EQ(g_fbtreeDefragRelocations, inner_count + spilled + leaf_count + N)
        << "every inner node, spilled prefix block, leaf, and item should relocate exactly once";

    char err[256];
    ASSERT_TRUE(fbtreeDebugValidate(fbt, false, err, sizeof(err))) << err;
    ASSERT_EQ(fbtreeLength(fbt), (unsigned long)N);

    /* Prefix-guided descents read the relocated prefix blocks: seek every
     * 61st member by value and check its rank. */
    for (int i = 0; i < N; i += 61) {
        snprintf(suffix, sizeof(suffix), "s%05d", i);
        sds probe = createPrefixString("P", prefix_len, suffix);
        fbtreeIterator it;
        fbtreeInitIterator(&it, fbt);
        ASSERT_EQ(fbtreeSeekToValue(probe, &it), (long)i) << "seek for member " << i;
        const_sds pos = fbtreeNext(&it);
        ASSERT_NE(pos, nullptr);
        ASSERT_EQ(memcmp(pos, probe, sdslen(probe)), 0) << "wrong member at rank " << i;
        sdsfree(probe);
    }
}

/* A sweep must visit every item exactly once and then terminate. With a
 * force-relocate defragfn, item_callback fires once per item, so the total
 * count equals the length and the cursor returns to 0. */
TEST_F(FbtreeTest, DefragScanSweepVisitsEachItemOnce) {
    enum { N = 5000 };
    char buf[32];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "key_%08d", i);
        insert(buf);
    }
    int relocated = 0;
    unsigned long cursor = 0;
    int calls = 0;
    do {
        cursor = fbtreeDefragScan(fbt, cursor, fbtreeDefragCountingItemCallback, &relocated, fbtreeDefragForceRelocate);
        ASSERT_LT(++calls, N + 100) << "sweep did not terminate";
    } while (cursor != 0);
    EXPECT_EQ(relocated, N) << "each item should be relocated exactly once";
    char err[256];
    ASSERT_TRUE(fbtreeDebugValidate(fbt, false, err, sizeof(err))) << err;
    ASSERT_EQ(fbtreeLength(fbt), (unsigned long)N);
}

/* Partial relocation: only some allocations move. The scan must patch the ones
 * that move and leave the rest, keeping the tree valid and content intact. */
TEST_F(FbtreeTest, DefragScanPartialRelocation) {
    enum { N = 5000 };
    char buf[32];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "key_%08d", i);
        insert(buf);
    }
    g_fbtreeDefragCounter = 0;
    g_fbtreeDefragEveryN = 3;
    unsigned long cursor = 0;
    int guard = 0;
    do {
        cursor = fbtreeDefragScan(fbt, cursor, fbtreeDefragNoopItemCallback, NULL, fbtreeDefragEveryN);
        ASSERT_LT(++guard, N + 100) << "sweep did not terminate";
    } while (cursor != 0);
    char err[256];
    ASSERT_TRUE(fbtreeDebugValidate(fbt, false, err, sizeof(err))) << err;
    verifyForwardKeys(fbt, N);
}

/* A no-op defragfn relocates nothing: the sweep must still terminate and leave
 * the tree byte-for-byte unchanged. */
TEST_F(FbtreeTest, DefragScanNoRelocation) {
    enum { N = 5000 };
    char buf[32];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "key_%08d", i);
        insert(buf);
    }
    node *root_before = fbt->root;
    unsigned long cursor = 0;
    int guard = 0;
    do {
        cursor = fbtreeDefragScan(fbt, cursor, fbtreeDefragNoopItemCallback, NULL, fbtreeDefragNoop);
        ASSERT_LT(++guard, N + 100) << "sweep did not terminate";
    } while (cursor != 0);
    EXPECT_EQ(fbt->root, root_before) << "no-op defrag must not move the root";
    verifyForwardKeys(fbt, N);
}

/* A single-leaf tree is its own root: relocating that leaf in the scan must
 * repoint fbt->root (the depth==0 path). */
TEST_F(FbtreeTest, DefragScanRelocatesRootLeaf) {
    enum { N = 5 };
    char buf[32];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "k%04d", i);
        insert(buf);
    }
    ASSERT_TRUE(fbt->root->is_leaf) << "test needs a single-leaf tree";

    unsigned long cursor = fbtreeDefragScan(fbt, 0, fbtreeDefragNoopItemCallback, NULL, fbtreeDefragForceRelocate);
    EXPECT_EQ(cursor, 0UL) << "single leaf is one sweep step";
    EXPECT_TRUE(fbt->root->is_leaf);
    char err[256];
    ASSERT_TRUE(fbtreeDebugValidate(fbt, false, err, sizeof(err))) << err;

    fbtreeIterator it;
    fbtreeInitIterator(&it, fbt);
    for (int i = 0; i < N; i++) {
        const_sds pos = fbtreeNext(&it);
        ASSERT_NE(pos, nullptr) << "missing element at index " << i;
        int len = snprintf(buf, sizeof(buf), "k%04d", i) + 1;
        ASSERT_EQ(memcmp(pos, buf, len), 0) << "content changed at index " << i;
    }
    ASSERT_EQ(fbtreeNext(&it), nullptr);
}

/* End-to-end: run a full sweep force-relocating everything — inner nodes,
 * leaves, and items — and confirm the tree validates and answers forward,
 * backward, and rank queries identically. */
TEST_F(FbtreeTest, DefragScanFullSweepEndToEnd) {
    enum { N = 5000 };
    char buf[32];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "key_%08d", i);
        insert(buf);
    }

    unsigned long cursor = 0;
    int guard = 0;
    do {
        cursor = fbtreeDefragScan(fbt, cursor, fbtreeDefragNoopItemCallback, NULL, fbtreeDefragForceRelocate);
        ASSERT_LT(++guard, N + 100) << "sweep did not terminate";
    } while (cursor != 0);

    char err[256];
    ASSERT_TRUE(fbtreeDebugValidate(fbt, false, err, sizeof(err))) << err;
    verifyForwardKeys(fbt, N);
    verifyBackwardKeys(fbt, N);

    unsigned long ranks[] = {0, 1, N / 2, N - 1};
    for (size_t k = 0; k < sizeof(ranks) / sizeof(ranks[0]); k++) {
        unsigned long r = ranks[k];
        fbtreeIterator it;
        fbtreeInitIterator(&it, fbt);
        fbtreeSeekToRank(&it, r);
        const_sds pos = fbtreeNext(&it);
        ASSERT_NE(pos, nullptr) << "seek to rank " << r;
        int len = snprintf(buf, sizeof(buf), "key_%08lu", r) + 1;
        ASSERT_EQ(memcmp(pos, buf, len), 0) << "wrong element at rank " << r;
    }
}

/* Dismissal hints memory to the OS (madvise DONTNEED) for contents this
 * process will not read again -- the fork child after serializing a value.
 * The observable unit contract is therefore not content preservation but
 * coverage: every allocation the tree owns is hinted exactly once. The
 * zmadvise_dontneed wrapper counts calls: one per item, per leaf struct,
 * and per inner node. */
TEST_F(FbtreeTest, DismissMemoryHintsEveryAllocationOnce) {
    enum { N = 3000 };
    char buf[32];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "key_%08d", i);
        insert(buf);
    }
    int inner_count = 0, leaf_count = 0;
    countTreeNodes(fbt->root, &inner_count, &leaf_count);
    ASSERT_EQ(countSpilledPrefixNodes(fbt->root), 0);

    MockValkey mock;
    EXPECT_CALL(mock, zmadvise_dontneed(_, _)).Times(N + leaf_count + inner_count);
    fbtreeDismissMemory(fbt);
}

/* Same coverage contract on a tree whose inner nodes carry spilled
 * long-prefix heap blocks: each spilled block is one additional allocation
 * the walk must hint. */
TEST_F(FbtreeTest, DismissMemoryWalksInnerNodesAndSpilledPrefixes) {
    const size_t prefix_len = EMBED_PREFIX_LEN + 46;
    enum { N = 5000 };
    char suffix[8];
    for (int i = 0; i < N; i++) {
        snprintf(suffix, sizeof(suffix), "s%05d", i);
        fbtreeInsert(fbt, createPrefixString("P", prefix_len, suffix));
    }
    int inner_count = 0, leaf_count = 0;
    countTreeNodes(fbt->root, &inner_count, &leaf_count);
    int spilled = countSpilledPrefixNodes(fbt->root);
    ASSERT_GT(spilled, 0) << "construction must produce spilled-prefix inner nodes";

    MockValkey mock;
    EXPECT_CALL(mock, zmadvise_dontneed(_, _)).Times(N + leaf_count + inner_count + spilled);
    fbtreeDismissMemory(fbt);
}

/* ==========================================================================
 * featureSearchSIMD tests - exercise the scalar, SSE2, AVX2, and NEON
 * implementations through test wrappers. FEATURE_SIZE, FEATURE_ROW_SIZE,
 * and FEATURE_BIAS come from fbtree_internal.h.
 * ========================================================================== */

#define TEST_ASSERT(x) ASSERT_TRUE(x)
#define TEST_ASSERT_MESSAGE(msg, x) ASSERT_TRUE(x) << msg

#define BIASED(x) ((char)((unsigned char)(x) ^ FEATURE_BIAS))

#define TEST_SETUP()                               \
    char features[FEATURE_SIZE][FEATURE_ROW_SIZE]; \
    initFeatures(features)

static void initFeatures(char features[FEATURE_SIZE][FEATURE_ROW_SIZE]) {
    for (int j = 0; j < FEATURE_SIZE; j++)
        for (int i = 0; i < FEATURE_ROW_SIZE; i++)
            features[j][i] = BIASED(0);
}

/* Declare test wrappers - these call the actual implementations in fbtree.c. */
extern "C" {
void featureSearchSIMD_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                    int num_keys,
                                    const unsigned char target[FEATURE_SIZE],
                                    int *out_left,
                                    int *out_right);

#if HAVE_X86_SIMD
void featureSearchSIMD_avx2_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                         int num_keys,
                                         const unsigned char target[FEATURE_SIZE],
                                         int *out_left,
                                         int *out_right);

void featureSearchSIMD_sse2_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                         int num_keys,
                                         const unsigned char target[FEATURE_SIZE],
                                         int *out_left,
                                         int *out_right);
#endif

void featureSearchSIMD_scalar_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                           int num_keys,
                                           const unsigned char target[FEATURE_SIZE],
                                           int *out_left,
                                           int *out_right);
}

typedef void (*FeatureSearchFn)(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                int num_keys,
                                const unsigned char target[FEATURE_SIZE],
                                int *out_left,
                                int *out_right);

/* Test all available implementations produce identical results against scalar (truth) */
static void testAllImpls(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                         int num_keys,
                         const unsigned char target[FEATURE_SIZE]) {
    int scalar_left, scalar_right;
    featureSearchSIMD_scalar_test_wrapper(features, num_keys, target, &scalar_left, &scalar_right);

    int def_left, def_right;
    featureSearchSIMD_test_wrapper(features, num_keys, target, &def_left, &def_right);
    TEST_ASSERT_MESSAGE("default mismatch", def_left == scalar_left && def_right == scalar_right);

#if HAVE_X86_SIMD
    int sse_left, sse_right;
    featureSearchSIMD_sse2_test_wrapper(features, num_keys, target, &sse_left, &sse_right);
    TEST_ASSERT_MESSAGE("sse2 mismatch", sse_left == scalar_left && sse_right == scalar_right);

    if (__builtin_cpu_supports("avx2")) {
        int avx_left, avx_right;
        featureSearchSIMD_avx2_test_wrapper(features, num_keys, target, &avx_left, &avx_right);
        TEST_ASSERT_MESSAGE("avx2 mismatch", avx_left == scalar_left && avx_right == scalar_right);
    }
#endif
}

/* ==========================================================================
 * Expected values test - demonstrates the semantics of featureSearchSIMD.
 * Range [left, right) = candidate children that might contain target.
 * ========================================================================== */

TEST(FeatureSearchTest, expected_values) {
    TEST_SETUP();
    features[0][0] = BIASED(0x20);
    features[0][1] = BIASED(0x40);
    features[0][2] = BIASED(0x40);
    features[0][3] = BIASED(0x40);
    features[0][4] = BIASED(0x60);
    features[0][5] = BIASED(0x80);
    int left, right;

    /* Case 1: Target before all - empty range (no candidates) */
    unsigned char t1[FEATURE_SIZE] = {0x10, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 6, t1, &left, &right);
    TEST_ASSERT_MESSAGE("before all: left", left == 0);
    TEST_ASSERT_MESSAGE("before all: right", right == 0);
    testAllImpls(features, 6, t1);

    /* Case 2: Target after all - empty range (no candidates) */
    unsigned char t2[FEATURE_SIZE] = {0x90, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 6, t2, &left, &right);
    TEST_ASSERT_MESSAGE("after all: left", left == 6);
    TEST_ASSERT_MESSAGE("after all: right", right == 6);
    testAllImpls(features, 6, t2);

    /* Case 3: Exact match on unique value - 1 candidate */
    unsigned char t3[FEATURE_SIZE] = {0x20, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 6, t3, &left, &right);
    TEST_ASSERT_MESSAGE("unique match: left", left == 0);
    TEST_ASSERT_MESSAGE("unique match: right", right == 1);
    testAllImpls(features, 6, t3);

    /* Case 4: Match on duplicates - few candidates (3 matches) */
    unsigned char t4[FEATURE_SIZE] = {0x40, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 6, t4, &left, &right);
    TEST_ASSERT_MESSAGE("duplicates: left", left == 1);
    TEST_ASSERT_MESSAGE("duplicates: right", right == 4);
    testAllImpls(features, 6, t4);

    /* Case 5: All same features - all candidates (nothing narrowed) */
    for (int i = 0; i < 5; i++) features[0][i] = BIASED(0x50);
    unsigned char t5[FEATURE_SIZE] = {0x50, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 5, t5, &left, &right);
    TEST_ASSERT_MESSAGE("all same: left", left == 0);
    TEST_ASSERT_MESSAGE("all same: right", right == 5);
    testAllImpls(features, 5, t5);

    /* Case 6: Between values - empty range (target 0x50 between 0x40 and 0x60) */
    features[0][0] = BIASED(0x20);
    features[0][1] = BIASED(0x40);
    features[0][2] = BIASED(0x60);
    features[0][3] = BIASED(0x80);
    unsigned char t6[FEATURE_SIZE] = {0x50, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 4, t6, &left, &right);
    TEST_ASSERT_MESSAGE("between: left", left == 2);
    TEST_ASSERT_MESSAGE("between: right", right == 2);
    testAllImpls(features, 4, t6);
}

/* ==========================================================================
 * Edge cases - empty, single element, two elements
 * ========================================================================== */

TEST(FeatureSearchTest, empty) {
    TEST_SETUP();
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 0, target);
}

TEST(FeatureSearchTest, single_match) {
    TEST_SETUP();
    features[0][0] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 1, target);
}

TEST(FeatureSearchTest, single_less) {
    TEST_SETUP();
    features[0][0] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x40, 0, 0, 0};
    testAllImpls(features, 1, target);
}

TEST(FeatureSearchTest, single_greater) {
    TEST_SETUP();
    features[0][0] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x60, 0, 0, 0};
    testAllImpls(features, 1, target);
}

TEST(FeatureSearchTest, two_elements) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x70);
    unsigned char targets[][FEATURE_SIZE] = {
        {0x20, 0, 0, 0}, {0x30, 0, 0, 0}, {0x50, 0, 0, 0}, {0x70, 0, 0, 0}, {0x80, 0, 0, 0}};
    for (int i = 0; i < 5; i++) {
        testAllImpls(features, 2, targets[i]);
    }
}

/* ==========================================================================
 * Basic scenarios - small arrays, before/after all, not found
 * ========================================================================== */

TEST(FeatureSearchTest, three_elements) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, before_all) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x20, 0, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, after_all) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x80, 0, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, not_found) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x45, 0, 0, 0}; /* Between 0x30 and 0x50 */
    testAllImpls(features, 3, target);
}

/* ==========================================================================
 * Duplicates - multiple matching features
 * ========================================================================== */

TEST(FeatureSearchTest, duplicates) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x50);
    features[0][3] = BIASED(0x50);
    features[0][4] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 5, target);
}

TEST(FeatureSearchTest, all_same) {
    TEST_SETUP();
    for (int i = 0; i < 10; i++)
        features[0][i] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 10, target);
}

/* ==========================================================================
 * Multi-byte features - tests all 4 feature bytes
 * ========================================================================== */

TEST(FeatureSearchTest, multibyte) {
    TEST_SETUP();
    features[0][0] = BIASED(0x50);
    features[1][0] = BIASED(0x10);
    features[0][1] = BIASED(0x50);
    features[1][1] = BIASED(0x30);
    features[0][2] = BIASED(0x50);
    features[1][2] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x50, 0x30, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, multibyte_tiebreak) {
    TEST_SETUP();
    for (int i = 0; i < 5; i++) {
        features[0][i] = BIASED(0x50);
        features[1][i] = BIASED(i * 0x20);
    }
    unsigned char target[FEATURE_SIZE] = {0x50, 0x40, 0, 0};
    testAllImpls(features, 5, target);
}

TEST(FeatureSearchTest, all_bytes_matter) {
    TEST_SETUP();
    for (int i = 0; i < 4; i++) {
        features[0][i] = BIASED(0x50);
        features[1][i] = BIASED(0x50);
        features[2][i] = BIASED(0x50);
        features[3][i] = BIASED(i * 0x30);
    }
    unsigned char target[FEATURE_SIZE] = {0x50, 0x50, 0x50, 0x60};
    testAllImpls(features, 4, target);
}

/* Test that each byte position can be the deciding factor.
 * 4 children with features: all bytes equal except one differs.
 * Child 0: {0x50, 0x50, 0x50, 0x50} - all match
 * Child 1: differs only on byte being tested */
TEST(FeatureSearchTest, deciding_byte) {
    TEST_SETUP();

    /* Test each byte position (1, 2, 3) as the deciding factor.
     * Byte 0 is already well-tested by single-byte tests. */
    for (int deciding_byte = 1; deciding_byte < FEATURE_SIZE; deciding_byte++) {
        initFeatures(features);

        /* Set up 4 children, all with 0x50 in all bytes */
        for (int child = 0; child < 4; child++)
            for (int byte = 0; byte < FEATURE_SIZE; byte++)
                features[byte][child] = BIASED(0x50);

        /* Make children differ only on the deciding byte */
        features[deciding_byte][0] = BIASED(0x20);
        features[deciding_byte][1] = BIASED(0x40);
        features[deciding_byte][2] = BIASED(0x60);
        features[deciding_byte][3] = BIASED(0x80);

        /* Target matches child 1 exactly */
        unsigned char target[FEATURE_SIZE] = {0x50, 0x50, 0x50, 0x50};
        target[deciding_byte] = 0x40;
        testAllImpls(features, 4, target);

        /* Target between child 1 and 2 - should find empty range */
        target[deciding_byte] = 0x50;
        testAllImpls(features, 4, target);

        /* Target less than all on deciding byte */
        target[deciding_byte] = 0x10;
        testAllImpls(features, 4, target);

        /* Target greater than all on deciding byte */
        target[deciding_byte] = 0x90;
        testAllImpls(features, 4, target);
    }
}

/* ==========================================================================
 * Value boundaries - signed/unsigned edge cases
 * ========================================================================== */

TEST(FeatureSearchTest, high_values) {
    TEST_SETUP();
    features[0][0] = BIASED(0x7F);
    features[0][1] = BIASED(0x80);
    features[0][2] = BIASED(0xFF);
    unsigned char target[FEATURE_SIZE] = {0x80, 0, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, boundary_values) {
    TEST_SETUP();
    features[0][0] = BIASED(0x00);
    features[0][1] = BIASED(0x7F);
    features[0][2] = BIASED(0x80);
    features[0][3] = BIASED(0xFF);
    unsigned char targets[][FEATURE_SIZE] = {
        {0x00, 0, 0, 0}, {0x7F, 0, 0, 0}, {0x80, 0, 0, 0}, {0xFF, 0, 0, 0}};
    for (int i = 0; i < 4; i++) {
        testAllImpls(features, 4, targets[i]);
    }
}

/* ==========================================================================
 * SIMD-specific - chunk boundaries, validity masks
 * ========================================================================== */

TEST(FeatureSearchTest, cross_chunk_boundary) {
    TEST_SETUP();
    int boundaries[] = {16, 32}; /* SSE and AVX boundaries */
    int sizes[] = {20, 40};
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};

    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < sizes[b]; i++)
            features[0][i] = BIASED(i < boundaries[b] ? 0x30 : 0x70);
        testAllImpls(features, sizes[b], target);
    }
}

TEST(FeatureSearchTest, chunk_boundaries) {
    TEST_SETUP();
    int sizes[] = {16, 32, 48};
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < sizes[s]; i++)
            features[0][i] = BIASED(i * 4);
        unsigned char target[FEATURE_SIZE] = {(unsigned char)(sizes[s] / 2 * 4), 0, 0, 0};
        testAllImpls(features, sizes[s], target);
    }
}

TEST(FeatureSearchTest, last_in_chunk) {
    TEST_SETUP();
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    int positions[] = {15, 31, 47, 63};
    int sizes[] = {20, 40, 55, 64};

    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < sizes[t]; i++)
            features[0][i] = BIASED(i == positions[t] ? 0x50 : 0x30);
        testAllImpls(features, sizes[t], target);
    }
}

TEST(FeatureSearchTest, first_in_chunk) {
    TEST_SETUP();
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    int positions[] = {16, 32, 48};
    int sizes[] = {20, 40, 55};

    for (int t = 0; t < 3; t++) {
        for (int i = 0; i < sizes[t]; i++)
            features[0][i] = BIASED(i == positions[t] ? 0x50 : 0x70);
        testAllImpls(features, sizes[t], target);
    }
}

TEST(FeatureSearchTest, no_false_positives) {
    TEST_SETUP();

    /* Set up: valid keys are all 0x30, but invalid positions have 0x50 */
    for (int i = 0; i < 64; i++)
        features[0][i] = BIASED(i < 20 ? 0x30 : 0x50);

    /* Search for 0x50 - should NOT find it since it's beyond num_keys=20 */
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 20, target);
}

TEST(FeatureSearchTest, duplicates_cross_chunk) {
    TEST_SETUP();

    /* Duplicates from position 14 to 18 (crosses chunk 0/1 boundary) */
    for (int i = 0; i < 30; i++)
        features[0][i] = BIASED(i < 14 ? 0x30 : (i < 19 ? 0x50 : 0x70));

    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 30, target);
}

/* ==========================================================================
 * Realistic sizes - full row, typical node size
 * ========================================================================== */

TEST(FeatureSearchTest, full_row) {
    TEST_SETUP();
    for (int i = 0; i < 64; i++)
        features[0][i] = BIASED(i * 3);
    unsigned char target[FEATURE_SIZE] = {96, 0, 0, 0};
    testAllImpls(features, 64, target);
}

TEST(FeatureSearchTest, typical_node_size) {
    TEST_SETUP();
    for (int i = 0; i < 61; i++)
        features[0][i] = BIASED(i * 4);
    unsigned char targets[][FEATURE_SIZE] = {
        {0, 0, 0, 0},
        {60, 0, 0, 0},
        {64, 0, 0, 0},
        {124, 0, 0, 0},
        {128, 0, 0, 0},
        {188, 0, 0, 0},
        {192, 0, 0, 0},
        {240, 0, 0, 0},
    };
    for (int i = 0; i < 8; i++) {
        testAllImpls(features, 61, targets[i]);
    }
}
