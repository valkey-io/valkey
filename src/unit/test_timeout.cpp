/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "server.h"
}

class TimeoutKeyTest : public ::testing::Test {};

TEST_F(TimeoutKeyTest, TestEncodeDecodeRoundtrip) {
    client dummy_client;
    unsigned char buf[16];

    uint64_t timeout = 0x0102030405060708ULL;
    encodeTimeoutKey(&dummy_client, timeout, buf);

    uint64_t decoded_timeout;
    client *decoded_client;
    decodeTimeoutKey(buf, &decoded_timeout, &decoded_client);

    ASSERT_EQ(decoded_timeout, timeout);
    ASSERT_EQ(decoded_client, &dummy_client);
}

TEST_F(TimeoutKeyTest, TestTimeoutBigEndianEncoding) {
    client dummy_client;
    unsigned char buf[16];

    uint64_t timeout = 0x0102030405060708ULL;
    encodeTimeoutKey(&dummy_client, timeout, buf);

    /* Verify big-endian byte order for timeout (first 8 bytes) */
    ASSERT_EQ(buf[0], 0x01);
    ASSERT_EQ(buf[1], 0x02);
    ASSERT_EQ(buf[2], 0x03);
    ASSERT_EQ(buf[3], 0x04);
    ASSERT_EQ(buf[4], 0x05);
    ASSERT_EQ(buf[5], 0x06);
    ASSERT_EQ(buf[6], 0x07);
    ASSERT_EQ(buf[7], 0x08);
}

TEST_F(TimeoutKeyTest, TestTimeoutLexicographicOrdering) {
    client dummy_client;
    unsigned char buf_a[16], buf_b[16];

    /* Big-endian encoding ensures memcmp preserves numeric order */
    encodeTimeoutKey(&dummy_client, 100, buf_a);
    encodeTimeoutKey(&dummy_client, 200, buf_b);

    /* Earlier timeout should sort before later timeout */
    ASSERT_LT(memcmp(buf_a, buf_b, 8), 0);
}
