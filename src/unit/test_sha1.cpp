/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "sha1.h"
}

#define BUFSIZE 4096

class Sha1Test : public ::testing::Test {};

TEST_F(Sha1Test, TestSha1) {
    SHA1_CTX ctx;
    unsigned char hash[20], buf[BUFSIZE];
    unsigned char expected[20] = {0x15, 0xdd, 0x99, 0xa1, 0x99, 0x1e, 0x0b, 0x38, 0x26, 0xfe,
                                  0xde, 0x3d, 0xef, 0xfc, 0x1f, 0xeb, 0xa4, 0x22, 0x78, 0xe6};
    int i;

    for (i = 0; i < BUFSIZE; i++) buf[i] = i;

    SHA1Init(&ctx);
    for (i = 0; i < 1000; i++) SHA1Update(&ctx, buf, BUFSIZE);
    SHA1Final(hash, &ctx);

    ASSERT_EQ(memcmp(hash, expected, 20), 0);
}
