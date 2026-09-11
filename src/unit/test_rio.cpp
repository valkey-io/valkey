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

static ConnectionType CT_Stub;

class RioConnOverflowTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        memset(&CT_Stub, 0, sizeof(CT_Stub));
    }

    void SetUp() override {
        memset(&conn, 0, sizeof(conn));
        conn.type = &CT_Stub;
        conn.fd = -1;
    }

    connection conn;
};

TEST_F(RioConnOverflowTest, InitKeepsFullWidthReadLimit) {
    rio r;
    rioInitWithConn(&r, &conn, UINT64_C(4294967296));
    EXPECT_EQ(r.io.conn.read_limit, UINT64_C(4294967296));
    EXPECT_EQ(r.io.conn.read_so_far, UINT64_C(0));
    rioFreeConn(&r, NULL);
}

TEST_F(RioConnOverflowTest, ReadChecksRemainingFullWidthLimit) {
    rio r;
    char buf[8];

    rioInitWithConn(&r, &conn, UINT64_C(4294967296));
    r.io.conn.read_so_far = UINT64_C(4294967292);
    errno = 0;
    EXPECT_EQ(rioRead(&r, buf, sizeof(buf)), 0u);
    EXPECT_EQ(errno, EOVERFLOW);
    rioFreeConn(&r, NULL);
}

TEST_F(RioConnOverflowTest, ReadRejectsLimitBeforeGrowingBuffer) {
    rio r;
    char buf[PROTO_IOBUF_LEN + 1];

    rioInitWithConn(&r, &conn, PROTO_IOBUF_LEN);
    size_t initial_alloc = sdsalloc(r.io.conn.buf);
    errno = 0;
    EXPECT_EQ(rioRead(&r, buf, PROTO_IOBUF_LEN + 1), 0u);
    EXPECT_EQ(errno, EOVERFLOW);
    EXPECT_EQ(sdsalloc(r.io.conn.buf), initial_alloc);
    rioFreeConn(&r, NULL);
}

TEST_F(RioConnOverflowTest, ReadRejectsOffTOverflow) {
    rio r;
    char buf[64];

    rioInitWithConn(&r, &conn, 0);
    r.io.conn.read_so_far = INT64_MAX - 10;
    errno = 0;
    EXPECT_EQ(rioRead(&r, buf, 64), 0u);
    EXPECT_EQ(errno, EOVERFLOW);
    rioFreeConn(&r, NULL);
}

TEST_F(RioConnOverflowTest, ReadRejectsReadSoFarBeyondReadLimit) {
    rio r;
    char buf[8];

    rioInitWithConn(&r, &conn, 100);
    r.io.conn.read_so_far = 200;
    errno = 0;
    EXPECT_EQ(rioRead(&r, buf, 8), 0u);
    EXPECT_EQ(errno, EOVERFLOW);
    rioFreeConn(&r, NULL);
}
