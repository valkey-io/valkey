/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define INTRINSICS_TEST_HAS_DIAGNOSTIC_PRAGMA
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
#endif

#undef __GNUC__
#undef __clang__

extern "C" {
#include "intrinsics.h"
}

#ifdef INTRINSICS_TEST_HAS_DIAGNOSTIC_PRAGMA
#pragma GCC diagnostic pop
#endif

class IntrinsicsTest : public ::testing::Test {};

TEST_F(IntrinsicsTest, TestBuiltinCtzllFallback) {
    ASSERT_EQ(64, builtin_ctzll(0));

    for (int i = 0; i < 64; i++) {
        uint64_t value = ((uint64_t)1) << i;
        ASSERT_EQ(i, builtin_ctzll(value));
    }
}
