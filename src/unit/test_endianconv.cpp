/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "endianconv.h"
}

class EndianconvTest : public ::testing::Test {};

TEST_F(EndianconvTest, TestEndianconv) {
    char buf[32];

    snprintf(buf, sizeof(buf), "ciaoroma");
    memrev16(buf);
    ASSERT_STREQ(buf, "icaoroma");

    snprintf(buf, sizeof(buf), "ciaoroma");
    memrev32(buf);
    ASSERT_STREQ(buf, "oaicroma");

    snprintf(buf, sizeof(buf), "ciaoroma");
    memrev64(buf);
    ASSERT_STREQ(buf, "amoroaic");
}

TEST_F(EndianconvTest, TestBswap) {
    ASSERT_EQ(bswap16(0x0102U), 0x0201U);
    ASSERT_EQ(bswap32(0x01020304U), 0x04030201U);
    ASSERT_EQ(bswap64(0x0102030405060708ULL), 0x0807060504030201ULL);
}

TEST_F(EndianconvTest, TestMemrevIfbeRoundtrip) {
    uint16_t v16 = 0x0102;
    uint32_t v32 = 0x01020304;
    uint64_t v64 = 0x0102030405060708ULL;

    uint16_t orig16 = v16;
    uint32_t orig32 = v32;
    uint64_t orig64 = v64;

    /* Apply twice - should return to original on both LE and BE */
    memrev16ifbe(&v16);
    memrev16ifbe(&v16);
    ASSERT_EQ(v16, orig16);

    memrev32ifbe(&v32);
    memrev32ifbe(&v32);
    ASSERT_EQ(v32, orig32);

    memrev64ifbe(&v64);
    memrev64ifbe(&v64);
    ASSERT_EQ(v64, orig64);
}

TEST_F(EndianconvTest, TestIntrevIfbeRoundtrip) {
    ASSERT_EQ(intrev16ifbe(intrev16ifbe(0x0102)), 0x0102);
    ASSERT_EQ(intrev32ifbe(intrev32ifbe(0x01020304)), 0x01020304);
    ASSERT_EQ(intrev64ifbe(intrev64ifbe(0x0102030405060708ULL)), 0x0102030405060708ULL);
}

TEST_F(EndianconvTest, TestHtonuNtohu64) {
    /* Round-trip must hold on any architecture */
    ASSERT_EQ(ntohu64(htonu64(0x0102030405060708ULL)), 0x0102030405060708ULL);
    ASSERT_EQ(ntohu64(htonu64(0ULL)), 0ULL);
    ASSERT_EQ(ntohu64(htonu64(UINT64_MAX)), UINT64_MAX);

    /* Verify network byte order (big-endian) output */
    uint64_t net = htonu64(0x0102030405060708ULL);
    unsigned char *p = (unsigned char *)&net;
    ASSERT_EQ(p[0], 0x01);
    ASSERT_EQ(p[1], 0x02);
    ASSERT_EQ(p[2], 0x03);
    ASSERT_EQ(p[3], 0x04);
    ASSERT_EQ(p[4], 0x05);
    ASSERT_EQ(p[5], 0x06);
    ASSERT_EQ(p[6], 0x07);
    ASSERT_EQ(p[7], 0x08);
}
