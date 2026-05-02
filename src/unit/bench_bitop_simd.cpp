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
#include <time.h>
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

static const int BITOP_AND = 0;
static const int BITOP_OR = 1;
static const int BITOP_XOR = 2;
static const int BITOP_NOT = 3;

typedef void (*BitopBenchFunc)(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen);

struct BenchLcg {
    explicit BenchLcg(uint32_t seed) :
        state(seed) {
    }

    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }

    unsigned char nextByte() {
        return (unsigned char)(next() >> 24);
    }

    uint32_t state;
};

struct BenchResult {
    double ns_per_byte;
    double gb_per_sec;
};

static volatile uint64_t bitop_bench_sink = 0;

static const char *opName(int op) {
    switch (op) {
    case BITOP_AND: return "AND";
    case BITOP_OR: return "OR";
    case BITOP_XOR: return "XOR";
    case BITOP_NOT: return "NOT";
    default: return "UNKNOWN";
    }
}

static const char *sizeName(unsigned long size) {
    switch (size) {
    case 4UL * 1024: return "4KB";
    case 16UL * 1024: return "16KB";
    case 64UL * 1024: return "64KB";
    case 256UL * 1024: return "256KB";
    case 1024UL * 1024: return "1MB";
    case 4UL * 1024 * 1024: return "4MB";
    default: return "unknown";
    }
}

static void bitopScalarOracle(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long maxlen, int op) {
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

static void bitopAndScalar(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen) {
    (void)minlen;
    bitopScalarOracle(dst, src, len, numkeys, maxlen, BITOP_AND);
}

static void bitopOrScalar(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen) {
    (void)minlen;
    bitopScalarOracle(dst, src, len, numkeys, maxlen, BITOP_OR);
}

static void bitopXorScalar(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen) {
    (void)minlen;
    bitopScalarOracle(dst, src, len, numkeys, maxlen, BITOP_XOR);
}

static void bitopNotScalar(unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen) {
    (void)minlen;
    bitopScalarOracle(dst, src, len, numkeys, maxlen, BITOP_NOT);
}

static BitopBenchFunc scalarFuncForOp(int op) {
    switch (op) {
    case BITOP_AND: return bitopAndScalar;
    case BITOP_OR: return bitopOrScalar;
    case BITOP_XOR: return bitopXorScalar;
    case BITOP_NOT: return bitopNotScalar;
    default: return nullptr;
    }
}

static uint64_t monotonicNs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static double runIterations(BitopBenchFunc func, unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen, unsigned long iterations) {
    uint64_t start = monotonicNs();
    for (unsigned long i = 0; i < iterations; i++) {
        func(dst, src, len, numkeys, minlen, maxlen);
        bitop_bench_sink += dst[i % maxlen];
    }
    uint64_t end = monotonicNs();
    return (double)(end - start);
}

static unsigned long calibratedIterations(BitopBenchFunc func, unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen) {
    const double kCalibrationNs = 20000000.0;
    const double kTargetNs = 500000000.0;
    const unsigned long kMaxIterations = 1UL << 30;
    unsigned long iterations = 1;
    double elapsed_ns = 0.0;

    while (iterations < kMaxIterations) {
        elapsed_ns = runIterations(func, dst, src, len, numkeys, minlen, maxlen, iterations);
        if (elapsed_ns >= kCalibrationNs) break;
        iterations *= 2;
    }

    if (elapsed_ns <= 0.0) return iterations;

    double scaled = ((double)iterations * kTargetNs) / elapsed_ns;
    if (scaled < 1.0) return 1;
    if (scaled > (double)kMaxIterations) return kMaxIterations;
    return (unsigned long)scaled;
}

static BenchResult measureImpl(BitopBenchFunc func, unsigned char *dst, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen) {
    runIterations(func, dst, src, len, numkeys, minlen, maxlen, 10);
    unsigned long iterations = calibratedIterations(func, dst, src, len, numkeys, minlen, maxlen);
    double elapsed_ns = runIterations(func, dst, src, len, numkeys, minlen, maxlen, iterations);
    double bytes = (double)maxlen * (double)iterations;
    BenchResult result = {elapsed_ns / bytes, bytes / elapsed_ns};
    return result;
}

static void verifyImpl(BitopBenchFunc func, int op, unsigned char **src, unsigned long *len, unsigned long numkeys, unsigned long minlen, unsigned long maxlen) {
    std::vector<unsigned char> expected(maxlen);
    std::vector<unsigned char> actual(maxlen);
    bitopScalarOracle(expected.data(), src, len, numkeys, maxlen, op);
    func(actual.data(), src, len, numkeys, minlen, maxlen);

    if (std::memcmp(expected.data(), actual.data(), maxlen) != 0) {
        for (unsigned long i = 0; i < maxlen; i++) {
            if (expected[i] != actual[i]) {
                ASSERT_EQ((int)expected[i], (int)actual[i]) << "op=" << opName(op) << " offset=" << i;
            }
        }
    }
}

static void printBenchLine(const char *impl, int op, unsigned long size, unsigned long sources, BenchResult result, double scalar_ns_per_byte) {
    double speedup = scalar_ns_per_byte / result.ns_per_byte;
    std::printf("BENCH bitop op=%s size=%s sources=%lu impl=%s ns_per_byte=%.3f GB_per_sec=%.3f "
                "speedup_vs_scalar=%.2fx\n",
                opName(op), sizeName(size), sources, impl, result.ns_per_byte, result.gb_per_sec, speedup);
}

#if HAVE_X86_SIMD
static bool cpuHasAvx2() {
    return __builtin_cpu_supports("avx2") != 0;
}

static BitopBenchFunc avx2FuncForOp(int op) {
    switch (op) {
    case BITOP_AND: return bitopAndAVX2;
    case BITOP_OR: return bitopOrAVX2;
    case BITOP_XOR: return bitopXorAVX2;
    case BITOP_NOT: return bitopNotAVX2;
    default: return nullptr;
    }
}
#endif

#if HAVE_ARM_NEON
static BitopBenchFunc neonFuncForOp(int op) {
    switch (op) {
    case BITOP_AND: return bitopAndNEON;
    case BITOP_OR: return bitopOrNEON;
    case BITOP_XOR: return bitopXorNEON;
    case BITOP_NOT: return bitopNotNEON;
    default: return nullptr;
    }
}
#endif

TEST(BitopSimdBench, HelperThroughput) {
    if (std::getenv("BITOP_RUN_BENCH") == nullptr) GTEST_SKIP() << "Set BITOP_RUN_BENCH=1 to run";

    bool has_runtime_simd = false;
#if HAVE_X86_SIMD
    has_runtime_simd = has_runtime_simd || cpuHasAvx2();
#endif
#if HAVE_ARM_NEON
    has_runtime_simd = true;
#endif
    if (!has_runtime_simd) GTEST_SKIP() << "No SIMD BITOP helper is available at runtime";

    static const unsigned long sizes[] = {4UL * 1024, 16UL * 1024, 64UL * 1024,
                                          256UL * 1024, 1024UL * 1024, 4UL * 1024 * 1024};
    static const int ops[] = {BITOP_AND, BITOP_OR, BITOP_XOR, BITOP_NOT};

    for (size_t op_index = 0; op_index < sizeof(ops) / sizeof(ops[0]); op_index++) {
        int op = ops[op_index];
        unsigned long sources = (op == BITOP_NOT) ? 1 : 2;
        for (size_t size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]); size_index++) {
            unsigned long size = sizes[size_index];
            BenchLcg rng(0xb17c0000u ^ (uint32_t)op ^ (uint32_t)size);
            std::vector<std::vector<unsigned char>> srcbuf(sources);
            std::vector<unsigned char *> src(sources);
            std::vector<unsigned long> len(sources, size);
            for (unsigned long i = 0; i < sources; i++) {
                srcbuf[i].resize(size);
                for (unsigned long j = 0; j < size; j++) {
                    srcbuf[i][j] = rng.nextByte();
                }
                src[i] = srcbuf[i].data();
            }

            unsigned long minlen = size;
            unsigned long maxlen = size;
            BitopBenchFunc scalar_func = scalarFuncForOp(op);
            ASSERT_NE(scalar_func, nullptr);

            std::vector<unsigned char> dst(size);
            BenchResult scalar_result = measureImpl(scalar_func, dst.data(), src.data(), len.data(), sources, minlen,
                                                    maxlen);
            printBenchLine("SCALAR", op, size, sources, scalar_result, scalar_result.ns_per_byte);

#if HAVE_X86_SIMD
            if (cpuHasAvx2()) {
                BitopBenchFunc avx2_func = avx2FuncForOp(op);
                ASSERT_NE(avx2_func, nullptr);
                verifyImpl(avx2_func, op, src.data(), len.data(), sources, minlen, maxlen);
                BenchResult avx2_result = measureImpl(avx2_func, dst.data(), src.data(), len.data(), sources, minlen,
                                                      maxlen);
                printBenchLine("AVX2", op, size, sources, avx2_result, scalar_result.ns_per_byte);
            }
#endif

#if HAVE_ARM_NEON
            BitopBenchFunc neon_func = neonFuncForOp(op);
            ASSERT_NE(neon_func, nullptr);
            verifyImpl(neon_func, op, src.data(), len.data(), sources, minlen, maxlen);
            BenchResult neon_result = measureImpl(neon_func, dst.data(), src.data(), len.data(), sources, minlen,
                                                  maxlen);
            printBenchLine("NEON", op, size, sources, neon_result, scalar_result.ns_per_byte);
#endif
        }
    }

    RecordProperty("bitop_bench_sink", (int)(bitop_bench_sink & 0x7fffffff));
    std::fflush(stdout);
}
