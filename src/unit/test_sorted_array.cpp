/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdlib>

extern "C" {
#include "sorted_array.h"
}

/* Counts calls to the free method so ownership can be asserted. */
static int free_count = 0;

static void countingFree(void *) {
    free_count++;
}

static int compareLongLong(const void *a, const void *b) {
    long long x = *(const long long *)a;
    long long y = *(const long long *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* Verify the ordering on the internal array. */
static void assertSortedOrder(sortedArray *sa) {
    for (unsigned long i = 1; i < sortedArrayLen(sa); i++) {
        void *prev = sa->nodes[i - 1].value;
        void *curr = sa->nodes[i].value;
        ASSERT_GE(sa->compare(prev, curr), 0) << "order violated between index " << (i - 1) << " and " << i;
    }
}

class SortedArrayTest : public ::testing::Test {
  protected:
    void SetUp() override {
        free_count = 0;
    }
};

TEST_F(SortedArrayTest, EmptyCollection) {
    sortedArray *sa = sortedArrayCreate(compareLongLong);

    ASSERT_EQ(sortedArrayLen(sa), 0UL);
    ASSERT_EQ(sa->capacity, 0UL); /* nothing allocated until the first insert */
    ASSERT_EQ(sortedArrayPeekMin(sa), nullptr);
    ASSERT_EQ(sortedArrayExtractMin(sa), nullptr);
    ASSERT_EQ(sortedArrayGet(sa, 0), nullptr);

    sortedArrayRelease(sa);
}

TEST_F(SortedArrayTest, SingleElement) {
    sortedArray *sa = sortedArrayCreate(compareLongLong);
    long long v = 42;

    sortedArrayInsert(sa, &v);
    ASSERT_EQ(sortedArrayLen(sa), 1UL);
    ASSERT_EQ(*(long long *)sortedArrayPeekMin(sa), 42);
    ASSERT_EQ(sortedArrayGet(sa, 0), &v);
    ASSERT_EQ(sortedArrayGet(sa, 1), nullptr);

    ASSERT_EQ(sortedArrayExtractMin(sa), &v);
    ASSERT_EQ(sortedArrayLen(sa), 0UL);
    ASSERT_EQ(sortedArrayPeekMin(sa), nullptr);

    sortedArrayRelease(sa);
}

TEST_F(SortedArrayTest, RandomInserts) {
    sortedArray *sa = sortedArrayCreate(compareLongLong);
    long long values[1000];
    long long expected[1000];

    for (int i = 0; i < 1000; i++) {
        values[i] = rand() % 500;
        expected[i] = values[i];
        sortedArrayInsert(sa, &values[i]);
    }

    qsort(expected, 1000, sizeof(long long), compareLongLong);
    for (int i = 0; i < 1000; i++) {
        ASSERT_EQ(*(long long *)sortedArrayExtractMin(sa), expected[i]);
    }
    ASSERT_EQ(sortedArrayLen(sa), 0UL);

    sortedArrayRelease(sa);
}

TEST_F(SortedArrayTest, InterleavedInsertAndExtract) {
    sortedArray *sa = sortedArrayCreate(compareLongLong);
    long long values[2000];
    int inserted = 0;

    for (int round = 0; round < 2000; round++) {
        if (sortedArrayLen(sa) == 0 || (rand() % 3) != 0) {
            values[inserted] = rand() % 10000;
            sortedArrayInsert(sa, &values[inserted]);
            inserted++;
        } else {
            sortedArrayExtractMin(sa);
        }
        assertSortedOrder(sa);
    }

    sortedArrayRelease(sa);
}

TEST_F(SortedArrayTest, ReleaseNoFreeMethod) {
    sortedArray *sa = sortedArrayCreate(compareLongLong);
    long long values[16];

    /* A freshly created sa has no free method. */
    ASSERT_EQ(sortedArrayGetFreeMethod(sa), nullptr);
    for (int i = 0; i < 16; i++) {
        values[i] = i;
        sortedArrayInsert(sa, &values[i]);
    }
    sortedArrayRelease(sa);
    ASSERT_EQ(free_count, 0);
}

TEST_F(SortedArrayTest, ReleaseWithFreeMethod) {
    sortedArray *sa = sortedArrayCreate(compareLongLong);
    sortedArraySetFreeMethod(sa, countingFree);
    ASSERT_EQ(sortedArrayGetFreeMethod(sa), countingFree);

    long long values[40];
    for (int i = 0; i < 40; i++) {
        values[i] = i;
        sortedArrayInsert(sa, &values[i]);
    }

    sortedArrayRelease(sa);
    ASSERT_EQ(free_count, 40);
}

TEST_F(SortedArrayTest, EmptyMethod) {
    sortedArray *sa = sortedArrayCreate(compareLongLong);
    sortedArraySetFreeMethod(sa, countingFree);

    long long values[40];
    for (int i = 0; i < 40; i++) {
        values[i] = i;
        sortedArrayInsert(sa, &values[i]);
    }
    unsigned long capacity_when_full = sa->capacity;

    sortedArrayEmpty(sa);
    ASSERT_EQ(free_count, 40);
    ASSERT_EQ(sortedArrayLen(sa), 0UL);
    ASSERT_EQ(sa->capacity, capacity_when_full);
    ASSERT_EQ(sortedArrayPeekMin(sa), nullptr);

    /* Refill to prove the emptied structure is still usable. */
    long long refill[10];
    for (int i = 0; i < 10; i++) {
        refill[i] = 100 - i;
        sortedArrayInsert(sa, &refill[i]);
    }
    ASSERT_EQ(sortedArrayLen(sa), 10UL);
    ASSERT_EQ(sa->capacity, capacity_when_full); /* Capacity should remain stable */
    ASSERT_EQ(*(long long *)sortedArrayPeekMin(sa), 91);
    assertSortedOrder(sa);

    sortedArrayRelease(sa);
    ASSERT_EQ(free_count, 50); /* 40 + 10 */
}

TEST_F(SortedArrayTest, ExtractMinTransfersOwnershipToCaller) {
    sortedArray *sa = sortedArrayCreate(compareLongLong);
    sortedArraySetFreeMethod(sa, countingFree);

    long long values[8];
    for (int i = 0; i < 8; i++) {
        values[i] = i;
        sortedArrayInsert(sa, &values[i]);
    }

    long long *element = (long long *)sortedArrayExtractMin(sa);
    ASSERT_TRUE(element != nullptr);
    ASSERT_EQ(free_count, 0);
    ASSERT_EQ(sortedArrayLen(sa), 7UL);

    countingFree(element); /* the caller owns it now */
    ASSERT_EQ(free_count, 1);

    sortedArrayRelease(sa);
    ASSERT_EQ(free_count, 8); /* 1 + 7 */
}
