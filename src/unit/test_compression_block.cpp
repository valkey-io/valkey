/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Tests for the value compression backends. See src/compression_block_interface.h. */

#include "generated_wrappers.hpp"

#include <string.h>

extern "C" {
#include "compression_block_interface.h"
#include "server.h"
#include "zmalloc.h"
}

/* zmalloc.h defines helper macros (__str/__xstr) that collide with libstdc++
 * internals. Keep them local to C headers in this C++ translation unit. */
#ifdef __xstr
#undef __xstr
#endif
#ifdef __str
#undef __str
#endif

/* Sample data. */
#define SAMPLE_COUNT 64
#define SAMPLE_CAP 512

typedef struct blockCompressorTestData {
    char value[SAMPLE_COUNT][SAMPLE_CAP];
    size_t len[SAMPLE_COUNT];
} blockCompressorTestData;

/* Builds JSON-like values that share their field names, which is the shape this
 * feature targets. */
static void blockCompressorTestDataInit(blockCompressorTestData *d) {
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        int n = snprintf(d->value[i],
                         SAMPLE_CAP,
                         "{\"user_id\":%d,\"user_name\":\"person_%d\","
                         "\"email_address\":\"person_%d@example.com\","
                         "\"created_at\":\"2026-09-10T11:0%d:00Z\",\"is_active\":true,"
                         "\"role\":\"member\",\"preferences\":{\"language\":\"en\","
                         "\"timezone\":\"UTC\",\"theme\":\"dark\"},\"login_count\":%d,"
                         "\"last_seen_at\":\"2026-09-10T11:0%d:00Z\"}",
                         i,
                         i,
                         i,
                         i % 10,
                         i * 7,
                         i % 10);
        d->len[i] = (size_t)n;
    }
}

/* ===== construction ===== */

TEST(BlockCompressor, NewCompressorLz4) {
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    EXPECT_STREQ(c->name, "lz4");
    EXPECT_EQ(c->id, ALGO_LZ4);
    EXPECT_TRUE(c->api != NULL);
    EXPECT_TRUE(c->state != NULL);
    EXPECT_TRUE(c->max_output_size != NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_NONE);
    /* A NULL config means no size limits. */
    EXPECT_EQ(c->config.min_input_len, (size_t)0);
    EXPECT_EQ(c->config.max_input_len, (size_t)0);
    blockCompressorFree(c);
}

TEST(BlockCompressor, NewCompressorCopiesConfig) {
    blockCompressorConfig cfg;
    cfg.min_input_len = 256;
    cfg.max_input_len = 131072;

    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, &cfg);
    ASSERT_TRUE(c != NULL);
    EXPECT_EQ(c->config.min_input_len, (size_t)256);
    EXPECT_EQ(c->config.max_input_len, (size_t)131072);

    /* The config is copied, so changing the caller's copy must not reach it. */
    cfg.min_input_len = 1;
    cfg.max_input_len = 2;
    EXPECT_EQ(c->config.min_input_len, (size_t)256);
    EXPECT_EQ(c->config.max_input_len, (size_t)131072);
    blockCompressorFree(c);
}

TEST(BlockCompressor, NewCompressorRejectsUnavailableBackends) {
    /* off has no backend at all. */
    EXPECT_TRUE(blockCompressorCreate(ALGO_NONE, NULL) == NULL);
    /* zstd is not vendored yet, so it has no function table. */
    EXPECT_TRUE(blockCompressorCreate(ALGO_ZSTD, NULL) == NULL);
    /* An id outside the enum must not be treated as a valid backend. */
    EXPECT_TRUE(blockCompressorCreate((compressionAlgo)99, NULL) == NULL);
}

TEST(BlockCompressor, FreeCompressorAcceptsNull) {
    blockCompressorFree(NULL);
}

TEST(BlockCompressor, Strerror) {
    EXPECT_TRUE(blockCompressorStrerror(BLOCK_COMPRESSOR_ERR_NONE) != NULL);
    EXPECT_TRUE(blockCompressorStrerror(BLOCK_COMPRESSOR_ERR_BAD_SIZE) != NULL);
    EXPECT_TRUE(blockCompressorStrerror(BLOCK_COMPRESSOR_ERR_COMPRESS) != NULL);
    EXPECT_TRUE(blockCompressorStrerror(BLOCK_COMPRESSOR_ERR_DECOMPRESS) != NULL);
    /* Every code, known or not, maps to text. */
    EXPECT_TRUE(blockCompressorStrerror(12345) != NULL);
    /* Two different codes must not read the same. */
    EXPECT_STRNE(blockCompressorStrerror(BLOCK_COMPRESSOR_ERR_COMPRESS), blockCompressorStrerror(BLOCK_COMPRESSOR_ERR_DECOMPRESS));
}

/* ===== max_output_size ===== */

TEST(BlockCompressor, MaxOutputSizeAppliesConfigRange) {
    blockCompressorConfig cfg;
    cfg.min_input_len = 256;
    cfg.max_input_len = 131072;
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, &cfg);
    ASSERT_TRUE(c != NULL);

    /* Below the low end and above the high end are both refused. */
    EXPECT_EQ(c->max_output_size(c, 1), (size_t)0);
    EXPECT_EQ(c->max_output_size(c, 255), (size_t)0);
    EXPECT_EQ(c->max_output_size(c, 131073), (size_t)0);

    /* Both ends of the range are inclusive. */
    EXPECT_GT(c->max_output_size(c, 256), (size_t)0);
    EXPECT_GT(c->max_output_size(c, 131072), (size_t)0);

    blockCompressorFree(c);
}

TEST(BlockCompressor, MaxOutputSizeZeroMeansNoLimit) {
    blockCompressorConfig cfg;
    cfg.min_input_len = 0;
    cfg.max_input_len = 0;
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, &cfg);
    ASSERT_TRUE(c != NULL);

    /* With both limits at 0 only the backend's own limits apply. */
    EXPECT_GT(c->max_output_size(c, 1), (size_t)0);
    EXPECT_GT(c->max_output_size(c, 100), (size_t)0);
    EXPECT_GT(c->max_output_size(c, 10 * 1024 * 1024), (size_t)0);

    blockCompressorFree(c);
}

TEST(BlockCompressor, MaxOutputSizeRespectsBackendLimits) {
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    /* An empty value is never compressed, so 0 is refused even with no config. */
    EXPECT_EQ(c->max_output_size(c, 0), (size_t)0);

    /* LZ4_MAX_INPUT_SIZE is 0x7E000000. One byte past it must be refused. */
    EXPECT_GT(c->max_output_size(c, 0x7E000000), (size_t)0);
    EXPECT_EQ(c->max_output_size(c, (size_t)0x7E000000 + 1), (size_t)0);

    blockCompressorFree(c);
}

TEST(BlockCompressor, MaxOutputSizeIsAtLeastTheInput) {
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    /* The bound covers data that does not compress at all, so it can never be
     * smaller than the input. It also grows with the input. */
    size_t sizes[5] = {1, 256, 1024, 65536, 131072};
    size_t previous = 0;
    for (int i = 0; i < 5; i++) {
        size_t bound = c->max_output_size(c, sizes[i]);
        EXPECT_GE(bound, sizes[i]);
        EXPECT_GT(bound, previous);
        previous = bound;
    }
    blockCompressorFree(c);
}

/* ===== round trip ===== */

TEST(BlockCompressor, RoundTrip) {
    blockCompressorTestData *d = (blockCompressorTestData *)zmalloc(sizeof(*d));
    blockCompressorTestDataInit(d);

    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        size_t bound = c->max_output_size(c, d->len[i]);
        ASSERT_GT(bound, (size_t)0);
        char *out = (char *)zmalloc(bound);
        char *back = (char *)zmalloc(d->len[i]);

        size_t n = c->api->compress(c, d->value[i], d->len[i], out, bound);
        EXPECT_GT(n, (size_t)0);
        EXPECT_LE(n, bound);
        EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_NONE);

        size_t back_len = c->api->decompress(c, out, n, back, d->len[i]);
        EXPECT_EQ(back_len, d->len[i]);
        EXPECT_EQ(memcmp(back, d->value[i], d->len[i]), 0);

        zfree(out);
        zfree(back);
    }
    blockCompressorFree(c);
    zfree(d);
}

/* A worker compresses and the main thread decompresses, each with its own
 * instance. Here two instances stand in for two threads. */
TEST(BlockCompressor, OneInstanceDecompressesWhatAnotherCompressed) {
    blockCompressorTestData *d = (blockCompressorTestData *)zmalloc(sizeof(*d));
    blockCompressorTestDataInit(d);

    blockCompressor *writer = blockCompressorCreate(ALGO_LZ4, NULL);
    blockCompressor *reader = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(writer != NULL);
    ASSERT_TRUE(reader != NULL);
    EXPECT_TRUE(writer->state != reader->state); /* separate scratch buffers */

    size_t bound = writer->max_output_size(writer, d->len[40]);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(d->len[40]);

    size_t n = writer->api->compress(writer, d->value[40], d->len[40], out, bound);
    EXPECT_GT(n, (size_t)0);

    size_t back_len = reader->api->decompress(reader, out, n, back, d->len[40]);
    EXPECT_EQ(back_len, d->len[40]);
    EXPECT_EQ(memcmp(back, d->value[40], d->len[40]), 0);

    zfree(out);
    zfree(back);
    blockCompressorFree(writer);
    blockCompressorFree(reader);
    zfree(d);
}

/* One instance alternating between a large value and a small one on the same
 * working stream. */
TEST(BlockCompressor, MixedLargeAndSmallValuesOnOneInstance) {
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    size_t big_len = 20000, small_len = 300;
    char *big = (char *)zmalloc(big_len);
    char *small = (char *)zmalloc(small_len);
    memset(big, 'q', big_len);
    memset(small, 'r', small_len);

    for (int round = 0; round < 3; round++) {
        size_t big_bound = c->max_output_size(c, big_len);
        size_t small_bound = c->max_output_size(c, small_len);
        char *big_out = (char *)zmalloc(big_bound);
        char *small_out = (char *)zmalloc(small_bound);
        char *big_back = (char *)zmalloc(big_len);
        char *small_back = (char *)zmalloc(small_len);

        size_t bn = c->api->compress(c, big, big_len, big_out, big_bound);
        size_t sn = c->api->compress(c, small, small_len, small_out, small_bound);
        EXPECT_GT(bn, (size_t)0);
        EXPECT_GT(sn, (size_t)0);

        EXPECT_EQ(c->api->decompress(c, big_out, bn, big_back, big_len), big_len);
        EXPECT_EQ(memcmp(big_back, big, big_len), 0);
        EXPECT_EQ(c->api->decompress(c, small_out, sn, small_back, small_len), small_len);
        EXPECT_EQ(memcmp(small_back, small, small_len), 0);

        zfree(big_out);
        zfree(small_out);
        zfree(big_back);
        zfree(small_back);
    }
    zfree(big);
    zfree(small);
    blockCompressorFree(c);
}

/* ===== failure paths ===== */

TEST(BlockCompressor, CompressFailsWhenDestinationTooSmall) {
    blockCompressorTestData *d = (blockCompressorTestData *)zmalloc(sizeof(*d));
    blockCompressorTestDataInit(d);

    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    char tiny[4];
    size_t n = c->api->compress(c, d->value[0], d->len[0], tiny, sizeof(tiny));
    EXPECT_EQ(n, (size_t)0);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_COMPRESS);

    /* The instance must still work after a failure, and the error must clear. */
    size_t bound = c->max_output_size(c, d->len[0]);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    EXPECT_GT(c->api->compress(c, d->value[0], d->len[0], out, bound), (size_t)0);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_NONE);

    zfree(out);
    blockCompressorFree(c);
    zfree(d);
}

TEST(BlockCompressor, CompressRejectsEmptyInput) {
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    char out[64];
    EXPECT_EQ(c->api->compress(c, "abc", 0, out, sizeof(out)), (size_t)0);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_BAD_SIZE);
    blockCompressorFree(c);
}

TEST(BlockCompressor, DecompressRejectsBadSizes) {
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    char out[64];

    EXPECT_EQ(c->api->decompress(c, "abc", 0, out, sizeof(out)), (size_t)0);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_BAD_SIZE);

    EXPECT_EQ(c->api->decompress(c, "abc", 3, out, 0), (size_t)0);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_BAD_SIZE);

    blockCompressorFree(c);
}

TEST(BlockCompressor, DecompressFailsOnCorruptBody) {
    blockCompressorTestData *d = (blockCompressorTestData *)zmalloc(sizeof(*d));
    blockCompressorTestDataInit(d);

    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    size_t bound = c->max_output_size(c, d->len[0]);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(d->len[0]);
    size_t n = c->api->compress(c, d->value[0], d->len[0], out, bound);
    ASSERT_GT(n, (size_t)0);

    /* Damage the body. LZ4's safe decoder must refuse it or return the wrong
     * length. It must never write past dstcap, which the sanitizer builds check. */
    out[n / 2] = (char)(out[n / 2] ^ 0xFF);
    size_t back_len = c->api->decompress(c, out, n, back, d->len[0]);
    if (back_len == d->len[0]) {
        /* A one-bit change can still decode to a valid but different value. */
        EXPECT_NE(memcmp(back, d->value[0], d->len[0]), 0);
    } else {
        EXPECT_EQ(back_len, (size_t)0);
        EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_DECOMPRESS);
    }

    zfree(out);
    zfree(back);
    blockCompressorFree(c);
    zfree(d);
}

/* ===== data shapes ===== */

TEST(BlockCompressor, RoundTripOnIncompressibleData) {
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    /* Random bytes do not shrink. The frame may grow, which is what the bound is
     * for, and the net-savings guard above this layer throws such frames away. */
    size_t len = 4096;
    char *val = (char *)zmalloc(len);
    for (size_t i = 0; i < len; i++) val[i] = (char)(rand() & 0xFF);

    size_t bound = c->max_output_size(c, len);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(len);

    size_t n = c->api->compress(c, val, len, out, bound);
    EXPECT_GT(n, (size_t)0);
    EXPECT_LE(n, bound);
    EXPECT_EQ(c->api->decompress(c, out, n, back, len), len);
    EXPECT_EQ(memcmp(back, val, len), 0);

    zfree(val);
    zfree(out);
    zfree(back);
    blockCompressorFree(c);
}

TEST(BlockCompressor, RoundTripOnHighlyCompressibleData) {
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    size_t len = 8192;
    char *val = (char *)zmalloc(len);
    memset(val, 'a', len);

    size_t bound = c->max_output_size(c, len);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(len);

    size_t n = c->api->compress(c, val, len, out, bound);
    EXPECT_GT(n, (size_t)0);
    EXPECT_LT(n * 100, len); /* well under 1% of the original */
    EXPECT_EQ(c->api->decompress(c, out, n, back, len), len);
    EXPECT_EQ(memcmp(back, val, len), 0);

    zfree(val);
    zfree(out);
    zfree(back);
    blockCompressorFree(c);
}

TEST(BlockCompressor, RoundTripAtSizeBoundaries) {
    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    /* One byte, the smallest the backend accepts, up to the 128 KiB cap. */
    size_t sizes[6] = {1, 2, 255, 256, 65535, 131072};
    for (int i = 0; i < 6; i++) {
        size_t len = sizes[i];
        char *val = (char *)zmalloc(len);
        for (size_t j = 0; j < len; j++) val[j] = (char)('a' + (j % 26));

        size_t bound = c->max_output_size(c, len);
        ASSERT_GT(bound, (size_t)0);
        char *out = (char *)zmalloc(bound);
        char *back = (char *)zmalloc(len);

        size_t n = c->api->compress(c, val, len, out, bound);
        EXPECT_GT(n, (size_t)0);
        EXPECT_EQ(c->api->decompress(c, out, n, back, len), len);
        EXPECT_EQ(memcmp(back, val, len), 0);

        zfree(val);
        zfree(out);
        zfree(back);
    }
    blockCompressorFree(c);
}

/* The same instance is reused for many values in a row, so a leftover from one
 * call must never change the next. */
TEST(BlockCompressor, RepeatedUseOfOneInstanceIsStable) {
    blockCompressorTestData *d = (blockCompressorTestData *)zmalloc(sizeof(*d));
    blockCompressorTestDataInit(d);

    blockCompressor *c = blockCompressorCreate(ALGO_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    size_t bound = c->max_output_size(c, d->len[40]);
    ASSERT_GT(bound, (size_t)0);
    char *first = (char *)zmalloc(bound);
    char *again = (char *)zmalloc(bound);

    size_t n1 = c->api->compress(c, d->value[40], d->len[40], first, bound);
    ASSERT_GT(n1, (size_t)0);

    /* Compress other values in between, then the same value once more. The output
     * must be byte for byte what it was the first time. */
    for (int i = 41; i < SAMPLE_COUNT; i++) {
        char *scratch = (char *)zmalloc(c->max_output_size(c, d->len[i]));
        c->api->compress(c, d->value[i], d->len[i], scratch, c->max_output_size(c, d->len[i]));
        zfree(scratch);
    }
    size_t n2 = c->api->compress(c, d->value[40], d->len[40], again, bound);
    EXPECT_EQ(n2, n1);
    EXPECT_EQ(memcmp(again, first, n1), 0);

    zfree(first);
    zfree(again);
    blockCompressorFree(c);
    zfree(d);
}
