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
void beforeSleep(aeEventLoop *eventLoop);
void freePrefetchCommandsBatch(void);
extern int ProcessingEventsWhileBlocked;
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

static bool second_batch_client_processed;

static int processSecondBatchClient(client *c) {
    (void)c;
    second_batch_client_processed = true;
    return C_OK;
}

static int processFirstBatchClient(client *c) {
    (void)c;
    ProcessingEventsWhileBlocked = 1;
    beforeSleep(server.el);
    ProcessingEventsWhileBlocked = 0;
    EXPECT_TRUE(second_batch_client_processed);
    return C_OK;
}

TEST(IOThreadsTest, BusyEventLoopProcessesRemainingCommandBatch) {
    MockValkey mock;
    client first = {};
    client second = {};
    memset(&server, 0, sizeof(server));
    server.prefetch_batch_max_size = 16;
    server.clients_to_close = listCreate();
    server.clients_pending_write = listCreate();
    server.el = aeCreateEventLoop(64);
    testOnlyInitIOThreadQueues();
    prefetchCommandsBatchInit();
    ASSERT_EQ(addCommandToBatchAndProcessIfFull(&first), C_OK);
    ASSERT_EQ(addCommandToBatchAndProcessIfFull(&second), C_OK);

    second_batch_client_processed = false;
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(&second)).WillOnce(testing::Invoke(processSecondBatchClient));
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(&first)).WillOnce(testing::Invoke(processFirstBatchClient));
    EXPECT_CALL(mock, beforeNextClient(testing::_)).Times(2);
    processClientsCommandsBatch();

    freePrefetchCommandsBatch();
    testOnlyFreeIOThreadQueues();
    aeDeleteEventLoop(server.el);
    listRelease(server.clients_to_close);
    listRelease(server.clients_pending_write);
}
