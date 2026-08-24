/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_help.h"

#include <stdint.h>

/* Struct mirroring the file-scope definition in cluster_slot_stats.c. */
typedef struct {
    int slot;
    uint64_t stat;
} slotStatForSort;

/* Wrapper function declarations for accessing static comparators. */
int testOnlySlotStatForSortAscCmp(const void *a, const void *b);
int testOnlySlotStatForSortDescCmp(const void *a, const void *b);

/* Small-value sanity: ascending comparator orders by stat value. */
int test_slotStatForSortAscCmpSmallValues(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {0, 10};
    slotStatForSort b = {1, 20};
    TEST_ASSERT(testOnlySlotStatForSortAscCmp(&a, &b) < 0);
    TEST_ASSERT(testOnlySlotStatForSortAscCmp(&b, &a) > 0);

    return 0;
}

/* Small-value sanity: descending comparator orders by stat value (reversed). */
int test_slotStatForSortDescCmpSmallValues(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {0, 10};
    slotStatForSort b = {1, 20};
    TEST_ASSERT(testOnlySlotStatForSortDescCmp(&a, &b) > 0);
    TEST_ASSERT(testOnlySlotStatForSortDescCmp(&b, &a) < 0);

    return 0;
}

/* Ascending: a.stat < b.stat with difference exceeding INT_MAX must return negative. */
int test_slotStatForSortAscCmpDiffExceedsIntMax(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {0, 1};
    slotStatForSort b = {1, 5000000000ULL};
    TEST_ASSERT(testOnlySlotStatForSortAscCmp(&a, &b) < 0);

    return 0;
}

/* Ascending: a.stat > b.stat with difference exceeding INT_MAX must return positive. */
int test_slotStatForSortAscCmpLargeGreaterThanSmall(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {0, 5000000000ULL};
    slotStatForSort b = {1, 1};
    TEST_ASSERT(testOnlySlotStatForSortAscCmp(&a, &b) > 0);

    return 0;
}

/* Descending: a.stat < b.stat with difference exceeding INT_MAX must return positive. */
int test_slotStatForSortDescCmpDiffExceedsIntMax(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {0, 1};
    slotStatForSort b = {1, 5000000000ULL};
    TEST_ASSERT(testOnlySlotStatForSortDescCmp(&a, &b) > 0);

    return 0;
}

/* Descending: a.stat > b.stat with difference exceeding INT_MAX must return negative. */
int test_slotStatForSortDescCmpLargeGreaterThanSmall(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {0, 5000000000ULL};
    slotStatForSort b = {1, 1};
    TEST_ASSERT(testOnlySlotStatForSortDescCmp(&a, &b) < 0);

    return 0;
}

/* Exactly 2^32 difference: the buggy subtraction-cast truncates to zero.
 * This must NOT return 0 (equal). */
int test_slotStatForSortAscCmpExactly2Pow32(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {0, 4294967296ULL};
    slotStatForSort b = {1, 0};
    TEST_ASSERT(testOnlySlotStatForSortAscCmp(&a, &b) > 0);
    TEST_ASSERT(testOnlySlotStatForSortAscCmp(&b, &a) < 0);

    return 0;
}

int test_slotStatForSortDescCmpExactly2Pow32(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {0, 4294967296ULL};
    slotStatForSort b = {1, 0};
    TEST_ASSERT(testOnlySlotStatForSortDescCmp(&a, &b) < 0);
    TEST_ASSERT(testOnlySlotStatForSortDescCmp(&b, &a) > 0);

    return 0;
}

/* Tie-break: equal stat values order by ascending slot in both comparators. */
int test_slotStatForSortAscCmpTieBreakBySlot(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {5, 100};
    slotStatForSort b = {10, 100};
    TEST_ASSERT(testOnlySlotStatForSortAscCmp(&a, &b) < 0);
    TEST_ASSERT(testOnlySlotStatForSortAscCmp(&b, &a) > 0);

    return 0;
}

int test_slotStatForSortDescCmpTieBreakBySlot(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {5, 100};
    slotStatForSort b = {10, 100};
    TEST_ASSERT(testOnlySlotStatForSortDescCmp(&a, &b) < 0);
    TEST_ASSERT(testOnlySlotStatForSortDescCmp(&b, &a) > 0);

    return 0;
}

/* Equal entries: same slot and same stat must return zero. */
int test_slotStatForSortAscCmpEqual(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {7, 42};
    slotStatForSort b = {7, 42};
    TEST_ASSERT(testOnlySlotStatForSortAscCmp(&a, &b) == 0);

    return 0;
}

int test_slotStatForSortDescCmpEqual(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    slotStatForSort a = {7, 42};
    slotStatForSort b = {7, 42};
    TEST_ASSERT(testOnlySlotStatForSortDescCmp(&a, &b) == 0);

    return 0;
}
