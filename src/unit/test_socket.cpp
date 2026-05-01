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

class SocketTest : public ::testing::Test {
  protected:
    StrictMock<MockValkey> mock;

    static void SetUpTestSuite() {
        /* serverLog dereferences server.logfile; set it so registration doesn't segfault. */
        server.logfile = (char *)"";
        RedisRegisterConnectionTypeSocket();
    }
};

/* When the socket connection type (CT_Socket) is in use, connBlockingConnect
 * dispatches to connSocketBlockingConnect. Verify it returns C_ERR when
 * aeWait returns -1 and preserves errno from the failed poll(2) call. */
TEST_F(SocketTest, BlockingConnectReturnsErrorWhenAeWaitFails) {
    EXPECT_CALL(mock, anetTcpNonBlockConnect(_, _, _)).WillOnce(Return(5));
    EXPECT_CALL(mock, aeWait(_, _, _)).WillOnce(DoAll(Assign(&errno, EINTR), Return(-1)));

    connection *conn = connCreate(connectionTypeTcp());
    int ret = connBlockingConnect(conn, "127.0.0.1", 1234, 100);

    EXPECT_EQ(ret, C_ERR);
    EXPECT_NE(conn->state, CONN_STATE_CONNECTED);
    EXPECT_EQ(conn->last_errno, EINTR);

    conn->fd = -1;
    connClose(conn);
}

/* Same dispatch path (CT_Socket -> connSocketBlockingConnect).
 * Verify connBlockingConnect returns C_ERR with ETIMEDOUT when aeWait
 * returns 0 (poll timed out without the fd becoming writable). */
TEST_F(SocketTest, BlockingConnectReturnsTimeoutWhenAeWaitTimesOut) {
    EXPECT_CALL(mock, anetTcpNonBlockConnect(_, _, _)).WillOnce(Return(5));
    EXPECT_CALL(mock, aeWait(_, _, _)).WillOnce(Return(0));

    connection *conn = connCreate(connectionTypeTcp());
    int ret = connBlockingConnect(conn, "127.0.0.1", 1234, 100);

    EXPECT_EQ(ret, C_ERR);
    EXPECT_EQ(conn->last_errno, ETIMEDOUT);

    conn->fd = -1;
    connClose(conn);
}
