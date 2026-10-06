/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Unit tests for the traffic rate estimator's mul/div helper. The 64-bit
 * fallback of trafficMulDivRound must not wrap on byte-scale inputs: byte
 * weights (unlike hot-key counts) reach a 2^64 product with ordinary values —
 * a few hundred sampled accesses to a 512 MiB value inside a one-second
 * window already overflow a*b — and a wrap would silently reorder TRAFFIC GET.
 */

#include "generated_wrappers.hpp"

#include <cstdint>

extern "C" {
#include "traffic.h"
}

#ifdef __SIZEOF_INT128__
/* Wide reference: the exact rounded value, no overflow. */
static uint64_t refMulDivRound(uint64_t a, uint64_t b, uint64_t c) {
    if (c == 0) return 0;
    __uint128_t num = (__uint128_t)a * b;
    return (uint64_t)((num + c / 2) / c);
}
#endif

TEST(Traffic, MulDivRoundExactOnSmallValues) {
    /* Hand-checked products, including the round-to-nearest carry. */
    EXPECT_EQ(trafficMulDivRound(100, 1, 3), 33u); /* 100/3 -> 33.33 -> 33 */
    EXPECT_EQ(trafficMulDivRound(101, 1, 3), 34u); /* 33.67 -> 34 */
    EXPECT_EQ(trafficMulDivRound(0, 100000000ULL, 1), 0u);
    EXPECT_EQ(trafficMulDivRound(7, 0, 5), 0u);
    EXPECT_EQ(trafficMulDivRound(1, 1, 1), 1u);
    /* Degenerate divisor: report 0 rather than dividing by zero. */
    EXPECT_EQ(trafficMulDivRound(10, 10, 0), 0u);
}

TEST(Traffic, MulDivRoundMatchesWideReference) {
#ifdef __SIZEOF_INT128__
    /* Fixed table crossing several magnitudes, including values whose a*b
     * exceeds 2^64 (the fallback must agree with the wide path). */
    const uint64_t as[8] = {1u, 999u, 1000003u, 576460752303423488u,
                            172u * 512u * 1024u * 1024u, /* 92,274,688,000 */
                            1800000000000u, 0xFFFFFFFFFFFFFFFFu, 1u};
    const uint64_t bs[4] = {1u, 100000000u, 4294967296u, 1000000000000000u};
    const uint64_t cs[5] = {1u, 2u * 1000000u, 997u, 60000000000u, 0xFFFFFFFFFFFFFFFFu};
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            for (int k = 0; k < 5; k++) {
                EXPECT_EQ(trafficMulDivRound(as[i], bs[j], cs[k]), refMulDivRound(as[i], bs[j], cs[k]))
                    << "a=" << as[i] << " b=" << bs[j] << " c=" << cs[k];
            }
        }
    }
#else
    GTEST_SKIP() << "no 128-bit reference on this platform";
#endif
}

TEST(Traffic, MulDivRoundDoesNotWrapOnByteScaleInputs) {
    /* The scenario from review: a one-second window sampled 172 accesses of a
     * 512 MiB value at 100%, so twice_midpoint ~= 2 * 92,274,688,000 and the
     * numerator twice_midpoint * 1e8 ~= 1.8e19 — past 2^64. The wrapped
     * fallback would return a small number and misorder the leaderboard. */
    const uint64_t twice_midpoint = 2ULL * 172ULL * 512ULL * 1024ULL * 1024ULL;
    const uint64_t b = 100ULL * 1000000ULL;        /* the estimator's constant */
    const uint64_t c = 2ULL * 100ULL * 1010000ULL; /* 2 * pct * duration_us */
    uint64_t rps = trafficMulDivRound(twice_midpoint, b, c);
    /* Expected rate: 92 GB / 1.01 s ~= 91 GB/s; a wrap would report < 1e9. */
    EXPECT_GT(rps, 90000000000ULL) << "byte-scale numerator wrapped around";
    EXPECT_LT(rps, 100000000000ULL);

    /* The maximum reachable configuration: c = 2 * 100% * 300s window and a
     * numerator beyond 2^64 must still not wrap (c*b <= 6e18 < 2^64). */
    const uint64_t c_max = 2ULL * 100ULL * 300ULL * 1000000ULL;
    uint64_t r = trafficMulDivRound(0xFFFFFFFFFFFFFFFFULL, b, c_max);
    EXPECT_GT(r, 30000000000ULL) << "max-config numerator wrapped around";
}
