/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif

#define _Static_assert static_assert
extern "C" {
#include "ae.h"
#include "connection.h"
#include "io_threads.h"
#include "monotonic.h"
#include "server.h"
}
#undef _Static_assert


/* Static tracking for preemption test callbacks */
static int g_execution_order[1024];
static int g_execution_count = 0;

static monotime g_mock_time_offset = 0;
static monotime (*g_orig_getMonotonicUs)(void) = NULL;
static monotime mockGetMonotonicUs(void) {
    return g_orig_getMonotonicUs() + g_mock_time_offset;
}

static void setupMockClock(void) {
    if (g_orig_getMonotonicUs != NULL) return;
    g_orig_getMonotonicUs = getMonotonicUs;
    getMonotonicUs = mockGetMonotonicUs;
    g_mock_time_offset = 0;
}

static void restoreMockClock(void) {
    if (g_orig_getMonotonicUs) {
        getMonotonicUs = g_orig_getMonotonicUs;
        g_orig_getMonotonicUs = NULL;
    }
}

static void advanceMockTime(uint64_t us) {
    g_mock_time_offset += us;
}

static void testNormalEventCallback(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);
    if (g_execution_count < 1024) {
        g_execution_order[g_execution_count++] = fd;
    }
    /* Read 1 byte from pipe to clear the event */
    char buf[1];
    if (read(fd, buf, 1) < 0) {
        /* Ignore read error */
    }
}

static void testHighPriorityEventCallback(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);
    if (g_execution_count < 1024) {
        /* Use 999 to identify high priority execution */
        g_execution_order[g_execution_count++] = 999;
    }
    char buf[1];
    if (read(fd, buf, 1) < 0) {
        /* Ignore read error */
    }
}

static int g_hp_pipe2_write_fd = -1;
static int g_normal_cb_count = 0;
static void testNormalEventCallbackMaskCheck(aeEventLoop *el, int fd, void *privdata, int mask) {
    testNormalEventCallback(el, fd, privdata, mask);
    g_normal_cb_count++;
    /* Sleep > 1000 us on 3rd normal event (j = 2) and trigger 2nd HP pipe so (itr_cnt & 3) == 0 check at j = 4 polls hp_el */
    if (g_normal_cb_count == 3 && g_hp_pipe2_write_fd != -1) {
        usleep(1500);
        char c = 'x';
        if (write(g_hp_pipe2_write_fd, &c, 1) < 0) {
        }
    }
}

static void testLevelTriggeredCallback(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);
    if (privdata) {
        int *serve_counts = (int *)privdata;
        serve_counts[0]++;
    }
    if (g_execution_count < 1024) {
        g_execution_order[g_execution_count++] = fd;
    }
    /* Read only 1 byte per execution to test level-triggered behavior (if more data in buffer, fd remains readable) */
    char buf[1];
    if (read(fd, buf, 1) < 0) {
        /* Ignore read error */
    }
}

static void dummyConnectionHandler(struct connection *conn) {
    UNUSED(conn);
}

class SocketPrioritizationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        monotonicInit();
        setupMockClock();
        /* Initialize minimal server fields needed to prevent crashes in logging/connection layer */
        server.logfile = strdup("");
        server.syslog_enabled = 0;

        /* Initialize connection types registry ONCE */
        static bool conn_types_initialized = false;
        if (!conn_types_initialized) {
            connTypeInitialize();
            conn_types_initialized = true;
        }

        server.el = aeCreateEventLoop(1024);
        aeEventLoop *hp_el = aeCreateEventLoop(1024);
        if (server.el && hp_el) {
            aeLinkHighPriorityEventLoop(server.el, hp_el);
        }
        g_execution_count = 0;
    }

    void TearDown() override {
        if (server.el) {
            aeDeleteEventLoop(server.el);
            server.el = NULL;
        }
        if (server.logfile) {
            free(server.logfile);
            server.logfile = NULL;
        }
        restoreMockClock();
    }
};

class SocketPrioritizationConnTest : public SocketPrioritizationTest, public ::testing::WithParamInterface<int> {};

TEST_F(SocketPrioritizationTest, EventLoopDualInitialization) {
    ASSERT_NE(server.el, (aeEventLoop *)NULL);
    ASSERT_NE(server.el->hp_event_loop, (aeEventLoop *)NULL);
    EXPECT_EQ(server.el->hp_event_loop, server.el->hp_event_loop);
    EXPECT_EQ(server.el->hp_event_loop->hp_event_loop, (aeEventLoop *)NULL);
}

TEST_P(SocketPrioritizationConnTest, ConnectionPriorityMetadataAndHelpers) {
    ConnectionType *ct = connectionByType(GetParam());
    if (ct == NULL) return;
    connection *conn = connCreate(ct);
    ASSERT_NE(conn, (connection *)NULL);

    /* Default priority should be normal */
    EXPECT_EQ(connGetPriority(conn), CONN_PRIORITY_NORMAL);

    /* Update metadata when fd is not yet set (-1) */
    connSetPriority(conn, CONN_PRIORITY_HIGH);
    EXPECT_EQ(connGetPriority(conn), CONN_PRIORITY_HIGH);

    connSetPriority(conn, CONN_PRIORITY_NORMAL);
    EXPECT_EQ(connGetPriority(conn), CONN_PRIORITY_NORMAL);

    conn->state = CONN_STATE_NONE;
    connClose(conn);
}

TEST_P(SocketPrioritizationConnTest, DynamicPriorityUpdateOnActiveConnection) {
    ConnectionType *ct = connectionByType(GetParam());
    if (ct == NULL) return;
    int fds[2];
    int ret = pipe(fds);
    ASSERT_EQ(ret, 0);
    int read_fd = fds[0];
    int write_fd = fds[1];

    connection *conn = connCreate(ct);
    ASSERT_NE(conn, (connection *)NULL);
    conn->fd = read_fd;

    /* Set a read handler while priority is normal */
    ret = connSetReadHandler(conn, dummyConnectionHandler);
    EXPECT_EQ(ret, C_OK);

    /* Event should exist in normal loop, but not in high priority loop */
    EXPECT_NE(aeGetFileEvents(server.el, read_fd) & AE_READABLE, 0);
    EXPECT_EQ(aeGetFileEvents(server.el->hp_event_loop, read_fd) & AE_READABLE, 0);

    /* Dynamically upgrade connection to high priority */
    EXPECT_EQ(connSetPriority(conn, CONN_PRIORITY_HIGH), C_OK);
    EXPECT_EQ(connGetPriority(conn), CONN_PRIORITY_HIGH);
    int events = aeGetFileEvents(server.el, read_fd);
    EXPECT_NE(events & AE_READABLE, 0);
    EXPECT_NE(events & AE_HIGH_PRIORITY, 0);
    EXPECT_NE(aeGetFileEvents(server.el->hp_event_loop, read_fd) & AE_READABLE, 0);

    /* Dynamically downgrade connection back to normal priority */
    EXPECT_EQ(connSetPriority(conn, CONN_PRIORITY_NORMAL), C_OK);
    EXPECT_EQ(connGetPriority(conn), CONN_PRIORITY_NORMAL);
    EXPECT_NE(aeGetFileEvents(server.el, read_fd) & AE_READABLE, 0);
    EXPECT_EQ(aeGetFileEvents(server.el->hp_event_loop, read_fd) & AE_READABLE, 0);

    conn->state = CONN_STATE_NONE;
    connClose(conn);
    close(write_fd);
}

TEST_F(SocketPrioritizationTest, PreemptionOfNormalEventsByHighPriorityLoop) {
    int normal_pipes[2][2];
    int hp_pipe[2];
    int i;
    int ret;

    for (i = 0; i < 2; i++) {
        ret = pipe(normal_pipes[i]);
        ASSERT_EQ(ret, 0);
        /* Set non-blocking */
        fcntl(normal_pipes[i][0], F_SETFL, O_NONBLOCK);
        fcntl(normal_pipes[i][1], F_SETFL, O_NONBLOCK);
    }
    ret = pipe(hp_pipe);
    ASSERT_EQ(ret, 0);
    fcntl(hp_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(hp_pipe[1], F_SETFL, O_NONBLOCK);

    /* Register normal events on server.el */
    ret = aeCreateFileEvent(server.el, normal_pipes[0][0], AE_READABLE, testNormalEventCallback, NULL);
    EXPECT_EQ(ret, AE_OK);
    ret = aeCreateFileEvent(server.el, normal_pipes[1][0], AE_READABLE, testNormalEventCallback, NULL);
    EXPECT_EQ(ret, AE_OK);

    /* Register high priority event on server.el->hp_event_loop */
    ret = aeCreateFileEvent(server.el->hp_event_loop, hp_pipe[0], AE_READABLE, testHighPriorityEventCallback, NULL);
    EXPECT_EQ(ret, AE_OK);

    /* Write 1 byte to all pipes so they fire */
    char c = 'x';
    if (write(normal_pipes[0][1], &c, 1) < 0) {
    }
    if (write(normal_pipes[1][1], &c, 1) < 0) {
    }
    if (write(hp_pipe[1], &c, 1) < 0) {
    }

    /* Ensure more than HP_EVENTS_POLL_CHECK_EVERY_US (1000 us) has elapsed for hp_event_loop->last_poll_us */
    advanceMockTime(1000000);

    g_execution_count = 0;
    /* Process events on main event loop. At iteration j = 0, PROCESS_HP_EVENTS_IF_NEEDED will trigger
     * polling of server.el->hp_event_loop and execute testHighPriorityEventCallback before processing the normal events! */
    aeProcessEvents(server.el, AE_FILE_EVENTS | AE_DONT_WAIT);

    /* Verify that high priority event (id 999) executed first due to preemption */
    ASSERT_GE(g_execution_count, 1);
    EXPECT_EQ(g_execution_order[0], 999);

    /* Cleanup events and file descriptors */
    aeDeleteFileEvent(server.el, normal_pipes[0][0], AE_READABLE);
    aeDeleteFileEvent(server.el, normal_pipes[1][0], AE_READABLE);
    aeDeleteFileEvent(server.el->hp_event_loop, hp_pipe[0], AE_READABLE);

    for (i = 0; i < 2; i++) {
        close(normal_pipes[i][0]);
        close(normal_pipes[i][1]);
    }
    close(hp_pipe[0]);
    close(hp_pipe[1]);
}

TEST_F(SocketPrioritizationTest, QoSNames) {
    EXPECT_STREQ(getConnectionPriorityName(CONN_PRIORITY_NORMAL), "normal");
    EXPECT_STREQ(getConnectionPriorityName(CONN_PRIORITY_HIGH), "prioritized");
}

static void testHighPriorityWriteCallback(struct connection *conn) {
    if (g_execution_count < 1024) {
        g_execution_order[g_execution_count++] = 999;
    }
    connSetWriteHandler(conn, NULL);
}

TEST_P(SocketPrioritizationConnTest, WriteHandlerPriorityPreemption) {
    ConnectionType *ct = connectionByType(GetParam());
    if (ct == NULL) return;
    int normal_pipe[2];
    int hp_pipe[2];
    int ret;

    ret = pipe(normal_pipe);
    ASSERT_EQ(ret, 0);
    fcntl(normal_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(normal_pipe[1], F_SETFL, O_NONBLOCK);

    ret = pipe(hp_pipe);
    ASSERT_EQ(ret, 0);
    fcntl(hp_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(hp_pipe[1], F_SETFL, O_NONBLOCK);

    connection *hp_conn = connCreate(ct);
    ASSERT_NE(hp_conn, (connection *)NULL);
    hp_conn->state = CONN_STATE_CONNECTED;
    hp_conn->fd = hp_pipe[1];
    connSetPriority(hp_conn, CONN_PRIORITY_HIGH);

    /* Register normal read event on server.el */
    ret = aeCreateFileEvent(server.el, normal_pipe[0], AE_READABLE, testNormalEventCallback, NULL);
    EXPECT_EQ(ret, AE_OK);

    /* Register high priority write event on server.el->hp_event_loop via connection wrapper */
    ret = connSetWriteHandler(hp_conn, testHighPriorityWriteCallback);
    EXPECT_EQ(ret, C_OK);

    char c = 'w';
    if (write(normal_pipe[1], &c, 1) < 0) {
    }

    advanceMockTime(1000000);
    g_execution_count = 0;
    aeProcessEvents(server.el, AE_FILE_EVENTS | AE_DONT_WAIT);

    ASSERT_GE(g_execution_count, 1);
    EXPECT_EQ(g_execution_order[0], 999);

    aeDeleteFileEvent(server.el, normal_pipe[0], AE_READABLE);
    hp_conn->state = CONN_STATE_NONE;
    connClose(hp_conn);
    close(normal_pipe[0]);
    close(normal_pipe[1]);
    close(hp_pipe[0]);
}

TEST_F(SocketPrioritizationTest, MultiIterationPreemptionCheckMask) {
    /* Verify that PROCESS_HP_EVENTS_IF_NEEDED checks hp_el on iterations where (itr_cnt & 3) == 0 */
    int normal_pipes[5][2];
    int hp_pipe[2];
    int hp_pipe2[2];
    int i, ret;

    for (i = 0; i < 5; i++) {
        ret = pipe(normal_pipes[i]);
        ASSERT_EQ(ret, 0);
        fcntl(normal_pipes[i][0], F_SETFL, O_NONBLOCK);
        fcntl(normal_pipes[i][1], F_SETFL, O_NONBLOCK);
    }
    ret = pipe(hp_pipe);
    ASSERT_EQ(ret, 0);
    fcntl(hp_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(hp_pipe[1], F_SETFL, O_NONBLOCK);

    ret = pipe(hp_pipe2);
    ASSERT_EQ(ret, 0);
    fcntl(hp_pipe2[0], F_SETFL, O_NONBLOCK);
    fcntl(hp_pipe2[1], F_SETFL, O_NONBLOCK);

    for (i = 0; i < 5; i++) {
        ret = aeCreateFileEvent(server.el, normal_pipes[i][0], AE_READABLE, testNormalEventCallbackMaskCheck, NULL);
        EXPECT_EQ(ret, AE_OK);
    }
    ret = aeCreateFileEvent(server.el->hp_event_loop, hp_pipe[0], AE_READABLE, testHighPriorityEventCallback, NULL);
    EXPECT_EQ(ret, AE_OK);
    ret = aeCreateFileEvent(server.el->hp_event_loop, hp_pipe2[0], AE_READABLE, testHighPriorityEventCallback, NULL);
    EXPECT_EQ(ret, AE_OK);

    char c = 'z';
    for (i = 0; i < 5; i++) {
        if (write(normal_pipes[i][1], &c, 1) < 0) {
        }
    }
    if (write(hp_pipe[1], &c, 1) < 0) {
    }

    g_hp_pipe2_write_fd = hp_pipe2[1];
    g_normal_cb_count = 0;
    advanceMockTime(1000000);
    g_execution_count = 0;

    aeProcessEvents(server.el, AE_FILE_EVENTS | AE_DONT_WAIT);

    ASSERT_EQ(g_execution_count, 7);
    /* At start (pre-loop), hp_event_loop processed */
    EXPECT_EQ(g_execution_order[0], 999);
    EXPECT_NE(g_execution_order[1], 999);
    EXPECT_NE(g_execution_order[2], 999);
    EXPECT_NE(g_execution_order[3], 999);
    EXPECT_NE(g_execution_order[4], 999);
    /* At iteration 4 ((j & 3) == 0 after >1000us elapsed), hp_event_loop is checked and serviced again on later masked iteration */
    EXPECT_EQ(g_execution_order[5], 999);
    EXPECT_NE(g_execution_order[6], 999);

    g_hp_pipe2_write_fd = -1;

    for (i = 0; i < 5; i++) {
        aeDeleteFileEvent(server.el, normal_pipes[i][0], AE_READABLE);
        close(normal_pipes[i][0]);
        close(normal_pipes[i][1]);
    }
    aeDeleteFileEvent(server.el->hp_event_loop, hp_pipe[0], AE_READABLE);
    close(hp_pipe[0]);
    close(hp_pipe[1]);
    aeDeleteFileEvent(server.el->hp_event_loop, hp_pipe2[0], AE_READABLE);
    close(hp_pipe2[0]);
    close(hp_pipe2[1]);
}

TEST_F(SocketPrioritizationTest, LevelTriggeredEventsFairnessAndOrdering) {
    const int NUM_FDS = 20;
    const int BYTES_PER_FD = 5;
    int pipes[NUM_FDS][2];
    int counts[NUM_FDS];
    int i, ret;

    g_execution_count = 0;
    for (i = 0; i < NUM_FDS; i++) {
        counts[i] = 0;
        ret = pipe(pipes[i]);
        ASSERT_EQ(ret, 0);
        fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);
        fcntl(pipes[i][1], F_SETFL, O_NONBLOCK);

        ret = aeCreateFileEvent(server.el->hp_event_loop, pipes[i][0], AE_READABLE, testLevelTriggeredCallback, &counts[i]);
        EXPECT_EQ(ret, AE_OK);

        char data[BYTES_PER_FD] = {'a', 'b', 'c', 'd', 'e'};
        if (write(pipes[i][1], data, BYTES_PER_FD) < 0) {
        }
    }

    /* Run iterations until all FDs drain their BYTES_PER_FD bytes */
    int max_iterations = NUM_FDS * BYTES_PER_FD + 50;
    for (int iter = 0; iter < max_iterations; iter++) {
        int total_served = 0;
        for (i = 0; i < NUM_FDS; i++) total_served += counts[i];
        if (total_served >= NUM_FDS * BYTES_PER_FD) break;

        aeProcessEvents(server.el->hp_event_loop, AE_FILE_EVENTS | AE_DONT_WAIT);
    }

    /* Verify all FDs were served exactly BYTES_PER_FD times without any level-triggered event being lost or starved */
    for (i = 0; i < NUM_FDS; i++) {
        EXPECT_EQ(counts[i], BYTES_PER_FD);
        aeDeleteFileEvent(server.el->hp_event_loop, pipes[i][0], AE_READABLE);
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

TEST_F(SocketPrioritizationTest, LevelTriggeredBatchProcessingAndStarvationPrevention) {
    const int NUM_FDS = 20;
    int pipes[NUM_FDS][2];
    int counts[NUM_FDS];
    int i, ret;

    g_execution_count = 0;
    for (i = 0; i < NUM_FDS; i++) {
        counts[i] = 0;
        ret = pipe(pipes[i]);
        ASSERT_EQ(ret, 0);
        fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);
        fcntl(pipes[i][1], F_SETFL, O_NONBLOCK);
    }

    /* 1. Register Head FDs (0..6) with 3 bytes (unprocessed at the head) and Tail FDs (14..19) with 5 bytes */
    for (i = 0; i <= 6; i++) {
        ret = aeCreateFileEvent(server.el->hp_event_loop, pipes[i][0], AE_READABLE, testLevelTriggeredCallback, &counts[i]);
        EXPECT_EQ(ret, AE_OK);
        char data[3] = {'h', 'e', 'a'};
        if (write(pipes[i][1], data, 3) < 0) {
        }
    }
    for (i = 14; i <= 19; i++) {
        ret = aeCreateFileEvent(server.el->hp_event_loop, pipes[i][0], AE_READABLE, testLevelTriggeredCallback, &counts[i]);
        EXPECT_EQ(ret, AE_OK);
        char data[5] = {'t', 'a', 'i', 'l', 's'};
        if (write(pipes[i][1], data, 5) < 0) {
        }
    }

    /* Process batch 1: Head and Tail FDs each fire once (reading 1 byte due to level trigger) */
    aeProcessEvents(server.el->hp_event_loop, AE_FILE_EVENTS | AE_DONT_WAIT);
    for (i = 0; i <= 6; i++) EXPECT_GE(counts[i], 1);
    for (i = 14; i <= 19; i++) EXPECT_GE(counts[i], 1);

    /* 2. Register Middle FDs (7..13) with 3 bytes (new events in the middle while head/tail have remaining level-triggered data) */
    for (i = 7; i <= 13; i++) {
        ret = aeCreateFileEvent(server.el->hp_event_loop, pipes[i][0], AE_READABLE, testLevelTriggeredCallback, &counts[i]);
        EXPECT_EQ(ret, AE_OK);
        char data[3] = {'m', 'i', 'd'};
        if (write(pipes[i][1], data, 3) < 0) {
        }
    }

    /* Process remaining batches until all buffers across all groups are completely drained */
    for (int iter = 0; iter < 200; iter++) {
        int total_served = 0;
        for (i = 0; i < NUM_FDS; i++) total_served += counts[i];
        /* 7 head FDs x 3 + 7 middle FDs x 3 + 6 tail FDs x 5 = 21 + 21 + 30 = 72 total bytes */
        if (total_served >= 72) break;

        aeProcessEvents(server.el->hp_event_loop, AE_FILE_EVENTS | AE_DONT_WAIT);
    }

    /* Verify every single FD across head (unprocessed), middle (new), and tail (level-triggered remaining data) was fully served */
    for (i = 0; i <= 6; i++) EXPECT_EQ(counts[i], 3);
    for (i = 7; i <= 13; i++) EXPECT_EQ(counts[i], 3);
    for (i = 14; i <= 19; i++) EXPECT_EQ(counts[i], 5);

    for (i = 0; i < NUM_FDS; i++) {
        aeDeleteFileEvent(server.el->hp_event_loop, pipes[i][0], AE_READABLE);
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

TEST_F(SocketPrioritizationTest, DualEventLoopResize) {
    ASSERT_NE(server.el, (aeEventLoop *)NULL);
    ASSERT_NE(server.el->hp_event_loop, (aeEventLoop *)NULL);
    EXPECT_EQ(aeGetSetSize(server.el), 1024);
    EXPECT_EQ(aeGetSetSize(server.el->hp_event_loop), 1024);

    int ret = aeResizeSetSize(server.el, 2048);
    EXPECT_EQ(ret, AE_OK);
    EXPECT_EQ(aeGetSetSize(server.el), 2048);
    EXPECT_EQ(aeGetSetSize(server.el->hp_event_loop), 2048);
}

TEST_F(SocketPrioritizationTest, FallbackMaskSanitization) {
    aeEventLoop *standalone_el = aeCreateEventLoop(64);
    ASSERT_NE(standalone_el, (aeEventLoop *)NULL);
    EXPECT_EQ(standalone_el->hp_event_loop, (aeEventLoop *)NULL);

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    /* Register with AE_HIGH_PRIORITY on standalone event loop with no hp_event_loop */
    int ret = aeCreateFileEvent(standalone_el, fds[0], AE_READABLE | AE_HIGH_PRIORITY, testNormalEventCallback, NULL);
    EXPECT_EQ(ret, AE_OK);

    /* Mask should only have AE_READABLE, and NOT retain AE_HIGH_PRIORITY (0x8) */
    int events = aeGetFileEvents(standalone_el, fds[0]);
    EXPECT_EQ(events, AE_READABLE);

    /* Delete the readable event */
    aeDeleteFileEvent(standalone_el, fds[0], AE_READABLE);
    EXPECT_EQ(aeGetFileEvents(standalone_el, fds[0]), AE_NONE);

    close(fds[0]);
    close(fds[1]);
    aeDeleteEventLoop(standalone_el);
}

TEST_F(SocketPrioritizationTest, PostponedStateDynamicPriorityUpdate) {
    ConnectionType *ct = connectionByType(CONN_TYPE_SOCKET);
    ASSERT_NE(ct, (ConnectionType *)NULL);

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    connection *conn = connCreate(ct);
    ASSERT_NE(conn, (connection *)NULL);
    conn->fd = fds[0];

    int ret = connSetReadHandler(conn, dummyConnectionHandler);
    EXPECT_EQ(ret, C_OK);
    EXPECT_EQ(connGetPriority(conn), CONN_PRIORITY_NORMAL);

    /* Set postponed state (e.g. while offloaded to IO threads) */
    conn->flags |= CONN_FLAG_POSTPONE_UPDATE_STATE;

    /* Upgrading priority while postponed must update priority field but defer event loop migration */
    ret = connSetPriority(conn, CONN_PRIORITY_HIGH);
    EXPECT_EQ(ret, C_OK);
    EXPECT_EQ(connGetPriority(conn), CONN_PRIORITY_HIGH);

    conn->flags &= ~CONN_FLAG_POSTPONE_UPDATE_STATE;
    conn->state = CONN_STATE_NONE;
    connClose(conn);
    close(fds[1]);
}

TEST_F(SocketPrioritizationTest, WriteBarrierPreservation) {
    ConnectionType *ct = connectionByType(CONN_TYPE_SOCKET);
    ASSERT_NE(ct, (ConnectionType *)NULL);

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    connection *conn = connCreate(ct);
    ASSERT_NE(conn, (connection *)NULL);
    conn->fd = fds[0];

    int ret = connSetWriteHandlerWithBarrier(conn, dummyConnectionHandler, 1);
    EXPECT_EQ(ret, C_OK);

    int events = aeGetFileEvents(server.el, fds[0]);
    EXPECT_NE(events & AE_WRITABLE, 0);

    /* Upgrade priority: verify migration succeeds with barrier */
    ret = connSetPriority(conn, CONN_PRIORITY_HIGH);
    EXPECT_EQ(ret, C_OK);
    EXPECT_EQ(connGetPriority(conn), CONN_PRIORITY_HIGH);

    conn->state = CONN_STATE_NONE;
    connClose(conn);
    close(fds[1]);
}

INSTANTIATE_TEST_SUITE_P(
    ConnTypes,
    SocketPrioritizationConnTest,
    ::testing::Values(CONN_TYPE_SOCKET, CONN_TYPE_TLS));

#ifdef __linux__
/* This test verifies the Linux kernel epoll starvation prevention behavior
 * as discussed in Valkey Issue #3927 (https://github.com/valkey-io/valkey/issues/3927#issuecomment-4664882443).
 *
 * When epoll_wait is called with a maxevents limit smaller than the number of
 * currently ready level-triggered FDs:
 * 1. The kernel moves all ready FDs to a temporary transfer list (txlist).
 * 2. It copies up to maxevents to userspace.
 * 3. The copied FDs (still ready in LT mode) are re-queued to the tail of the ready list (rdllist).
 * 4. The remaining unprocessed FDs in txlist are bulk re-spliced to the FRONT of rdllist (via ep_done_scan).
 *
 * This ensures that:
 * - Unprocessed FDs are prioritized over re-queued FDs in the next call.
 * - New events arriving later are not starved by persistent LT events, because
 *   eventually the older unreturned events are promoted to the front.
 *
 * We verify this by:
 * 1. Registering 5 pipes (A, B, C, D, E).
 * 2. Making A, B, C, D ready.
 * 3. Calling epoll_wait(limit=3) -> returns 3 (e.g., A, B, C). D is left out.
 * 4. Making E (new event) ready (appended to tail -> [D, A, B, C, E]).
 * 5. Calling epoll_wait(limit=3) -> must return D (promoted to front), but NOT E (still at tail).
 * 6. Calling epoll_wait(limit=3) -> must return E (now promoted to front because it was left in txlist).
 */
TEST(EpollFairnessTest, KernelBehavior) {
    const int NUM_PIPES = 5;
    int pipes[NUM_PIPES][2];
    int epfd = epoll_create1(0);
    ASSERT_NE(epfd, -1);

    // 1. Registering 5 pipes (A, B, C, D, E) to epoll (Level-Triggered).
    for (int i = 0; i < NUM_PIPES; i++) {
        ASSERT_EQ(pipe(pipes[i]), 0);
        fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);

        struct epoll_event ee = {0};
        ee.events = EPOLLIN;
        ee.data.fd = pipes[i][0];
        ASSERT_EQ(epoll_ctl(epfd, EPOLL_CTL_ADD, pipes[i][0], &ee), 0);
    }

    char c = 'x';
    // 2. Making A, B, C, D ready.
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(write(pipes[i][1], &c, 1), 1);
    }

    // 3. Calling epoll_wait(limit=3) -> returns 3 (e.g., A, B, C). D is left out.
    // The unreturned one (D) is moved to the FRONT of rdllist.
    // The returned ones (A, B, C) are re-queued to the tail.
    // rdllist is now [D, A, B, C].
    struct epoll_event events[3];
    int ready = epoll_wait(epfd, events, 3, 0);
    ASSERT_EQ(ready, 3);

    // Track which FD was left out in the first call (D).
    int left_out_fd = -1;
    for (int i = 0; i < 4; i++) {
        int fd = pipes[i][0];
        int found = 0;
        for (int j = 0; j < ready; j++) {
            if (events[j].data.fd == fd) {
                found = 1;
                break;
            }
        }
        if (!found) {
            left_out_fd = fd;
            break;
        }
    }
    ASSERT_NE(left_out_fd, -1);

    // 4. Making E (new event) ready (appended to tail -> [D, A, B, C, E]).
    ASSERT_EQ(write(pipes[4][1], &c, 1), 1);

    // 5. Calling epoll_wait(limit=3) -> must return D (promoted to front), but NOT E (still at tail).
    // Kernel moves [D, A, B, C, E] to txlist.
    // Copies D, A, B (limit 3 reached). Re-queues D, A, B.
    // C, E are left in txlist and moved to the FRONT of rdllist -> [C, E, D, A, B].
    ready = epoll_wait(epfd, events, 3, 0);
    ASSERT_EQ(ready, 3);

    // Verify that the left-out FD (D) WAS returned in this second call
    // (proving it was moved to the front and not starved by A, B, C).
    int found_left_out = 0;
    for (int i = 0; i < ready; i++) {
        if (events[i].data.fd == left_out_fd) {
            found_left_out = 1;
            break;
        }
    }
    EXPECT_TRUE(found_left_out) << "Left-out FD was starved!";

    // Verify that the new event (E, pipe 4) was NOT returned yet (it was at the tail).
    for (int i = 0; i < ready; i++) {
        EXPECT_NE(events[i].data.fd, pipes[4][0]) << "New event E should not be returned yet";
    }

    // 6. Calling epoll_wait(limit=3) -> must return E (now promoted to front because it was left in txlist).
    // rdllist was [C, E, D, A, B] due to C, E being moved to the front.
    // It should return C, E, and one of D, A, B.
    ready = epoll_wait(epfd, events, 3, 0);
    ASSERT_EQ(ready, 3);

    // Verify that the new event (E, pipe 4) IS returned now
    // (proving it was moved to the front in the previous step and not starved).
    int found_new_event = 0;
    for (int i = 0; i < ready; i++) {
        if (events[i].data.fd == pipes[4][0]) {
            found_new_event = 1;
            break;
        }
    }
    EXPECT_TRUE(found_new_event) << "New event E was starved by persistent LT events!";

    // Cleanup resources.
    for (int i = 0; i < NUM_PIPES; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    close(epfd);
}
#endif

#ifdef HAVE_KQUEUE
#include <sys/event.h>
#include <sys/time.h>

/* This test verifies the kqueue event delivery fairness behavior.
 * kqueue generally processes events in FIFO order. When calling kevent
 * with a limit smaller than the number of ready events:
 * 1. The returned events are popped from the head of the active list.
 * 2. Unreturned ready events remain at the head of the list.
 * 3. Returned events (if still ready/level-triggered) are re-queued to the tail.
 *
 * This natural FIFO queuing prevents starvation of both unreturned and new events.
 *
 * We verify this by:
 * 1. Registering 5 pipes (A, B, C, D, E) to kqueue (Level-Triggered).
 * 2. Making A, B, C, D ready.
 * 3. Calling kevent(limit=3) -> returns 3 (e.g., A, B, C). D is left out.
 * 4. Making E (new event) ready (appended to tail -> [D, A, B, C, E]).
 * 5. Calling kevent(limit=3) -> must return D (promoted to front), but NOT E (still at tail).
 * 6. Calling kevent(limit=3) -> must return E (now promoted to front because it was left out).
 */
TEST(KqueueFairnessTest, KernelBehavior) {
    const int NUM_PIPES = 5;
    int pipes[NUM_PIPES][2];
    int kq = kqueue();
    ASSERT_NE(kq, -1);

    // 1. Registering 5 pipes (A, B, C, D, E) to kqueue (Level-Triggered).
    for (int i = 0; i < NUM_PIPES; i++) {
        ASSERT_EQ(pipe(pipes[i]), 0);
        fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);

        struct kevent ke;
        EV_SET(&ke, pipes[i][0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void *)(intptr_t)pipes[i][0]);
        ASSERT_NE(kevent(kq, &ke, 1, NULL, 0, NULL), -1);
    }

    char c = 'x';
    // 2. Making A, B, C, D ready.
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(write(pipes[i][1], &c, 1), 1);
    }

    // 3. Calling kevent(limit=3) -> returns 3 (e.g., A, B, C). D is left out.
    struct kevent events[3];
    struct timespec timeout = {0, 0};
    int ready = kevent(kq, NULL, 0, events, 3, &timeout);
    ASSERT_EQ(ready, 3);

    // Track which FD was left out in the first call (D).
    int left_out_fd = -1;
    for (int i = 0; i < 4; i++) {
        int fd = pipes[i][0];
        int found = 0;
        for (int j = 0; j < ready; j++) {
            if ((int)events[j].ident == fd) {
                found = 1;
                break;
            }
        }
        if (!found) {
            left_out_fd = fd;
            break;
        }
    }
    ASSERT_NE(left_out_fd, -1);

    // 4. Making E (new event) ready (appended to tail -> [D, A, B, C, E]).
    ASSERT_EQ(write(pipes[4][1], &c, 1), 1);

    // 5. Calling kevent(limit=3) -> must return D (promoted to front), but NOT E (still at tail).
    ready = kevent(kq, NULL, 0, events, 3, &timeout);
    ASSERT_EQ(ready, 3);

    // Verify that the left-out FD (D) WAS returned in this second call.
    int found_left_out = 0;
    for (int i = 0; i < ready; i++) {
        if ((int)events[i].ident == left_out_fd) {
            found_left_out = 1;
            break;
        }
    }
    EXPECT_TRUE(found_left_out) << "Left-out FD was starved!";

    // Verify that the new event (E, pipe 4) was NOT returned yet (it was at the tail).
    for (int i = 0; i < ready; i++) {
        EXPECT_NE((int)events[i].ident, pipes[4][0]) << "New event E should not be returned yet";
    }

    // 6. Calling kevent(limit=3) -> must return E (now promoted to front because it was left out).
    ready = kevent(kq, NULL, 0, events, 3, &timeout);
    ASSERT_EQ(ready, 3);

    // Verify that the new event (E, pipe 4) IS returned now.
    int found_new_event = 0;
    for (int i = 0; i < ready; i++) {
        if ((int)events[i].ident == pipes[4][0]) {
            found_new_event = 1;
            break;
        }
    }
    EXPECT_TRUE(found_new_event) << "New event E was starved!";

    // Cleanup resources.
    for (int i = 0; i < NUM_PIPES; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    close(kq);
}
#endif
