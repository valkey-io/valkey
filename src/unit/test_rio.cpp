/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cerrno>
#include <cstring>

extern "C" {
#include "rio.h"
#include "server.h"
}

/* Tests for the overflow-safe read accounting in rioConnRead().
 *
 * These tests cover the 64-bit arithmetic only. They cannot reproduce the
 * 32-bit size_t narrowing of a replication bulk length (where 4294967296
 * truncates to 0 and RIO then treats the limit as unlimited), because size_t
 * is 64 bits wide on the hosts these tests run on. What they do verify is that
 * rioInitWithConn() keeps the limit at full 64-bit width and that the guards
 * in rioConnRead() reject requests that would wrap around.
 *
 * All three checks are evaluated before rioConnRead() ever calls connRead(),
 * so a zeroed stub connection is enough and no socket is needed. The
 * requested lengths are kept small so that the buffer preallocated by
 * rioInitWithConn() is never grown. */

static ConnectionType CT_Stub;

class RioConnOverflowTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        /* A zeroed connection type is sufficient: none of its callbacks are
         * reached by the tests below. */
        memset(&CT_Stub, 0, sizeof(CT_Stub));
    }

    void SetUp() override {
        memset(&conn, 0, sizeof(conn));
        conn.type = &CT_Stub;
        conn.fd = -1;
    }

    connection conn;
};

/* The read limit is stored at full 64-bit width, not narrowed to size_t of a
 * smaller build or truncated to 0. */
TEST_F(RioConnOverflowTest, InitKeepsFullWidthReadLimit) {
    rio r;
    rioInitWithConn(&r, &conn, UINT64_C(4294967296));
    EXPECT_EQ(r.io.conn.read_limit, UINT64_C(4294967296));
    EXPECT_EQ(r.io.conn.read_so_far, UINT64_C(0));
    rioFreeConn(&r, NULL);
}

/* A request that would push the running total past UINT64_MAX is rejected
 * with EOVERFLOW, even when no read limit is set. */
TEST_F(RioConnOverflowTest, ReadRejectsWrapAroundOfReadSoFar) {
    rio r;
    char buf[64];

    rioInitWithConn(&r, &conn, 0);
    r.io.conn.read_so_far = UINT64_MAX - 10;
    errno = 0;
    EXPECT_EQ(rioRead(&r, buf, 64), 0);
    EXPECT_EQ(errno, EOVERFLOW);
    rioFreeConn(&r, NULL);
}

/* A running total that already exceeds the read limit is rejected with
 * EOVERFLOW instead of computing a negative remainder. */
TEST_F(RioConnOverflowTest, ReadRejectsReadSoFarBeyondReadLimit) {
    rio r;
    char buf[8];

    rioInitWithConn(&r, &conn, 100);
    r.io.conn.read_so_far = 200;
    errno = 0;
    EXPECT_EQ(rioRead(&r, buf, 8), 0);
    EXPECT_EQ(errno, EOVERFLOW);
    rioFreeConn(&r, NULL);
}
