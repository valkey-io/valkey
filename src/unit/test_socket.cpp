/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cerrno>

extern "C" {
#include "connection.h"
#include "server.h"
}

/* Not a real descriptor: tests reset conn->fd before connClose so it is never closed. */
#define FAKE_FD 5

class SocketTest : public ::testing::Test {
  protected:
    StrictMock<MockValkey> mock;

    static void SetUpTestSuite() {
        /* serverLog dereferences server.logfile; set it so registration doesn't segfault. */
        server.logfile = (char *)"";
        if (!connectionByType(CONN_TYPE_SOCKET)) connTypeInitialize();
    }

    void closeConn(connection *conn) {
        conn->fd = -1;
        connClose(conn);
    }
};

TEST_F(SocketTest, BlockingConnectRetriesWhenAeWaitIsInterrupted) {
    EXPECT_CALL(mock, anetTcpNonBlockConnect(_, _, _)).WillOnce(Return(FAKE_FD));
    EXPECT_CALL(mock, aeWait(FAKE_FD, AE_WRITABLE, _))
        .WillOnce(DoAll(Assign(&errno, EINTR), Return(-1)))
        .WillOnce(Return(AE_WRITABLE));

    connection *conn = connCreate(connectionTypeTcp());
    EXPECT_EQ(connBlockingConnect(conn, "127.0.0.1", 1234, 10000), C_OK);
    EXPECT_EQ(conn->state, CONN_STATE_CONNECTED);
    closeConn(conn);
}

TEST_F(SocketTest, BlockingConnectReturnsTimeoutWhenInterruptedPastDeadline) {
    EXPECT_CALL(mock, anetTcpNonBlockConnect(_, _, _)).WillOnce(Return(FAKE_FD));
    EXPECT_CALL(mock, aeWait(FAKE_FD, AE_WRITABLE, _)).WillOnce(DoAll(Assign(&errno, EINTR), Return(-1)));

    connection *conn = connCreate(connectionTypeTcp());
    EXPECT_EQ(connBlockingConnect(conn, "127.0.0.1", 1234, 0), C_ERR);
    EXPECT_EQ(conn->state, CONN_STATE_ERROR);
    EXPECT_EQ(conn->last_errno, ETIMEDOUT);
    closeConn(conn);
}

TEST_F(SocketTest, BlockingConnectReturnsErrorWhenAeWaitFails) {
    EXPECT_CALL(mock, anetTcpNonBlockConnect(_, _, _)).WillOnce(Return(FAKE_FD));
    EXPECT_CALL(mock, aeWait(FAKE_FD, AE_WRITABLE, _)).WillOnce(DoAll(Assign(&errno, ENOMEM), Return(-1)));

    connection *conn = connCreate(connectionTypeTcp());
    EXPECT_EQ(connBlockingConnect(conn, "127.0.0.1", 1234, 10000), C_ERR);
    EXPECT_EQ(conn->state, CONN_STATE_ERROR);
    EXPECT_EQ(conn->last_errno, ENOMEM);
    closeConn(conn);
}

TEST_F(SocketTest, BlockingConnectReturnsTimeoutWhenAeWaitTimesOut) {
    EXPECT_CALL(mock, anetTcpNonBlockConnect(_, _, _)).WillOnce(Return(FAKE_FD));
    EXPECT_CALL(mock, aeWait(FAKE_FD, AE_WRITABLE, _)).WillOnce(Return(0));

    connection *conn = connCreate(connectionTypeTcp());
    EXPECT_EQ(connBlockingConnect(conn, "127.0.0.1", 1234, 10000), C_ERR);
    EXPECT_EQ(conn->state, CONN_STATE_ERROR);
    EXPECT_EQ(conn->last_errno, ETIMEDOUT);
    closeConn(conn);
}
