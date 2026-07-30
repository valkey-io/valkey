/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <stdint.h>

#ifdef __INTRINSICS_H
#error "intrinsics.h must not be included before this point, or the fallback path is not compiled"
#endif

#undef __GNUC__
#undef __clang__

extern "C" {
#include "../intrinsics.h"
}

class IntrinsicsTest : public ::testing::Test {};

TEST_F(IntrinsicsTest, TestBuiltinCtzllFallback) {
    ASSERT_EQ(64, builtin_ctzll(0));

    for (int i = 0; i < 64; i++) {
        uint64_t value = ((uint64_t)1) << i;
        ASSERT_EQ(i, builtin_ctzll(value));
    }
}
