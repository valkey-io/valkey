/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "config.h"
#include "fmacros.h"
#include "zmalloc.h"

#if HAVE_X86_SIMD
extern void bitopAndAVX2(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
extern void bitopOrAVX2(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
extern void bitopXorAVX2(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
extern void bitopNotAVX2(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
#endif
#if HAVE_ARM_NEON
extern void bitopAndNEON(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
extern void bitopOrNEON(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
extern void bitopXorNEON(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
extern void bitopNotNEON(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
#endif
}

#if HAVE_X86_SIMD || HAVE_ARM_NEON
static const int BITOP_AND = 0;
static const int BITOP_OR = 1;
static const int BITOP_XOR = 2;
static const int BITOP_NOT = 3;

typedef void (*BitopSimdFunc)(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);

enum BitopLayout {
    LAYOUT_EQUAL,
    LAYOUT_ASCENDING,
    LAYOUT_DESCENDING,
    LAYOUT_RANDOM_MIXED,
};

struct Lcg {
    explicit Lcg(uint32_t seed) :
        state(seed) {
    }

    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }

    unsigned char nextByte() {
        return (unsigned char)(next() >> 24);
    }

    unsigned long nextLength(unsigned long maxlen) {
        if (maxlen == 0) return 0;
        return (unsigned long)(next() % (maxlen + 1));
    }

    uint32_t state;
};

static const char *opName(int op) {
    switch (op) {
    case BITOP_AND: return "AND";
    case BITOP_OR: return "OR";
    case BITOP_XOR: return "XOR";
    case BITOP_NOT: return "NOT";
    default: return "UNKNOWN";
    }
}

static const char *layoutName(BitopLayout layout) {
    switch (layout) {
    case LAYOUT_EQUAL: return "equal";
    case LAYOUT_ASCENDING: return "ascending";
    case LAYOUT_DESCENDING: return "descending";
    case LAYOUT_RANDOM_MIXED: return "random-mixed";
    default: return "unknown";
    }
}

static void bitopScalarOracle(unsigned char *dst, const unsigned char *const *src, const unsigned long *len, unsigned long numkeys, unsigned long maxlen, int op) {
    for (unsigned long j = 0; j < maxlen; j++) {
        unsigned char output = (len[0] <= j) ? 0 : src[0][j];
        if (op == BITOP_NOT) {
            dst[j] = (unsigned char)~output;
            continue;
        }

        for (unsigned long i = 1; i < numkeys; i++) {
            unsigned char byte = (len[i] <= j) ? 0 : src[i][j];
            switch (op) {
            case BITOP_AND: output = (unsigned char)(output & byte); break;
            case BITOP_OR: output = (unsigned char)(output | byte); break;
            case BITOP_XOR: output = (unsigned char)(output ^ byte); break;
            }
        }
        dst[j] = output;
    }
}

static void makeLengths(std::vector<unsigned long> *len, unsigned long size, BitopLayout layout, Lcg *rng) {
    unsigned long numkeys = len->size();

    for (unsigned long i = 0; i < numkeys; i++) {
        switch (layout) {
        case LAYOUT_EQUAL: (*len)[i] = size; break;
        case LAYOUT_ASCENDING: (*len)[i] = (size * (i + 1)) / numkeys; break;
        case LAYOUT_DESCENDING: (*len)[i] = (size * (numkeys - i)) / numkeys; break;
        case LAYOUT_RANDOM_MIXED: (*len)[i] = rng->nextLength(size); break;
        }
    }

    if (layout == LAYOUT_RANDOM_MIXED && numkeys > 0) {
        unsigned long zero_index = rng->next() % numkeys;
        (*len)[zero_index] = 0;
        if (numkeys > 1) {
            unsigned long max_index = (zero_index + 1) % numkeys;
            (*len)[max_index] = size;
        }
    }
}

static unsigned long minLength(const std::vector<unsigned long> &len) {
    unsigned long minlen = len[0];
    for (unsigned long i = 1; i < len.size(); i++) {
        if (len[i] < minlen) minlen = len[i];
    }
    return minlen;
}

static unsigned long maxLength(const std::vector<unsigned long> &len) {
    unsigned long maxlen = len[0];
    for (unsigned long i = 1; i < len.size(); i++) {
        if (len[i] > maxlen) maxlen = len[i];
    }
    return maxlen;
}

static void runOneCase(const char *impl, BitopSimdFunc func, int op, unsigned long requested_size, BitopLayout layout, const std::vector<unsigned long> &len, Lcg *rng) {
    const unsigned char kGuardByte = 0xcc;
    const size_t kGuardSize = 32;
    unsigned long numkeys = len.size();
    unsigned long minlen = minLength(len);
    unsigned long maxlen = maxLength(len);

    ::testing::Message case_msg;
    case_msg << "impl=" << impl << " op=" << opName(op) << " requested_size=" << requested_size
             << " layout=" << layoutName(layout) << " numkeys=" << numkeys << " minlen=" << minlen
             << " maxlen=" << maxlen;
    SCOPED_TRACE(case_msg);

    ::testing::Message lengths_msg;
    lengths_msg << "lengths=[";
    for (unsigned long i = 0; i < numkeys; i++) {
        if (i != 0) lengths_msg << ",";
        lengths_msg << len[i];
    }
    lengths_msg << "]";
    SCOPED_TRACE(lengths_msg);

    std::vector<std::vector<unsigned char>> srcbuf(numkeys);
    std::vector<unsigned char *> src(numkeys);
    std::vector<const unsigned char *> oracle_src(numkeys);
    for (unsigned long i = 0; i < numkeys; i++) {
        srcbuf[i].resize(len[i]);
        for (unsigned long j = 0; j < len[i]; j++) {
            srcbuf[i][j] = rng->nextByte();
        }
        src[i] = srcbuf[i].data();
        oracle_src[i] = srcbuf[i].data();
    }

    std::vector<unsigned char> expected(maxlen);
    bitopScalarOracle(expected.data(), oracle_src.data(), len.data(), numkeys, maxlen, op);

    std::vector<unsigned char> dstbuf(maxlen + 2 * kGuardSize, kGuardByte);
    unsigned char *dst = dstbuf.data() + kGuardSize;
    if (maxlen > 0) std::memset(dst, 0xa5, maxlen);

    std::vector<unsigned long> simd_len = len;
    func(dst, src.data(), simd_len.data(), numkeys, minlen, maxlen);

    for (size_t i = 0; i < kGuardSize; i++) {
        ASSERT_EQ((int)kGuardByte, (int)dstbuf[i]) << "leading guard offset=" << i;
        ASSERT_EQ((int)kGuardByte, (int)dstbuf[kGuardSize + maxlen + i]) << "trailing guard offset=" << i;
    }

    if (maxlen > 0 && std::memcmp(dst, expected.data(), maxlen) != 0) {
        for (unsigned long i = 0; i < maxlen; i++) {
            if (dst[i] != expected[i]) {
                ASSERT_EQ((int)expected[i], (int)dst[i]) << "active region mismatch offset=" << i;
            }
        }
    }
}

#if HAVE_X86_SIMD
static bool cpuHasAvx2() {
    return __builtin_cpu_supports("avx2") != 0;
}
#endif
#endif

class BitopSimdTest : public ::testing::Test {
  protected:
#if HAVE_X86_SIMD || HAVE_ARM_NEON
    void runCases(const char *impl, BitopSimdFunc func, int op) {
        static const unsigned long sizes[] = {0, 1, 31, 32, 33, 63, 64, 65, 127,
                                              128, 255, 256, 257, 511, 512, 4095, 4096, 4097};
        static const unsigned long multi_counts[] = {1, 2, 3, 4, 8, 15, 16, 17, 33};
        static const unsigned long not_counts[] = {1};
        static const BitopLayout layouts[] = {LAYOUT_EQUAL, LAYOUT_ASCENDING, LAYOUT_DESCENDING,
                                              LAYOUT_RANDOM_MIXED};

        const unsigned long *counts = (op == BITOP_NOT) ? not_counts : multi_counts;
        size_t count_len = (op == BITOP_NOT) ? (sizeof(not_counts) / sizeof(not_counts[0]))
                                             : (sizeof(multi_counts) / sizeof(multi_counts[0]));
        Lcg rng(0x5eed0000u ^ (uint32_t)op ^ ((uint32_t)impl[0] << 8));

        for (size_t size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]); size_index++) {
            for (size_t count_index = 0; count_index < count_len; count_index++) {
                for (size_t layout_index = 0; layout_index < sizeof(layouts) / sizeof(layouts[0]); layout_index++) {
                    std::vector<unsigned long> len(counts[count_index]);
                    makeLengths(&len, sizes[size_index], layouts[layout_index], &rng);
                    runOneCase(impl, func, op, sizes[size_index], layouts[layout_index], len, &rng);
                }
            }
        }
    }
#endif
};

TEST_F(BitopSimdTest, AvailabilityCheck) {
    bool has_simd = false;
#if HAVE_X86_SIMD
    has_simd = true;
    bool has_avx2 = cpuHasAvx2();
    std::printf("AVX2 available: %s\n", has_avx2 ? "yes" : "no");
    RecordProperty("avx2", has_avx2 ? "available" : "unavailable");
#endif
#if defined(__aarch64__) && HAVE_ARM_NEON
    has_simd = true;
    std::printf("NEON available: AArch64 base ABI\n");
    RecordProperty("neon", "AArch64 base ABI");
#endif
    if (!has_simd) GTEST_SKIP() << "SIMD BITOP helpers are not compiled for this architecture";
}

#if HAVE_X86_SIMD
TEST_F(BitopSimdTest, BitopAndAvx2) {
    if (!cpuHasAvx2()) GTEST_SKIP() << "AVX2 is not available on this CPU";
    runCases("AVX2", bitopAndAVX2, BITOP_AND);
}

TEST_F(BitopSimdTest, BitopOrAvx2) {
    if (!cpuHasAvx2()) GTEST_SKIP() << "AVX2 is not available on this CPU";
    runCases("AVX2", bitopOrAVX2, BITOP_OR);
}

TEST_F(BitopSimdTest, BitopXorAvx2) {
    if (!cpuHasAvx2()) GTEST_SKIP() << "AVX2 is not available on this CPU";
    runCases("AVX2", bitopXorAVX2, BITOP_XOR);
}

TEST_F(BitopSimdTest, BitopNotAvx2) {
    if (!cpuHasAvx2()) GTEST_SKIP() << "AVX2 is not available on this CPU";
    runCases("AVX2", bitopNotAVX2, BITOP_NOT);
}
#endif

#if HAVE_ARM_NEON
TEST_F(BitopSimdTest, BitopAndNeon) {
    runCases("NEON", bitopAndNEON, BITOP_AND);
}

TEST_F(BitopSimdTest, BitopOrNeon) {
    runCases("NEON", bitopOrNEON, BITOP_OR);
}

TEST_F(BitopSimdTest, BitopXorNeon) {
    runCases("NEON", bitopXorNEON, BITOP_XOR);
}

TEST_F(BitopSimdTest, BitopNotNeon) {
    runCases("NEON", bitopNotNEON, BITOP_NOT);
}
#endif
