/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdint>
#include <cstdlib>
#include <time.h>

extern "C" {
#include "config.h"
#include "fmacros.h"
#include "zmalloc.h"

extern long long popcountScalar(void *s, long count);
#if HAVE_X86_SIMD
extern long long popcountAVX2(void *s, long count);
#endif
#if HAVE_ARM_NEON
extern long long popcountNEON(void *s, long count);
#endif
}

static long long bitcount(void *s, long count) {
    long long bits = 0;
    uint8_t *p = (uint8_t *)s;
    for (int x = 0; x < count; x += 1) {
        uint8_t val = *(x + p);
        while (val) {
            bits += val & 1;
            val >>= 1;
        }
    }
    return bits;
}

static void test_case(const char *msg, int size) {
    size_t bufsize = size > 0 ? size : 1;
    uint8_t *buf = (uint8_t *)malloc(bufsize);
    ASSERT_NE(buf, nullptr) << msg;

    int fuzzing = 1000;
    for (int y = 0; y < fuzzing; y += 1) {
        for (int z = 0; z < size; z += 1) {
            buf[z] = random() % 256;
        }

        long long expect = bitcount(buf, size);
        long long ret_scalar = popcountScalar(buf, size);
        ASSERT_EQ(expect, ret_scalar) << msg;
#if HAVE_X86_SIMD
        long long ret_avx2 = popcountAVX2(buf, size);
        ASSERT_EQ(expect, ret_avx2) << msg;
#endif
#if HAVE_ARM_NEON
        long long ret_neon = popcountNEON(buf, size);
        ASSERT_EQ(expect, ret_neon) << msg;
#endif
    }

    free(buf);
}

/* Minimal test fixture */
class BitopsTest : public ::testing::Test {
};

TEST_F(BitopsTest, TestPopcount) {
    /* The AVX2 version divides the array into the following 3 parts.
     *        Part A         Part B       Part C
     * +-----------------+--------------+---------+
     * | 8 * 32bytes * X |  32bytes * Y | Z bytes |
     * +-----------------+--------------+---------+
     */
    /* So we test the following cases */
    test_case("Popcount(Part A)", 8 * 32 * 2);
    test_case("Popcount(Part B)", 32 * 2);
    test_case("Popcount(Part C)", 2);
    test_case("Popcount(Part A + Part B)", 8 * 32 * 7 + 32 * 2);
    test_case("Popcount(Part A + Part C)", 8 * 32 * 11 + 7);
    test_case("Popcount(Part A + Part B + Part C)", 8 * 32 * 3 + 3 * 32 + 5);
    test_case("Popcount(Corner case)", 0);
}
