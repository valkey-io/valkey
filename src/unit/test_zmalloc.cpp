/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdio>

extern "C" {
#include "zmalloc.h"
}

class ZmallocTest : public ::testing::Test {};

TEST_F(ZmallocTest, TestZmallocAllocReallocCallocAndFree) {
    size_t used_memory_before = zmalloc_used_memory();
    void *ptr, *ptr2;

    ptr = zmalloc(123);
    printf("Allocated 123 bytes; used: %lld\n",
           (long long)zmalloc_used_memory() - used_memory_before);

    ptr = zrealloc(ptr, 456);
    printf("Reallocated to 456 bytes; used: %lld\n",
           (long long)zmalloc_used_memory() - used_memory_before);

    ptr2 = zcalloc(123);
    printf("Callocated 123 bytes; used: %lld\n",
           (long long)zmalloc_used_memory() - used_memory_before);

    zfree(ptr);
    zfree(ptr2);
    printf("Freed pointers; used: %lld\n",
           (long long)zmalloc_used_memory() - used_memory_before);

    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

TEST_F(ZmallocTest, TestZmallocAllocZeroByteAndFree) {
    size_t used_memory_before = zmalloc_used_memory();
    void *ptr;

    ptr = zmalloc(0);
    printf("Allocated 0 bytes; used: %zu\n", zmalloc_used_memory());
    zfree(ptr);

    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}
