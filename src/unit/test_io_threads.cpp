/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <stdint.h>

extern "C" {
#include "io_threads.h"
#include "server.h"
}

/* Each IO thread writes its own counters on every busy loop iteration. These tests
 * check that no two threads' counters, and no main-thread state, share a cache line. */
static uintptr_t addr(const void *p) {
    return (uintptr_t)p;
}

static uintptr_t cacheLine(const void *p) {
    return addr(p) / CACHE_LINE_SIZE;
}

TEST(IOThreadsTest, ActiveTimeSlotsAreCacheLineAligned) {
    for (int id = 0; id < IO_THREADS_MAX_NUM; id++) {
        ASSERT_EQ(addr(testOnlyGetIOThreadActiveTimeSlot(id)) % CACHE_LINE_SIZE, 0u) << "slot " << id;
    }
}

TEST(IOThreadsTest, ActiveTimeSlotsDoNotShareCacheLines) {
    for (int id = 1; id < IO_THREADS_MAX_NUM; id++) {
        ASSERT_EQ(addr(testOnlyGetIOThreadActiveTimeSlot(id)) - addr(testOnlyGetIOThreadActiveTimeSlot(id - 1)),
                  (uintptr_t)CACHE_LINE_SIZE)
            << "slots " << id - 1 << " and " << id;
    }
}

TEST(IOThreadsTest, JobsFinishedCounterHasItsOwnCacheLine) {
    const void *finished = testOnlyGetIOJobsFinishedCounter();
    ASSERT_EQ(addr(finished) % CACHE_LINE_SIZE, 0u);
    ASSERT_NE(cacheLine(finished), cacheLine(testOnlyGetIOJobsSubmittedCounter()));
    for (int id = 0; id < IO_THREADS_MAX_NUM; id++) {
        ASSERT_NE(cacheLine(finished), cacheLine(testOnlyGetIOThreadActiveTimeSlot(id))) << "slot " << id;
    }
}
