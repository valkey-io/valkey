/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

TEST_F(ZmallocTest, TestZmallocCacheAlignedAllocAndFree) {
    size_t used_memory_before = zmalloc_used_memory();
    void *ptr = zmalloc_cache_aligned(123);
    uintptr_t alignment;
    size_t usable_size;

    ASSERT_NE(ptr, nullptr);
    alignment = (uintptr_t)ptr % CACHE_LINE_SIZE;
    usable_size = zmalloc_usable_size(ptr);

    zfree(ptr);

    ASSERT_EQ(alignment, 0u);
    ASSERT_GE(usable_size, 123u);
    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

/* Sweep a range of alignments and sizes through one of the aligned allocators.
 * 'name' only makes a failure say which entry point was being exercised. */
static void testAlignedAllocSweep(void *(*alloc)(size_t, size_t), const char *name) {
    static const size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 4096};
    static const size_t sizes[] = {1, 7, 123, 1024, 8193};
    size_t used_memory_before = zmalloc_used_memory();
    size_t i, j;

    for (i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
        for (j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
            size_t alignment = alignments[i];
            size_t size = sizes[j];
            void *ptr = alloc(alignment, size);

            ASSERT_NE(ptr, nullptr) << name << " alignment " << alignment << " size " << size;
            ASSERT_EQ((uintptr_t)ptr % alignment, 0u) << name << " alignment " << alignment << " size " << size;
            ASSERT_GE(zmalloc_usable_size(ptr), size) << name << " alignment " << alignment << " size " << size;

            /* The whole requested range must be writable. */
            memset(ptr, 0xa5, size);

            zfree(ptr);
        }
    }

    ASSERT_EQ(zmalloc_used_memory(), used_memory_before) << name;
}

/* Check that an allocator rejects alignments that are zero or not a power of
 * two, without disturbing the memory accounting. */
static void testAlignedAllocRejectsBadAlignment(void *(*alloc)(size_t, size_t), const char *name) {
    static const size_t alignments[] = {0, 3, 6, 24, 100, 1000};
    size_t used_memory_before = zmalloc_used_memory();
    size_t i;

    for (i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
        errno = 0;
        ASSERT_EQ(alloc(alignments[i], 128), nullptr) << name << " alignment " << alignments[i];
        ASSERT_EQ(errno, EINVAL) << name << " alignment " << alignments[i];
    }

    ASSERT_EQ(zmalloc_used_memory(), used_memory_before) << name;
}

TEST_F(ZmallocTest, TestZalignedAllocAndFree) {
    testAlignedAllocSweep(zaligned_alloc, "zaligned_alloc");
}

TEST_F(ZmallocTest, TestZtryalignedAllocAndFree) {
    testAlignedAllocSweep(ztryaligned_alloc, "ztryaligned_alloc");
}

TEST_F(ZmallocTest, TestZalignedAllocIsAccounted) {
    size_t used_memory_before = zmalloc_used_memory();
    void *ptr = zaligned_alloc(4096, 1024 * 1024);

    ASSERT_NE(ptr, nullptr);
    ASSERT_GE(zmalloc_used_memory() - used_memory_before, 1024u * 1024u);

    zfree(ptr);

    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

TEST_F(ZmallocTest, TestZalignedAllocUsable) {
    size_t used_memory_before = zmalloc_used_memory();
    size_t usable = 0;
    void *ptr = zaligned_alloc_usable(64, 123, &usable);

    ASSERT_NE(ptr, nullptr);
    ASSERT_EQ((uintptr_t)ptr % 64, 0u);
    ASSERT_GE(usable, 123u);

    /* The reported usable size must really be usable. */
    memset(ptr, 0x5a, usable);

    zfree(ptr);

    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

TEST_F(ZmallocTest, TestZalignedAllocZeroByte) {
    size_t used_memory_before = zmalloc_used_memory();
    void *ptr = zaligned_alloc(64, 0);

    ASSERT_NE(ptr, nullptr);
    ASSERT_EQ((uintptr_t)ptr % 64, 0u);

    zfree(ptr);

    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

TEST_F(ZmallocTest, TestZalignedAllocInvalidAlignment) {
    testAlignedAllocRejectsBadAlignment(zaligned_alloc, "zaligned_alloc");
}

TEST_F(ZmallocTest, TestZtryalignedAllocInvalidAlignment) {
    testAlignedAllocRejectsBadAlignment(ztryaligned_alloc, "ztryaligned_alloc");
}

TEST_F(ZmallocTest, TestZtryalignedAllocUsable) {
    size_t used_memory_before = zmalloc_used_memory();
    size_t usable = 0;
    void *ptr = ztryaligned_alloc_usable(128, 4096, &usable);

    ASSERT_NE(ptr, nullptr);
    ASSERT_EQ((uintptr_t)ptr % 128, 0u);
    ASSERT_GE(usable, 4096u);

    /* The reported usable size must really be usable. */
    memset(ptr, 0x5a, usable);

    zfree(ptr);

    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}

/* An allocation too large to ever succeed must come back as NULL rather than
 * reach the OOM handler, which is the whole point of the 'try' variant. */
TEST_F(ZmallocTest, TestZtryalignedAllocHugeSizeReturnsNull) {
    size_t used_memory_before = zmalloc_used_memory();
    /* Route the size through a volatile so that the compiler doesn't warn about
     * an allocation larger than the maximum object size. */
    volatile size_t huge = SIZE_MAX / 2;

    ASSERT_EQ(ztryaligned_alloc(64, huge), nullptr);
    huge = SIZE_MAX;
    ASSERT_EQ(ztryaligned_alloc(64, huge), nullptr);

    ASSERT_EQ(zmalloc_used_memory(), used_memory_before);
}
