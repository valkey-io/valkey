/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include "config.h"
#include "fmacros.h"
#include "zmalloc.h"

#if HAVE_X86_SIMD
extern void bitopAndSSE2(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
extern void bitopOrSSE2(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
extern void bitopXorSSE2(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
extern void bitopNotSSE2(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);
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
static const unsigned long MAX_SOURCES = 64;
static const size_t GUARD_SIZE = 64;
static const unsigned char GUARD_BYTE = 0xcc;

typedef void (*BitopSimdFunc)(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);

enum BitopLayout {
    LAYOUT_EQUAL,
    LAYOUT_ASCENDING,
    LAYOUT_DESCENDING,
    LAYOUT_RANDOM_MIXED,
    LAYOUT_WITH_EMPTY,
};

static const char *opName(int op) {
    static const char *names[] = {"AND", "OR", "XOR", "NOT"};
    return names[op];
}

/* Keep each backend's random inputs identical and reproducible. */
static uint32_t nextRandom(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void bitopScalarOracle(unsigned char *dst, const unsigned char *const *src, const unsigned long *len, unsigned long numkeys, unsigned long maxlen, int op) {
    for (unsigned long j = 0; j < maxlen; j++) {
        unsigned char output = len[0] <= j ? 0 : src[0][j];
        if (op == BITOP_NOT) {
            dst[j] = (unsigned char)~output;
            continue;
        }
        for (unsigned long i = 1; i < numkeys; i++) {
            unsigned char byte = len[i] <= j ? 0 : src[i][j];
            switch (op) {
            case BITOP_AND: output &= byte; break;
            case BITOP_OR: output |= byte; break;
            case BITOP_XOR: output ^= byte; break;
            }
        }
        dst[j] = output;
    }
}

/* Separate nonempty mixed inputs from empty sources so random cases exercise
 * both vectorized common prefixes and zero-extension fallbacks. */
static void makeLengths(unsigned long *len, unsigned long numkeys, unsigned long size, BitopLayout layout, uint32_t *state) {
    unsigned long lower = (size + 1) / 2;
    for (unsigned long i = 0; i < numkeys; i++) {
        switch (layout) {
        case LAYOUT_EQUAL: len[i] = size; break;
        case LAYOUT_ASCENDING: len[i] = size * (i + 1) / numkeys; break;
        case LAYOUT_DESCENDING: len[i] = size * (numkeys - i) / numkeys; break;
        case LAYOUT_RANDOM_MIXED: len[i] = lower + nextRandom(state) % (size - lower + 1); break;
        case LAYOUT_WITH_EMPTY: len[i] = nextRandom(state) % (size + 1); break;
        }
    }
    if (layout == LAYOUT_RANDOM_MIXED || layout == LAYOUT_WITH_EMPTY) {
        unsigned long index = nextRandom(state) % numkeys;
        len[index] = layout == LAYOUT_WITH_EMPTY ? 0 : lower;
        if (numkeys > 1) len[(index + 1) % numkeys] = size;
    }
}

/* Use EXPECT so result or guard failures still release the allocated buffers. */
static void checkResult(BitopSimdFunc func, int op, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long dst_offset) {
    unsigned long minlen = len[0], maxlen = len[0];
    const unsigned char *oracle_src[MAX_SOURCES];
    for (unsigned long i = 0; i < numkeys; i++) {
        if (len[i] < minlen) minlen = len[i];
        if (len[i] > maxlen) maxlen = len[i];
        oracle_src[i] = src[i];
    }
    unsigned char *expected = (unsigned char *)zmalloc(maxlen ? maxlen : 1);
    size_t bufsize = maxlen + 2 * GUARD_SIZE + dst_offset;
    unsigned char *buffer = (unsigned char *)zmalloc(bufsize);
    unsigned long start = GUARD_SIZE + dst_offset;
    unsigned char *dst = buffer + start;
    memset(buffer, GUARD_BYTE, bufsize);
    memset(dst, 0xa5, maxlen);
    bitopScalarOracle(expected, oracle_src, len, numkeys, maxlen, op);
    func(dst, src, len, numkeys, minlen, maxlen);

    EXPECT_EQ(0, memcmp(dst, expected, maxlen));
    for (size_t i = 0; i < start; i++) {
        EXPECT_EQ(GUARD_BYTE, buffer[i]) << "leading guard offset=" << i;
    }
    for (size_t i = start + maxlen; i < bufsize; i++) {
        EXPECT_EQ(GUARD_BYTE, buffer[i]) << "trailing guard offset=" << i;
    }
    zfree(buffer);
    zfree(expected);
}

static void runOneCase(BitopSimdFunc func, int op, unsigned long *len, unsigned long numkeys, unsigned long src_offset, unsigned long dst_offset, uint32_t *state) {
    unsigned char *buffers[MAX_SOURCES];
    unsigned char *src[MAX_SOURCES];
    for (unsigned long i = 0; i < numkeys; i++) {
        unsigned long offset = (src_offset + i * 5) % 32;
        buffers[i] = len[i] ? (unsigned char *)zmalloc(len[i] + offset) : NULL;
        src[i] = len[i] ? buffers[i] + offset : NULL;
        for (unsigned long j = 0; j < len[i]; j++) {
            src[i][j] = (unsigned char)(nextRandom(state) >> 24);
        }
    }
    checkResult(func, op, src, len, numkeys, dst_offset);
    for (unsigned long i = 0; i < numkeys; i++) zfree(buffers[i]);
}

static void runCases(const char *impl, BitopSimdFunc func, int op) {
    static const unsigned long sizes[] = {0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127,
                                          128, 129, 255, 256, 257, 511, 512, 4095, 4096, 4097};
    static const unsigned long counts[] = {1, 2, 3, 4, 8, 15, 16, 17, 33, 64};
    static const BitopLayout layouts[] = {LAYOUT_EQUAL, LAYOUT_ASCENDING, LAYOUT_DESCENDING,
                                          LAYOUT_RANDOM_MIXED, LAYOUT_WITH_EMPTY};
    uint32_t state = 0x5eed0000u ^ (uint32_t)op;
    unsigned long len[MAX_SOURCES];
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
            unsigned long numkeys = counts[c];
            if (op == BITOP_NOT && numkeys != 1) continue;
            for (size_t l = 0; l < sizeof(layouts) / sizeof(layouts[0]); l++) {
                char trace[192];
                snprintf(trace, sizeof(trace), "impl=%s op=%s size=%lu numkeys=%lu layout=%d state=%u",
                         impl, opName(op), sizes[s], numkeys, (int)layouts[l], state);
                SCOPED_TRACE(trace);
                makeLengths(len, numkeys, sizes[s], layouts[l], &state);
                runOneCase(func, op, len, numkeys, 0, 0, &state);
            }
        }
    }
}

/* Every backend sees each source and destination alignment modulo 32. */
static void runUnalignedCases(const char *impl, const BitopSimdFunc *kernels) {
    static const unsigned long sizes[] = {0, 1, 15, 16, 17, 31, 32, 33, 127, 128, 129, 255, 256, 257};
    uint32_t state = 0x5eed1234u;
    for (int op = BITOP_AND; op <= BITOP_NOT; op++) {
        unsigned long numkeys = op == BITOP_NOT ? 1 : 3;
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            for (int mixed = 0; mixed < 2; mixed++) {
                for (unsigned long src_offset = 0; src_offset < 32; src_offset++) {
                    for (unsigned long dst_offset = 0; dst_offset < 32; dst_offset++) {
                        unsigned long len[3];
                        char trace[192];
                        snprintf(trace, sizeof(trace), "impl=%s op=%s size=%lu mixed=%d src_offset=%lu dst_offset=%lu state=%u",
                                 impl, opName(op), sizes[s], mixed, src_offset, dst_offset, state);
                        SCOPED_TRACE(trace);
                        makeLengths(len, numkeys, sizes[s], mixed ? LAYOUT_RANDOM_MIXED : LAYOUT_EQUAL, &state);
                        runOneCase(kernels[op], op, len, numkeys, src_offset, dst_offset, &state);
                    }
                }
            }
        }
    }
}

/* Guard pages detect reads outside each source, even when ASan is disabled.
 * Read-only source pages also catch accidental writes to the inputs. */
static void runInputBoundsCases(const char *impl, const BitopSimdFunc *kernels) {
    long page_size = sysconf(_SC_PAGESIZE);
    ASSERT_GT(page_size, 0);
    static const BitopLayout layouts[] = {LAYOUT_EQUAL, LAYOUT_RANDOM_MIXED, LAYOUT_WITH_EMPTY};
    const unsigned long sizes[] = {0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129,
                                   255, 256, 257, (unsigned long)page_size - 1,
                                   (unsigned long)page_size, (unsigned long)page_size + 1};
    uint32_t state = 0x5eed5678u;
    for (int op = BITOP_AND; op <= BITOP_NOT; op++) {
        unsigned long numkeys = op == BITOP_NOT ? 1 : 3;
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            unsigned long size = sizes[s];
            size_t readable = (size / page_size + 1) * page_size;
            size_t stride = readable + 2 * page_size;
            size_t mapping_size = stride * numkeys;
            for (size_t l = 0; l < sizeof(layouts) / sizeof(layouts[0]); l++) {
                for (int at_end = 0; at_end < 2; at_end++) {
                    char trace[192];
                    snprintf(trace, sizeof(trace), "impl=%s op=%s size=%lu layout=%d at_end=%d state=%u",
                             impl, opName(op), size, (int)layouts[l], at_end, state);
                    SCOPED_TRACE(trace);
                    unsigned char *mapping = (unsigned char *)mmap(NULL, mapping_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    ASSERT_NE(MAP_FAILED, mapping);
                    unsigned char *src[3];
                    unsigned long len[3];
                    makeLengths(len, numkeys, size, layouts[l], &state);
                    for (unsigned long i = 0; i < numkeys; i++) {
                        unsigned char *data = mapping + i * stride + page_size;
                        if (mprotect(data, readable, PROT_READ | PROT_WRITE) != 0) {
                            munmap(mapping, mapping_size);
                            FAIL() << "mprotect source buffer failed";
                        }
                        src[i] = at_end ? data + readable - len[i] : data;
                        if (len[i] == 0) src[i] = mapping + i * stride;
                        for (unsigned long j = 0; j < len[i]; j++) {
                            src[i][j] = (unsigned char)(nextRandom(&state) >> 24);
                        }
                        if (mprotect(data, readable, PROT_READ) != 0) {
                            munmap(mapping, mapping_size);
                            FAIL() << "mprotect source buffer read-only failed";
                        }
                    }
                    checkResult(kernels[op], op, src, len, numkeys, 1);
                    EXPECT_EQ(0, munmap(mapping, mapping_size));
                }
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

class BitopSimdTest : public ::testing::Test {};

TEST_F(BitopSimdTest, AvailabilityCheck) {
#if HAVE_X86_SIMD
    printf("SSE2 available: x86-64 baseline; AVX2 available: %s\n", cpuHasAvx2() ? "yes" : "no");
#elif HAVE_ARM_NEON
    printf("NEON available: AArch64 baseline\n");
#else
    GTEST_SKIP() << "SIMD BITOP helpers are not compiled for this architecture";
#endif
}

#if HAVE_X86_SIMD
TEST_F(BitopSimdTest, BitopAndSse2) {
    runCases("SSE2", bitopAndSSE2, BITOP_AND);
}

TEST_F(BitopSimdTest, BitopOrSse2) {
    runCases("SSE2", bitopOrSSE2, BITOP_OR);
}

TEST_F(BitopSimdTest, BitopXorSse2) {
    runCases("SSE2", bitopXorSSE2, BITOP_XOR);
}

TEST_F(BitopSimdTest, BitopNotSse2) {
    runCases("SSE2", bitopNotSSE2, BITOP_NOT);
}

TEST_F(BitopSimdTest, Sse2UnalignedBuffers) {
    static const BitopSimdFunc kernels[] = {bitopAndSSE2, bitopOrSSE2, bitopXorSSE2, bitopNotSSE2};
    runUnalignedCases("SSE2", kernels);
}

TEST_F(BitopSimdTest, Sse2InputBounds) {
    static const BitopSimdFunc kernels[] = {bitopAndSSE2, bitopOrSSE2, bitopXorSSE2, bitopNotSSE2};
    runInputBoundsCases("SSE2", kernels);
}

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

TEST_F(BitopSimdTest, Avx2UnalignedBuffers) {
    if (!cpuHasAvx2()) GTEST_SKIP() << "AVX2 is not available on this CPU";
    static const BitopSimdFunc kernels[] = {bitopAndAVX2, bitopOrAVX2, bitopXorAVX2, bitopNotAVX2};
    runUnalignedCases("AVX2", kernels);
}

TEST_F(BitopSimdTest, Avx2InputBounds) {
    if (!cpuHasAvx2()) GTEST_SKIP() << "AVX2 is not available on this CPU";
    static const BitopSimdFunc kernels[] = {bitopAndAVX2, bitopOrAVX2, bitopXorAVX2, bitopNotAVX2};
    runInputBoundsCases("AVX2", kernels);
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

TEST_F(BitopSimdTest, NeonUnalignedBuffers) {
    static const BitopSimdFunc kernels[] = {bitopAndNEON, bitopOrNEON, bitopXorNEON, bitopNotNEON};
    runUnalignedCases("NEON", kernels);
}

TEST_F(BitopSimdTest, NeonInputBounds) {
    static const BitopSimdFunc kernels[] = {bitopAndNEON, bitopOrNEON, bitopXorNEON, bitopNotNEON};
    runInputBoundsCases("NEON", kernels);
}
#endif
