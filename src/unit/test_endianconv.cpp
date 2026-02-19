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
