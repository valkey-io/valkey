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

static int parseHeader(const char *line, aofHeader *hdr) {
    const char *err = NULL;
    return aofParseHeader(line, strlen(line), hdr, &err);
}

TEST(AofHeaderTest, ParsesHeader) {
    aofHeader hdr;
    ASSERT_EQ(parseHeader("#HDR:v1;len:45;checksum:1234;\r\n", &hdr), C_OK);
    ASSERT_EQ(hdr.len, 45);
    ASSERT_EQ(hdr.checksum, 1234u);
    ASSERT_TRUE(hdr.has_checksum);
    /* The checksum covers the line up to the checksum field. */
    ASSERT_EQ(hdr.checksum_offset, strlen("#HDR:v1;len:45;"));
}

TEST(AofHeaderTest, ParsesFullRangeChecksum) {
    aofHeader hdr;
    ASSERT_EQ(parseHeader("#HDR:v1;len:0;checksum:18446744073709551615;\r\n", &hdr), C_OK);
    ASSERT_EQ(hdr.len, 0);
    ASSERT_EQ(hdr.checksum, UINT64_MAX);
}

TEST(AofHeaderTest, IgnoresUnknownFields) {
    aofHeader hdr;
    const char *line = "#HDR:v1;len:45;replid:abc;reploff:7;maxlen:5;checksum:9;\r\n";
    ASSERT_EQ(parseHeader(line, &hdr), C_OK);
    ASSERT_EQ(hdr.len, 45);
    ASSERT_EQ(hdr.checksum, 9u);
    ASSERT_EQ(hdr.checksum_offset, (size_t)(strstr(line, "checksum:") - line));
}

TEST(AofHeaderTest, ChecksumIsOptional) {
    aofHeader hdr;
    ASSERT_EQ(parseHeader("#HDR:v1;len:45;reploff:7;\r\n", &hdr), C_OK);
    ASSERT_EQ(hdr.len, 45);
    ASSERT_FALSE(hdr.has_checksum);
}

TEST(AofHeaderTest, RejectsMalformedHeaders) {
    const char *invalid[] = {
        "#HDR:v1;len:45;checksum:1;",                       /* No CRLF. */
        "#HDR:v1;len:45;checksum:1;\n",                     /* No CR. */
        "#HDR:v2;len:45;checksum:1;\r\n",                   /* Unsupported version. */
        "#HDR:v1;checksum:1;\r\n",                          /* No len. */
        "#HDR:v1;foo:1;len:45;checksum:1;\r\n",             /* len isn't the first field. */
        "#HDR:v1;len:45;len:46;checksum:1;\r\n",            /* Duplicate len. */
        "#HDR:v1;len:45;checksum:1;foo:2;\r\n",             /* checksum isn't the last field. */
        "#HDR:v1;len:abc;checksum:1;\r\n",                  /* Not a number. */
        "#HDR:v1;len:-1;checksum:1;\r\n",                   /* Negative. */
        "#HDR:v1;len:;checksum:1;\r\n",                     /* Empty. */
        "#HDR:v1;len:99999999999999999999;checksum:1;\r\n", /* Overflow. */
        "#HDR:v1;len:45;foo;checksum:1;\r\n",               /* Field without a value. */
        "#HDR:v1;len:45;:1;checksum:1;\r\n",                /* Field without a key. */
        "#HDR:v1;len:45;checksum:1\r\n",                    /* Unterminated field. */
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        aofHeader hdr;
        ASSERT_EQ(parseHeader(invalid[i], &hdr), C_ERR) << invalid[i];
    }
}
