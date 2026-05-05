/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "stream.h"
}

class StreamIdTest : public ::testing::Test {};

TEST_F(StreamIdTest, TestStreamEncodeDecodeRoundtrip) {
    streamID id = {0x0102030405060708ULL, 0x090a0b0c0d0e0f10ULL};
    unsigned char buf[16];

    streamEncodeID(buf, &id);

    streamID decoded;
    streamDecodeID(buf, &decoded);

    ASSERT_EQ(decoded.ms, id.ms);
    ASSERT_EQ(decoded.seq, id.seq);
}

TEST_F(StreamIdTest, TestStreamEncodeIDBigEndian) {
    streamID id = {0x0102030405060708ULL, 0x090a0b0c0d0e0f10ULL};
    unsigned char buf[16];

    streamEncodeID(buf, &id);

    /* Verify big-endian byte order for ms (first 8 bytes) */
    ASSERT_EQ(buf[0], 0x01);
    ASSERT_EQ(buf[1], 0x02);
    ASSERT_EQ(buf[2], 0x03);
    ASSERT_EQ(buf[3], 0x04);
    ASSERT_EQ(buf[4], 0x05);
    ASSERT_EQ(buf[5], 0x06);
    ASSERT_EQ(buf[6], 0x07);
    ASSERT_EQ(buf[7], 0x08);

    /* Verify big-endian byte order for seq (next 8 bytes) */
    ASSERT_EQ(buf[8], 0x09);
    ASSERT_EQ(buf[9], 0x0a);
    ASSERT_EQ(buf[10], 0x0b);
    ASSERT_EQ(buf[11], 0x0c);
    ASSERT_EQ(buf[12], 0x0d);
    ASSERT_EQ(buf[13], 0x0e);
    ASSERT_EQ(buf[14], 0x0f);
    ASSERT_EQ(buf[15], 0x10);
}

TEST_F(StreamIdTest, TestStreamIDLexicographicOrdering) {
    /* Big-endian encoding ensures memcmp preserves numeric order */
    streamID id_a = {100, 0};
    streamID id_b = {200, 0};
    unsigned char buf_a[16], buf_b[16];

    streamEncodeID(buf_a, &id_a);
    streamEncodeID(buf_b, &id_b);

    ASSERT_LT(memcmp(buf_a, buf_b, 16), 0);

    /* Test sequence ordering when ms is equal */
    streamID id_c = {100, 1};
    streamID id_d = {100, 2};
    unsigned char buf_c[16], buf_d[16];

    streamEncodeID(buf_c, &id_c);
    streamEncodeID(buf_d, &id_d);

    ASSERT_LT(memcmp(buf_c, buf_d, 16), 0);
}
