/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

extern "C" {
#include "ae.h"
}

static long long aeTestTimerProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    (void)eventLoop;
    (void)id;
    (void)clientData;
    return AE_NOMORE;
}

static void aeTestFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask) {
    (void)eventLoop;
    (void)fd;
    (void)clientData;
    (void)mask;
}

static void aeTestCountFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask) {
    (void)eventLoop;
    (void)fd;
    (void)mask;
    (*(int *)clientData)++;
}

typedef struct aeTestGrowData {
    int fd;
    int result;
    int calls;
    int high_fd_calls;
} aeTestGrowData;

static void aeTestGrowFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask) {
    (void)fd;
    (void)mask;
    aeTestGrowData *data = (aeTestGrowData *)clientData;
    data->calls++;
    data->result = aeCreateFileEvent(eventLoop, data->fd, AE_READABLE, aeTestCountFileProc, &data->high_fd_calls);
}

class AeTest : public ::testing::Test {
  protected:
    aeEventLoop *loop = nullptr;
    int pipefds[2] = {-1, -1};
    int highfd = -1;

    void SetUp() override {
        loop = aeCreateEventLoop(64);
        ASSERT_NE(loop, nullptr);
        /* A file event that is always ready lets aeProcessEvents() return
         * immediately from aeApiPoll when we exercise the !AE_DONT_WAIT path
         * (which is the path that calls usUntilEarliestTimer internally). */
        ASSERT_EQ(pipe(pipefds), 0);
        ASSERT_EQ(write(pipefds[1], "x", 1), 1);
        ASSERT_EQ(aeCreateFileEvent(loop, pipefds[0], AE_READABLE, aeTestFileProc, NULL), AE_OK);
    }
    void TearDown() override {
        aeDeleteEventLoop(loop);
        if (highfd >= 0) close(highfd);
        if (pipefds[0] >= 0) close(pipefds[0]);
        if (pipefds[1] >= 0) close(pipefds[1]);
    }
};

TEST_F(AeTest, HighFileDescriptorGrowsProtectedLoopAndPreservesHandlers) {
    int existing_calls = 0, high_fd_calls = 0;
    aeSetPollProtect(loop, 1);
    ASSERT_EQ(aeCreateFileEvent(loop, pipefds[0], AE_READABLE, aeTestCountFileProc, &existing_calls), AE_OK);
    highfd = fcntl(pipefds[0], F_DUPFD, 128);
    ASSERT_GE(highfd, 128);
    ASSERT_EQ(aeCreateFileEvent(loop, highfd, AE_READABLE, aeTestCountFileProc, &high_fd_calls), AE_OK);
    EXPECT_GT(aeGetSetSize(loop), highfd);
    aeProcessEvents(loop, AE_FILE_EVENTS | AE_DONT_WAIT);
    EXPECT_EQ(existing_calls, 1);
    EXPECT_EQ(high_fd_calls, 1);
}

TEST_F(AeTest, FileCallbackCanRegisterHighFileDescriptor) {
    highfd = fcntl(pipefds[0], F_DUPFD, 128);
    ASSERT_GE(highfd, 128);
    aeTestGrowData data = {highfd, AE_ERR, 0, 0};
    ASSERT_EQ(aeCreateFileEvent(loop, pipefds[0], AE_READABLE, aeTestGrowFileProc, &data), AE_OK);
    aeProcessEvents(loop, AE_FILE_EVENTS | AE_DONT_WAIT);
    ASSERT_EQ(data.result, AE_OK);
    EXPECT_EQ(data.calls, 1);
    aeProcessEvents(loop, AE_FILE_EVENTS | AE_DONT_WAIT);
    EXPECT_EQ(data.calls, 2);
    EXPECT_EQ(data.high_fd_calls, 1);
}

TEST_F(AeTest, PriorityLoopInitializationAndFileEventsCanGrow) {
    if (!strcmp(aeGetApiName(), "select") || !strcmp(aeGetApiName(), "evport")) {
        GTEST_SKIP() << "Priority event loops are unsupported by this polling backend";
    }
    aeDeleteFileEvent(loop, pipefds[0], AE_READABLE);
    ASSERT_EQ(aeResizeSetSize(loop, 1), AE_OK);
    ASSERT_EQ(aeActuateQoSEventLoopIfSupported(loop, 1, NULL), AE_OK);
    EXPECT_GT(aeGetSetSize(loop), loop->priority_fd);
    int high_fd_calls = 0;
    highfd = fcntl(pipefds[0], F_DUPFD, 128);
    ASSERT_GE(highfd, 128);
    ASSERT_EQ(aeCreateFileEvent(loop, highfd, AE_READABLE | AE_HIGH_PRIORITY, aeTestCountFileProc, &high_fd_calls), AE_OK);
    aeProcessEvents(loop, AE_FILE_EVENTS | AE_DONT_WAIT);
    EXPECT_EQ(high_fd_calls, 1);
}

TEST_F(AeTest, SelectGrowthRespectsBackendLimit) {
    if (strcmp(aeGetApiName(), "select")) GTEST_SKIP() << "Requires the select backend";
    highfd = fcntl(pipefds[0], F_DUPFD, FD_SETSIZE / 2);
    if (highfd == -1) GTEST_SKIP() << "Process file descriptor limit is too low";
    ASSERT_LT(highfd, FD_SETSIZE - 1);
    int high_fd_calls = 0;
    ASSERT_EQ(aeCreateFileEvent(loop, highfd, AE_READABLE, aeTestCountFileProc, &high_fd_calls), AE_OK);
    EXPECT_EQ(aeGetSetSize(loop), highfd + 1);

    EXPECT_EQ(aeCreateFileEvent(loop, FD_SETSIZE, AE_READABLE, aeTestFileProc, NULL), AE_ERR);
    EXPECT_EQ(errno, ERANGE);
    EXPECT_EQ(aeGetSetSize(loop), highfd + 1);
    aeProcessEvents(loop, AE_FILE_EVENTS | AE_DONT_WAIT);
    EXPECT_EQ(high_fd_calls, 1);
}

TEST_F(AeTest, InvalidFileDescriptorsDoNotGrowLoop) {
    int setsize = aeGetSetSize(loop);
    EXPECT_EQ(aeCreateFileEvent(loop, -1, AE_READABLE, aeTestFileProc, NULL), AE_ERR);
    EXPECT_EQ(errno, ERANGE);
    EXPECT_EQ(aeCreateFileEvent(loop, INT_MAX, AE_READABLE, aeTestFileProc, NULL), AE_ERR);
    EXPECT_EQ(errno, ERANGE);
    EXPECT_EQ(aeGetSetSize(loop), setsize);
}

TEST_F(AeTest, EarliestTimerTracksSoonerCreate) {
    long long id1 = aeCreateTimeEvent(loop, 10000, aeTestTimerProc, NULL, NULL);
    long long id2 = aeCreateTimeEvent(loop, 5000, aeTestTimerProc, NULL, NULL);
    long long id3 = aeCreateTimeEvent(loop, 20000, aeTestTimerProc, NULL, NULL);
    /* Creates on an unknown (NULL) cache don't fill it: the wait computation
     * inside aeProcessEvents rescans and finds the true earliest. */
    aeProcessEvents(loop, AE_ALL_EVENTS);
    ASSERT_NE(loop->earliestTimer, nullptr);
    ASSERT_EQ(loop->earliestTimer->id, id2);
    (void)id1;
    (void)id3;
}

TEST_F(AeTest, ValidCacheTracksOnlySoonerCreate) {
    long long id1 = aeCreateTimeEvent(loop, 60000, aeTestTimerProc, NULL, NULL);
    aeProcessEvents(loop, AE_ALL_EVENTS); /* rescan fills a valid cache */
    ASSERT_NE(loop->earliestTimer, nullptr);
    ASSERT_EQ(loop->earliestTimer->id, id1);
    /* Later create keeps the valid cache... */
    long long id2 = aeCreateTimeEvent(loop, 120000, aeTestTimerProc, NULL, NULL);
    ASSERT_EQ(loop->earliestTimer->id, id1);
    /* ...and a sooner create replaces it immediately, no rescan needed. */
    long long id3 = aeCreateTimeEvent(loop, 5000, aeTestTimerProc, NULL, NULL);
    ASSERT_EQ(loop->earliestTimer->id, id3);
    (void)id2;
}

TEST_F(AeTest, CreateWhileCacheInvalidKeepsTrueEarliest) {
    /* Regression: a timer created while the cache is invalidated must NOT be
     * adopted as earliest if an existing live timer fires sooner — otherwise
     * the next wait overshoots the real deadline. */
    long long id1 = aeCreateTimeEvent(loop, 5000, aeTestTimerProc, NULL, NULL);
    long long id2 = aeCreateTimeEvent(loop, 60000, aeTestTimerProc, NULL, NULL);
    aeProcessEvents(loop, AE_ALL_EVENTS); /* cache := id1 */
    ASSERT_EQ(loop->earliestTimer->id, id1);
    aeDeleteTimeEvent(loop, id1); /* cache invalidated */
    ASSERT_EQ(loop->earliestTimer, nullptr);
    /* Create a timer much later than the still-live id2: it must NOT be
     * adopted; the next lookup rescans and keeps id2 (60s), not id3 (120s). */
    long long id3 = aeCreateTimeEvent(loop, 120000, aeTestTimerProc, NULL, NULL);
    aeProcessEvents(loop, AE_ALL_EVENTS);
    ASSERT_NE(loop->earliestTimer, nullptr);
    ASSERT_EQ(loop->earliestTimer->id, id2);
    (void)id3;
}

TEST_F(AeTest, EarliestTimerInvalidatedOnDeleteThenRescanned) {
    long long id1 = aeCreateTimeEvent(loop, 5000, aeTestTimerProc, NULL, NULL);
    long long id2 = aeCreateTimeEvent(loop, 9000, aeTestTimerProc, NULL, NULL);
    /* Creates don't fill an unknown cache; the wait computation rescans. */
    aeProcessEvents(loop, AE_ALL_EVENTS);
    ASSERT_NE(loop->earliestTimer, nullptr);
    ASSERT_EQ(loop->earliestTimer->id, id1);
    aeDeleteTimeEvent(loop, id1);
    ASSERT_EQ(loop->earliestTimer, nullptr);
    aeProcessEvents(loop, AE_ALL_EVENTS);
    ASSERT_NE(loop->earliestTimer, nullptr);
    ASSERT_EQ(loop->earliestTimer->id, id2);
}

TEST_F(AeTest, CacheNotStaleAfterEarliestFires) {
    long long id1 = aeCreateTimeEvent(loop, 0, aeTestTimerProc, NULL, NULL);
    long long id2 = aeCreateTimeEvent(loop, 60000, aeTestTimerProc, NULL, NULL);
    /* First processing: the rescan caches id1, then id1 fires (due,
     * AE_NOMORE) and invalidates it. Second: the rescan must find id2 —
     * the stale id1 must never be served again. */
    aeProcessEvents(loop, AE_ALL_EVENTS);
    aeProcessEvents(loop, AE_ALL_EVENTS);
    ASSERT_NE(loop->earliestTimer, nullptr);
    ASSERT_EQ(loop->earliestTimer->id, id2);
    (void)id1;
}
