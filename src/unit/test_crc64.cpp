/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdint>
#include <cstdio>

extern "C" {
#include "crc64.h"
#include "fmacros.h"

extern uint64_t _crc64(uint_fast64_t crc, const void *in_data, const uint64_t len);
}

class Crc64Test : public ::testing::Test {
  protected:
    void SetUp() override {
        crc64_init();
    }
};

TEST_F(Crc64Test, TestCrc64) {
    unsigned char numbers[] = "123456789";
    ASSERT_EQ((uint64_t)_crc64(0, numbers, 9), 16845390139448941002ull) << "[calcula]: CRC64 '123456789'";
    ASSERT_EQ((uint64_t)crc64(0, numbers, 9), 16845390139448941002ull) << "[calcula]: CRC64 '123456789'";

    unsigned char li[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed "
                         "do eiusmod tempor incididunt ut labore et dolore magna "
                         "aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
                         "ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis "
                         "aute irure dolor in reprehenderit in voluptate velit esse "
                         "cillum dolore eu fugiat nulla pariatur. Excepteur sint "
                         "occaecat cupidatat non proident, sunt in culpa qui officia "
                         "deserunt mollit anim id est laborum.";
    ASSERT_EQ((uint64_t)_crc64(0, li, sizeof(li)), 14373597793578550195ull) << "[calcula]: CRC64 TEXT'";
    ASSERT_EQ((uint64_t)crc64(0, li, sizeof(li)), 14373597793578550195ull) << "[calcula]: CRC64 TEXT";
}
