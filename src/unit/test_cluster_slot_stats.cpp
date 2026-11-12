/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdint>

extern "C" {
#include "server.h"

/* Struct mirroring the file-scope definition in cluster_slot_stats.c. */
typedef struct {
    int slot;
    uint64_t stat;
} slotStatForSort;

/* Wrapper function declarations for accessing static comparators. */
int testOnlySlotStatForSortAscCmp(const void *a, const void *b);
int testOnlySlotStatForSortDescCmp(const void *a, const void *b);
}

class ClusterSlotStatsTest : public ::testing::Test {};

/* Small-value sanity: ascending comparator orders by stat value. */
TEST_F(ClusterSlotStatsTest, AscCmpSmallValues) {
    slotStatForSort a = {0, 10};
    slotStatForSort b = {1, 20};
    EXPECT_LT(testOnlySlotStatForSortAscCmp(&a, &b), 0);
    EXPECT_GT(testOnlySlotStatForSortAscCmp(&b, &a), 0);
}

/* Small-value sanity: descending comparator orders by stat value (reversed). */
TEST_F(ClusterSlotStatsTest, DescCmpSmallValues) {
    slotStatForSort a = {0, 10};
    slotStatForSort b = {1, 20};
    EXPECT_GT(testOnlySlotStatForSortDescCmp(&a, &b), 0);
    EXPECT_LT(testOnlySlotStatForSortDescCmp(&b, &a), 0);
}

/* Ascending: a.stat < b.stat with difference exceeding INT_MAX must return negative. */
TEST_F(ClusterSlotStatsTest, AscCmpDiffExceedsIntMax) {
    slotStatForSort a = {0, 1};
    slotStatForSort b = {1, 5000000000ULL};
    EXPECT_LT(testOnlySlotStatForSortAscCmp(&a, &b), 0);
}

/* Ascending: a.stat > b.stat with difference exceeding INT_MAX must return positive. */
TEST_F(ClusterSlotStatsTest, AscCmpLargeGreaterThanSmall) {
    slotStatForSort a = {0, 5000000000ULL};
    slotStatForSort b = {1, 1};
    EXPECT_GT(testOnlySlotStatForSortAscCmp(&a, &b), 0);
}

/* Descending: a.stat < b.stat with difference exceeding INT_MAX must return positive. */
TEST_F(ClusterSlotStatsTest, DescCmpDiffExceedsIntMax) {
    slotStatForSort a = {0, 1};
    slotStatForSort b = {1, 5000000000ULL};
    EXPECT_GT(testOnlySlotStatForSortDescCmp(&a, &b), 0);
}

/* Descending: a.stat > b.stat with difference exceeding INT_MAX must return negative. */
TEST_F(ClusterSlotStatsTest, DescCmpLargeGreaterThanSmall) {
    slotStatForSort a = {0, 5000000000ULL};
    slotStatForSort b = {1, 1};
    EXPECT_LT(testOnlySlotStatForSortDescCmp(&a, &b), 0);
}

/* Exactly 2^32 difference: the buggy subtraction-cast truncates to zero.
 * This must NOT return 0 (equal). */
TEST_F(ClusterSlotStatsTest, AscCmpExactly2Pow32) {
    slotStatForSort a = {0, 4294967296ULL};
    slotStatForSort b = {1, 0};
    EXPECT_GT(testOnlySlotStatForSortAscCmp(&a, &b), 0);
    EXPECT_LT(testOnlySlotStatForSortAscCmp(&b, &a), 0);
}

TEST_F(ClusterSlotStatsTest, DescCmpExactly2Pow32) {
    slotStatForSort a = {0, 4294967296ULL};
    slotStatForSort b = {1, 0};
    EXPECT_LT(testOnlySlotStatForSortDescCmp(&a, &b), 0);
    EXPECT_GT(testOnlySlotStatForSortDescCmp(&b, &a), 0);
}

/* Tie-break: equal stat values order by ascending slot in both comparators. */
TEST_F(ClusterSlotStatsTest, AscCmpTieBreakBySlot) {
    slotStatForSort a = {5, 100};
    slotStatForSort b = {10, 100};
    EXPECT_LT(testOnlySlotStatForSortAscCmp(&a, &b), 0);
    EXPECT_GT(testOnlySlotStatForSortAscCmp(&b, &a), 0);
}

TEST_F(ClusterSlotStatsTest, DescCmpTieBreakBySlot) {
    slotStatForSort a = {5, 100};
    slotStatForSort b = {10, 100};
    EXPECT_LT(testOnlySlotStatForSortDescCmp(&a, &b), 0);
    EXPECT_GT(testOnlySlotStatForSortDescCmp(&b, &a), 0);
}

/* Equal entries: same slot and same stat must return zero. */
TEST_F(ClusterSlotStatsTest, AscCmpEqual) {
    slotStatForSort a = {7, 42};
    slotStatForSort b = {7, 42};
    EXPECT_EQ(testOnlySlotStatForSortAscCmp(&a, &b), 0);
}

TEST_F(ClusterSlotStatsTest, DescCmpEqual) {
    slotStatForSort a = {7, 42};
    slotStatForSort b = {7, 42};
    EXPECT_EQ(testOnlySlotStatForSortDescCmp(&a, &b), 0);
}
