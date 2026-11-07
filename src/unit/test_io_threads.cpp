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

/* A replica's upstream primary connection must never be dispatched to an
 * I/O worker, even when the worker pool is active. */
TEST(IOThreadsTest, PrimaryLinkReadIsNotOffloaded) {
    client primary = {};
    connection conn = {};

    primary.conn = &conn;
    primary.flag.primary = 1;
    primary.io_read_state = CLIENT_IDLE;
    primary.io_write_state = CLIENT_IDLE;

    const int original_active_threads = server.active_io_threads_num;
    server.active_io_threads_num = 4;

    const int client_type = getClientType(&primary);
    const int result = trySendReadToIOThreads(&primary);
    const int read_state = primary.io_read_state;
    const int write_state = primary.io_write_state;
    const int pending_read = primary.flag.pending_read;

    server.active_io_threads_num = original_active_threads;

    EXPECT_EQ(CLIENT_TYPE_PRIMARY, client_type);
    EXPECT_EQ(C_ERR, result);
    EXPECT_EQ(CLIENT_IDLE, read_state);
    EXPECT_EQ(CLIENT_IDLE, write_state);
    EXPECT_EQ(0, pending_read);
}
