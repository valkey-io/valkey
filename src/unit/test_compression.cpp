/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Tests for the compression streaming and rio decorator layers. */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>

extern "C" {
#include "../../deps/lz4/lz4frame.h"
#include "compression.h"
#include "compression_rio.h"
#include "compression_stream.h"
#include "zmalloc.h"
}

/* zmalloc.h defines helper macros (__str/__xstr) that collide with libstdc++ internals.
 * Keep them local to C headers in this C++ translation unit. */
#ifdef __xstr
#undef __xstr
#endif
#ifdef __str
#undef __str
#endif

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    size_t max_chunk; /* 0 => unbounded */
} mem_reader_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    size_t max_chunk;
    int fail_after_success_reads;
    int success_reads;
} flaky_reader_t;

static stream_reader_config_t makeReaderConfig(uint8_t expected_stream_kind,
                                               bool allow_passthrough,
                                               size_t buffer_size) {
    stream_reader_config_t cfg = {};
    cfg.expected_stream_kind = expected_stream_kind;
    cfg.allow_passthrough = allow_passthrough;
    cfg.buffer_size = buffer_size;
    return cfg;
}

static stream_writer_config_t makeWriterConfig(compression_algo_t algo,
                                               int level,
                                               uint8_t stream_kind,
                                               bool codec_checksum_enabled = false) {
    stream_writer_config_t cfg = {};
    cfg.algo = algo;
    cfg.level = level;
    cfg.stream_kind = stream_kind;
    cfg.codec_checksum_enabled = codec_checksum_enabled;
    return cfg;
}

static bool lz4FrameHasBlockChecksum(const uint8_t *data, size_t len) {
    LZ4F_dctx *dctx = NULL;
    EXPECT_FALSE(LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION)));
    if (dctx == NULL) return false;

    LZ4F_frameInfo_t frame_info = {};
    size_t src_size = len;
    size_t ret = LZ4F_getFrameInfo(dctx, &frame_info, data, &src_size);
    bool has_block_checksum = !LZ4F_isError(ret) &&
                              frame_info.blockChecksumFlag == LZ4F_blockChecksumEnabled;
    LZ4F_freeDecompressionContext(dctx);
    return has_block_checksum;
}

static ssize_t memReaderRead(void *ctx, void *buf, size_t len) {
    mem_reader_t *r = (mem_reader_t *)ctx;
    if (!r || !buf) return -1;
    if (r->pos >= r->len) return 0;

    size_t avail = r->len - r->pos;
    size_t n = len < avail ? len : avail;
    if (r->max_chunk && n > r->max_chunk) n = r->max_chunk;

    memcpy(buf, r->data + r->pos, n);
    r->pos += n;
    return (ssize_t)n;
}

static ssize_t flakyReaderRead(void *ctx, void *buf, size_t len) {
    flaky_reader_t *r = (flaky_reader_t *)ctx;
    if (!r || !buf) return -1;
    if (r->success_reads >= r->fail_after_success_reads) return -1;
    if (r->pos >= r->len) return 0;

    size_t avail = r->len - r->pos;
    size_t n = len < avail ? len : avail;
    if (r->max_chunk && n > r->max_chunk) n = r->max_chunk;

    memcpy(buf, r->data + r->pos, n);
    r->pos += n;
    r->success_reads++;
    return (ssize_t)n;
}

/* ===================================================================
 * Streaming compression/decompression tests
 * =================================================================== */

/* --- Test: LZ4 compressor init/destroy lifecycle --- */
TEST(compression, streamCompressorInitDestroy) {
    stream_compressor_t sc;
    ASSERT_TRUE(streamCompressorInit(&sc, ALGO_LZ4, 0) == 0) << "LZ4 init should succeed";
    ASSERT_TRUE(sc.algo == ALGO_LZ4) << "algo should be LZ4";
    ASSERT_TRUE(sc.stream_started == false) << "stream_started should be false";
    ASSERT_TRUE(sc.ctx != NULL) << "ctx should be non-NULL";
    streamCompressorDestroy(&sc);
    ASSERT_TRUE(sc.ctx == NULL) << "ctx should be NULL after destroy";
    ASSERT_TRUE(sc.algo == ALGO_NONE) << "algo should be NONE after destroy";

    /* ALGO_NONE should fail */
    stream_compressor_t sc3;
    ASSERT_TRUE(streamCompressorInit(&sc3, ALGO_NONE, 0) == -1) << "NONE init should fail";

    /* NULL should fail */
    ASSERT_TRUE(streamCompressorInit(NULL, ALGO_LZ4, 0) == -1) << "NULL init should fail";
}

/* --- Test: LZ4 decompressor init/destroy lifecycle --- */
TEST(compression, streamDecompressorInitDestroy) {
    stream_decompressor_t sd;
    ASSERT_TRUE(streamDecompressorInit(&sd, ALGO_LZ4) == 0) << "LZ4 decomp init should succeed";
    ASSERT_TRUE(sd.algo == ALGO_LZ4) << "algo should be LZ4";
    ASSERT_TRUE(sd.ctx != NULL) << "ctx should be non-NULL";
    streamDecompressorDestroy(&sd);
    ASSERT_TRUE(sd.ctx == NULL) << "ctx should be NULL after destroy";
    ASSERT_TRUE(sd.algo == ALGO_NONE) << "algo should be NONE after destroy";

    return;
}

/* --- Test: LZ4 compress → decompress round-trip --- */
TEST(compression, streamCompressDecompressRoundTrip) {
    const char *input = "Hello, Valkey compression module! This is a test payload.";
    size_t input_len = strlen(input);

    /* Compress */
    stream_compressor_t sc;
    ASSERT_TRUE(streamCompressorInit(&sc, ALGO_LZ4, 0) == 0);

    size_t bound = streamCompressOutputBound(&sc, input_len);
    ASSERT_TRUE(bound > 0) << "bound should be > 0";

    uint8_t *compressed = (uint8_t *)zmalloc(bound);
    ASSERT_TRUE(compressed != NULL);
    ssize_t compressed_len = streamCompressFeed(&sc, compressed, bound,
                                                (const uint8_t *)input, input_len,
                                                FLUSH_END);
    ASSERT_TRUE(compressed_len > 0) << "compress should succeed";
    ASSERT_TRUE(sc.stream_started == false) << "frame should be closed after FLUSH_END";
    streamCompressorDestroy(&sc);

    /* Decompress */
    stream_decompressor_t sd;
    ASSERT_TRUE(streamDecompressorInit(&sd, ALGO_LZ4) == 0);

    uint8_t decompressed[256];
    size_t input_consumed = 0;
    ssize_t decompressed_len = streamDecompressFeed(&sd, decompressed, sizeof(decompressed),
                                                    compressed, (size_t)compressed_len,
                                                    &input_consumed);
    ASSERT_TRUE(decompressed_len > 0) << "decompress should succeed";
    ASSERT_TRUE((size_t)decompressed_len == input_len) << "decompressed length should match input";
    ASSERT_TRUE(memcmp(decompressed, input, input_len) == 0) << "decompressed content should match input";
    ASSERT_TRUE(input_consumed == (size_t)compressed_len) << "all compressed input should be consumed";

    streamDecompressorDestroy(&sd);
    zfree(compressed);
    return;
}

/* --- Test: streamCompressOutputBound returns sane values --- */
TEST(compression, streamCompressOutputBound) {
    stream_compressor_t sc;
    ASSERT_TRUE(streamCompressorInit(&sc, ALGO_LZ4, 0) == 0);

    /* Basic: bound for 1KB input should be > 0 */
    size_t b1 = streamCompressOutputBound(&sc, 1024);
    ASSERT_TRUE(b1 > 0) << "bound for 1KB should be > 0";

    /* Bound is always conservative (includes frame header + flush overhead),
     * so it should be stable regardless of frame state. */
    size_t b_before = streamCompressOutputBound(&sc, 1024);
    uint8_t *seed_buf = (uint8_t *)zmalloc(b_before);
    ASSERT_TRUE(seed_buf != NULL);
    ASSERT_TRUE(streamCompressFeed(&sc, seed_buf, b_before,
                                   (const uint8_t *)"x", 1, FLUSH_CONTINUE) >= 0)
        << "seed write should start the frame";
    size_t b_after = streamCompressOutputBound(&sc, 1024);
    ASSERT_TRUE(b_before == b_after) << "bound should be the same before and after frame start";

    /* Zero input should still return > 0 (frame header + flush overhead) */
    size_t b_zero = streamCompressOutputBound(&sc, 0);
    ASSERT_TRUE(b_zero > 0) << "zero input bound should be > 0";

    zfree(seed_buf);
    streamCompressorDestroy(&sc);
    return;
}

/* --- Test: streamCompressFeed error paths --- */
TEST(compression, streamCompressFeedErrors) {
    uint8_t buf[64];
    /* NULL compressor */
    ASSERT_TRUE(streamCompressFeed(NULL, buf, sizeof(buf),
                                   (const uint8_t *)"x", 1, FLUSH_CONTINUE) == -1)
        << "NULL sc should return -1";

    /* NULL output buffer */
    stream_compressor_t sc;
    ASSERT_TRUE(streamCompressorInit(&sc, ALGO_LZ4, 0) == 0);
    ASSERT_TRUE(streamCompressFeed(&sc, NULL, sizeof(buf),
                                   (const uint8_t *)"x", 1, FLUSH_CONTINUE) == -1)
        << "NULL output should return -1";
    streamCompressorDestroy(&sc);

    return;
}

/* --- Test: streamDecompressFeed error paths --- */
TEST(compression, streamDecompressFeedErrors) {
    const char *payload = "decompress sticky error";
    uint8_t buf[64];
    uint8_t out[128];
    size_t consumed = 0;

    /* NULL decompressor */
    ASSERT_TRUE(streamDecompressFeed(NULL, buf, sizeof(buf),
                                     (const uint8_t *)"x", 1, &consumed) == -1)
        << "NULL sd should return -1";

    /* NULL input_consumed */
    stream_decompressor_t sd;
    ASSERT_TRUE(streamDecompressorInit(&sd, ALGO_LZ4) == 0);
    ASSERT_TRUE(streamDecompressFeed(&sd, buf, sizeof(buf),
                                     (const uint8_t *)"x", 1, NULL) == -1)
        << "NULL input_consumed should return -1";
    ASSERT_TRUE(sd.errored == false) << "decompressor should not be errored by NULL bookkeeping arg";

    /* Zero output capacity should return -1 (no-progress prevention) */
    ASSERT_TRUE(streamDecompressFeed(&sd, buf, 0,
                                     (const uint8_t *)"x", 1, &consumed) == -1)
        << "zero output capacity should return -1";
    ASSERT_TRUE(sd.errored == true) << "decompressor should enter sticky errored state";

    /* Once errored, all subsequent feeds fail immediately. */
    stream_compressor_t sc;
    ASSERT_TRUE(streamCompressorInit(&sc, ALGO_LZ4, 0) == 0);
    size_t bound = streamCompressOutputBound(&sc, strlen(payload));
    uint8_t *compressed = (uint8_t *)zmalloc(bound);
    ASSERT_TRUE(compressed != NULL);
    ssize_t compressed_len = streamCompressFeed(&sc, compressed, bound,
                                                (const uint8_t *)payload, strlen(payload),
                                                FLUSH_END);
    ASSERT_TRUE(compressed_len > 0);
    streamCompressorDestroy(&sc);

    ASSERT_TRUE(streamDecompressFeed(&sd, out, sizeof(out),
                                     compressed, (size_t)compressed_len,
                                     &consumed) == -1)
        << "errored decompressor should fail even with valid input";
    zfree(compressed);

    streamDecompressorDestroy(&sd);
    return;
}

/* --- Test: pre-frame errors are recoverable, mid-frame errors are permanent --- */
TEST(compression, streamCompressFeedErrorRecovery) {
    stream_compressor_t sc;
    ASSERT_TRUE(streamCompressorInit(&sc, ALGO_LZ4, 0) == 0);

    /* Pre-frame error: compressBegin fails with tiny buffer, but no frame
     * bytes have been emitted yet — this is recoverable. */
    uint8_t tiny[1];
    ssize_t ret = streamCompressFeed(&sc, tiny, 1,
                                     (const uint8_t *)"test data", 9, FLUSH_END);
    ASSERT_TRUE(ret == -1) << "should fail with tiny buffer";
    ASSERT_TRUE(sc.errored == false) << "errored should NOT be set (pre-frame failure)";
    ASSERT_TRUE(sc.stream_started == false) << "stream_started should still be false";

    /* Retry with a proper buffer — should succeed */
    size_t bound = streamCompressOutputBound(&sc, 9);
    uint8_t *buf = (uint8_t *)zmalloc(bound);
    ssize_t ret2 = streamCompressFeed(&sc, buf, bound,
                                      (const uint8_t *)"test data", 9, FLUSH_END);
    ASSERT_TRUE(ret2 > 0) << "retry after pre-frame error should succeed";
    zfree(buf);
    streamCompressorDestroy(&sc);

    /* Mid-frame error: start a frame, then force an error — this is permanent. */
    stream_compressor_t sc2;
    ASSERT_TRUE(streamCompressorInit(&sc2, ALGO_LZ4, 0) == 0);

    /* First call with enough space to start the frame */
    size_t bound2 = streamCompressOutputBound(&sc2, 5);
    uint8_t *buf2 = (uint8_t *)zmalloc(bound2);
    ssize_t ret3 = streamCompressFeed(&sc2, buf2, bound2,
                                      (const uint8_t *)"hello", 5, FLUSH_CONTINUE);
    ASSERT_TRUE(ret3 >= 0) << "first write should succeed";
    ASSERT_TRUE(sc2.stream_started == true) << "stream should be started";

    /* Now force a mid-frame error with a tiny buffer */
    uint8_t tiny2[1];
    ssize_t ret4 = streamCompressFeed(&sc2, tiny2, 1,
                                      (const uint8_t *)"more data to compress", 21,
                                      FLUSH_END);
    ASSERT_TRUE(ret4 == -1) << "mid-frame error should fail";
    ASSERT_TRUE(sc2.errored == true) << "errored should be set (mid-frame failure)";

    /* Subsequent calls must fail immediately */
    size_t bound3 = streamCompressOutputBound(&sc2, 5);
    uint8_t *buf3 = (uint8_t *)zmalloc(bound3);
    ssize_t ret5 = streamCompressFeed(&sc2, buf3, bound3,
                                      (const uint8_t *)"hello", 5, FLUSH_END);
    ASSERT_TRUE(ret5 == -1) << "must fail on errored compressor";
    zfree(buf3);
    zfree(buf2);
    streamCompressorDestroy(&sc2);

    return;
}

TEST(compression, streamReaderClassifiesProbeInputs) {
    static const uint8_t plain_input[] = {'H', 'E', 'L', 'L', 'O'};
    static const uint8_t truncated_vkcs[] = {'V', 'K', 'C'};
    static const uint8_t invalid_vkcs[VKCS_ENVELOPE_SIZE] = {
        VKCS_MAGIC_0, VKCS_MAGIC_1, VKCS_MAGIC_2, VKCS_MAGIC_3,
        0, VKCS_CODEC_LZ4, 0, STREAM_KIND_RDB};
    static const uint8_t strict_non_vkcs[VKCS_ENVELOPE_SIZE] = {'R', 'E', 'D', 'I', 'S', '0', '0', '1'};

    struct {
        const char *name;
        const uint8_t *input;
        size_t input_len;
        size_t max_chunk;
        bool allow_passthrough;
        bool expect_probe_ok;
        stream_reader_error_t expected_error;
        size_t expected_read_len;
    } cases[] = {
        {"plain passthrough",
         plain_input,
         sizeof(plain_input),
         2,
         true,
         true,
         STREAM_READER_ERROR_NONE,
         sizeof(plain_input)},
        {"truncated VKCS prefix",
         truncated_vkcs,
         sizeof(truncated_vkcs),
         2,
         true,
         false,
         STREAM_READER_ERROR_INCOMPATIBLE,
         0},
        {"invalid VKCS envelope",
         invalid_vkcs,
         sizeof(invalid_vkcs),
         0,
         true,
         false,
         STREAM_READER_ERROR_INCOMPATIBLE,
         0},
        {"non-VKCS with passthrough disabled",
         strict_non_vkcs,
         sizeof(strict_non_vkcs),
         0,
         false,
         false,
         STREAM_READER_ERROR_INCOMPATIBLE,
         0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mem_reader_t mr = {};
        mr.data = cases[i].input;
        mr.len = cases[i].input_len;
        mr.max_chunk = cases[i].max_chunk;
        stream_reader_config_t cfg = makeReaderConfig(STREAM_KIND_RDB, cases[i].allow_passthrough, 0);
        stream_reader_t *t = stream_reader_create(&cfg, memReaderRead, &mr);
        ASSERT_TRUE(t != NULL) << cases[i].name;

        stream_reader_info_t info;
        if (cases[i].expect_probe_ok) {
            ASSERT_TRUE(stream_reader_probe(t) == 0) << cases[i].name;
            ASSERT_TRUE(stream_reader_get_info(t, &info) == 0) << cases[i].name;
            ASSERT_TRUE(info.compressed == 0) << cases[i].name;

            uint8_t out[16] = {0};
            ASSERT_TRUE(stream_reader_read(t, out, cases[i].expected_read_len) == (ssize_t)cases[i].expected_read_len)
                << cases[i].name;
            ASSERT_TRUE(memcmp(out, cases[i].input, cases[i].expected_read_len) == 0) << cases[i].name;
            ASSERT_TRUE(stream_reader_read(t, out, sizeof(out)) == 0) << cases[i].name;
        } else {
            ASSERT_TRUE(stream_reader_probe(t) == -1) << cases[i].name;
            ASSERT_TRUE(stream_reader_get_info(t, &info) == -1) << cases[i].name;
            ASSERT_TRUE(stream_reader_get_error(t) == cases[i].expected_error) << cases[i].name;

            uint8_t out[8] = {0};
            ASSERT_TRUE(stream_reader_read(t, out, sizeof(out)) == -1) << cases[i].name;
        }

        stream_reader_destroy(t);
    }
    return;
}

TEST(compression, streamReaderRejectsOversizedReadRequest) {
    const uint8_t input[] = {'H', 'E', 'L', 'L', 'O'};
    mem_reader_t mr = {};
    mr.data = input;
    mr.len = sizeof(input);
    mr.max_chunk = 2;
    stream_reader_config_t cfg = makeReaderConfig(STREAM_KIND_RDB, true, 0);

    stream_reader_t *t = stream_reader_create(&cfg, memReaderRead, &mr);
    ASSERT_TRUE(t != NULL) << "stream_reader_create should succeed";

    uint8_t out[8] = {0};
    size_t oversized = (size_t)std::numeric_limits<ssize_t>::max() + 1;
    ASSERT_TRUE(stream_reader_read(t, out, oversized) == -1)
        << "oversized reads should fail before touching stream state";

    stream_reader_info_t info;
    ASSERT_TRUE(stream_reader_get_info(t, &info) == 0)
        << "oversized read failure should not poison the reader";
    ASSERT_TRUE(info.compressed == 0) << "plain input should still probe as passthrough";

    ASSERT_TRUE(stream_reader_read(t, out, sizeof(input)) == (ssize_t)sizeof(input));
    ASSERT_TRUE(memcmp(out, input, sizeof(input)) == 0) << "subsequent valid read should still succeed";

    stream_reader_destroy(t);
    return;
}

/* ===================================================================
 * Tests for stream writer API and rio decorators
 * =================================================================== */

extern "C" {
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len);
}

/* --- Emit callback that appends to a dynamically growing buffer --- */
typedef struct {
    uint8_t *data;
} dynamic_buf_t;

static void dynamicBufInit(dynamic_buf_t *db) {
    db->data = (uint8_t *)sdsempty();
}

static void dynamicBufFree(dynamic_buf_t *db) {
    if (db->data) sdsfree((sds)db->data);
    db->data = NULL;
}

static int emitToDynamicBuf(void *ctx, const uint8_t *data, size_t len) {
    dynamic_buf_t *db = (dynamic_buf_t *)ctx;
    db->data = (uint8_t *)sdscatlen((sds)db->data, data, len);
    return db->data != NULL ? 0 : -1;
}

static int initVkcsRdbDecompressRio(decompress_rio_t *dr, rio *inner) {
    stream_reader_config_t cfg = makeReaderConfig(STREAM_KIND_RDB, false, 0);
    return rioInitWithDecompress(dr, inner, &cfg, NULL) == DECOMPRESS_RIO_INIT_OK ? 0 : -1;
}

TEST(compression, streamReaderValidatesCompressedStreamKinds) {
    struct {
        const char *name;
        const char *payload;
        uint8_t writer_kind;
        uint8_t expected_kind;
        size_t max_chunk;
        size_t buffer_size;
        bool expect_ok;
    } cases[] = {
        {"incremental RDB stream",
         "incremental probe payload",
         STREAM_KIND_RDB,
         STREAM_KIND_RDB,
         3,
         8,
         true},
        {"custom stream kind",
         "custom stream kind",
         0x7f,
         0x7f,
         0,
         0,
         true},
        {"custom stream when RDB expected",
         "stream-kind mismatch",
         0x7f,
         STREAM_KIND_RDB,
         0,
         0,
         false},
        {"RDB stream when custom expected",
         "stream-kind mismatch",
         STREAM_KIND_RDB,
         0x7f,
         0,
         0,
         false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t payload_len = strlen(cases[i].payload);
        dynamic_buf_t db;
        dynamicBufInit(&db);

        stream_writer_config_t wcfg = makeWriterConfig(ALGO_LZ4, 0, cases[i].writer_kind);
        stream_writer_t *w = stream_writer_create(&wcfg, emitToDynamicBuf, &db);
        ASSERT_TRUE(w != NULL) << cases[i].name;
        ASSERT_TRUE(stream_writer_write(w, cases[i].payload, payload_len) >= 0) << cases[i].name;
        ASSERT_TRUE(stream_writer_finish(w) == 0) << cases[i].name;
        stream_writer_destroy(w);

        mem_reader_t mr = {};
        mr.data = db.data;
        mr.len = sdslen((const char *)db.data);
        mr.max_chunk = cases[i].max_chunk;
        stream_reader_config_t rcfg = makeReaderConfig(cases[i].expected_kind, true, cases[i].buffer_size);
        stream_reader_t *r = stream_reader_create(&rcfg, memReaderRead, &mr);
        ASSERT_TRUE(r != NULL) << cases[i].name;

        stream_reader_info_t info;
        if (cases[i].expect_ok) {
            ASSERT_TRUE(stream_reader_probe(r) == 0) << cases[i].name;
            ASSERT_TRUE(stream_reader_get_info(r, &info) == 0) << cases[i].name;
            ASSERT_TRUE(info.compressed) << cases[i].name;
            ASSERT_TRUE(info.algo == ALGO_LZ4) << cases[i].name;
            ASSERT_TRUE(info.stream_kind == cases[i].expected_kind) << cases[i].name;

            uint8_t out[64] = {0};
            ASSERT_TRUE(stream_reader_read(r, out, payload_len) == (ssize_t)payload_len) << cases[i].name;
            ASSERT_TRUE(memcmp(out, cases[i].payload, payload_len) == 0) << cases[i].name;
            ASSERT_TRUE(stream_reader_read(r, out, sizeof(out)) == 0) << cases[i].name;
        } else {
            ASSERT_TRUE(stream_reader_probe(r) == -1) << cases[i].name;
            ASSERT_TRUE(stream_reader_get_info(r, &info) == -1) << cases[i].name;
            ASSERT_TRUE(stream_reader_get_error(r) == STREAM_READER_ERROR_INCOMPATIBLE) << cases[i].name;

            uint8_t out[32] = {0};
            ASSERT_TRUE(stream_reader_read(r, out, sizeof(out)) == -1) << cases[i].name;
        }

        stream_reader_destroy(r);
        dynamicBufFree(&db);
    }
    return;
}

/* --- Test: stream_reader marks errored on partial output + read error.
 * Regression for direct-path error accounting (partial bytes were returned
 * without sticky errored state). */
TEST(compression, streamReaderPartialThenErrorSetsErrored) {
    const size_t payload_len = 64 * 1024;
    uint8_t *payload = (uint8_t *)zmalloc(payload_len);
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < payload_len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        payload[i] = (uint8_t)(x & 0xFF);
    }

    dynamic_buf_t db;
    dynamicBufInit(&db);
    stream_writer_config_t wcfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *w = stream_writer_create(&wcfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(w != NULL) << "stream_writer_create should succeed";
    ASSERT_TRUE(stream_writer_write(w, payload, payload_len) >= 0);
    ASSERT_TRUE(stream_writer_finish(w) == 0);
    stream_writer_destroy(w);

    flaky_reader_t fr = {};
    fr.data = db.data;
    fr.len = sdslen((const char *)db.data);
    fr.max_chunk = 4096;
    /* One probe read plus two 4 KB payload reads is enough to produce some
     * output with an 8 KB window, but not enough to satisfy the full request. */
    fr.fail_after_success_reads = 3;
    stream_reader_config_t rcfg = makeReaderConfig(STREAM_KIND_RDB, false, 8 * 1024);
    stream_reader_t *r = stream_reader_create(&rcfg, flakyReaderRead, &fr);
    ASSERT_TRUE(r != NULL) << "stream_reader_create should succeed";

    const size_t out_len = 16 * 1024;
    uint8_t *out = (uint8_t *)zmalloc(out_len);
    ASSERT_TRUE(out != NULL);
    ssize_t n1 = stream_reader_read(r, out, out_len);
    ASSERT_TRUE(n1 > 0) << "first read should return partial output";
    ASSERT_TRUE(stream_reader_read(r, out, out_len) == -1) << "second read should fail immediately";

    stream_reader_destroy(r);

    /* Passthrough mode should also preserve partial bytes when source read
     * fails after probe/prefix buffering, then latch sticky error state. */
    const uint8_t plain[] = "NOTVKCS-passthrough-regression";
    flaky_reader_t fr_passthrough = {};
    fr_passthrough.data = plain;
    fr_passthrough.len = sizeof(plain) - 1;
    fr_passthrough.max_chunk = 0;
    fr_passthrough.fail_after_success_reads = 1; /* probe succeeds, next read fails */
    stream_reader_config_t pass_cfg = makeReaderConfig(STREAM_KIND_RDB, true, 0);
    stream_reader_t *rp = stream_reader_create(&pass_cfg, flakyReaderRead, &fr_passthrough);
    ASSERT_TRUE(rp != NULL) << "passthrough reader create should succeed";

    uint8_t pass_out[64];
    ssize_t p1 = stream_reader_read(rp, pass_out, sizeof(pass_out));
    ASSERT_TRUE(p1 > 0) << "passthrough first read should return partial output";
    ASSERT_TRUE(memcmp(pass_out, plain, (size_t)p1) == 0) << "passthrough partial bytes should match input prefix";
    ASSERT_TRUE(stream_reader_read(rp, pass_out, sizeof(pass_out)) == -1) << "passthrough second read should fail immediately";
    stream_reader_destroy(rp);
    zfree(out);

    dynamicBufFree(&db);
    zfree(payload);
    return;
}

/* --- Test: stream_writer_create/destroy --- */
TEST(compression, streamWriterCreateDestroy) {
    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL) << "create should succeed for LZ4";
    ASSERT_TRUE(stream_writer_is_errored(t) == 0) << "should not be errored";

    stream_writer_destroy(t);
    dynamicBufFree(&db);

    /* NULL config should fail */
    ASSERT_TRUE(stream_writer_create(NULL, emitToDynamicBuf, &db) == NULL) << "NULL config should return NULL";

    /* NULL emit_cb should fail */
    ASSERT_TRUE(stream_writer_create(&cfg, NULL, NULL) == NULL) << "NULL emit_cb should return NULL";

    /* ALGO_NONE should fail */
    stream_writer_config_t bad_cfg = makeWriterConfig(ALGO_NONE, 0, STREAM_KIND_RDB);
    ASSERT_TRUE(stream_writer_create(&bad_cfg, emitToDynamicBuf, &db) == NULL) << "ALGO_NONE should return NULL";
    bad_cfg = makeWriterConfig(ALGO_LZF, 0, STREAM_KIND_RDB);
    ASSERT_TRUE(stream_writer_create(&bad_cfg, emitToDynamicBuf, &db) == NULL) << "ALGO_LZF should return NULL";

    /* Concrete stream kinds outside the currently named ones are valid. */
    stream_writer_config_t future_kind_cfg = makeWriterConfig(ALGO_LZ4, 0, 0x7f);
    stream_writer_t *future_t = stream_writer_create(&future_kind_cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(future_t != NULL) << "custom stream_kind should succeed with envelope";
    stream_writer_destroy(future_t);

    /* destroy NULL should be safe */
    stream_writer_destroy(NULL);

    return;
}

/* --- Test: stream_writer write + finish round-trip --- */
TEST(compression, streamWriterRoundTrip) {
    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);

    /* Write some data */
    const char *test_data = "Hello, compression world! This is a test of the stream writer API.";
    size_t data_len = strlen(test_data);
    ASSERT_TRUE(stream_writer_write(t, test_data, data_len) >= 0);
    ASSERT_TRUE(stream_writer_is_errored(t) == 0) << "should not be errored after write";
    ASSERT_TRUE(sdslen((const char *)db.data) >= VKCS_ENVELOPE_SIZE) << "write should emit envelope";

    /* Finalize */
    ASSERT_TRUE(stream_writer_finish(t) == 0);
    ASSERT_TRUE(stream_writer_is_errored(t) == 0) << "should not be errored after finish";

    /* Verify output starts with VKCS envelope */
    ASSERT_TRUE(sdslen((const char *)db.data) >= VKCS_ENVELOPE_SIZE) << "output should have at least envelope size";
    ASSERT_TRUE(db.data[0] == VKCS_MAGIC_0) << "magic byte 0";
    ASSERT_TRUE(db.data[1] == VKCS_MAGIC_1) << "magic byte 1";
    ASSERT_TRUE(db.data[2] == VKCS_MAGIC_2) << "magic byte 2";
    ASSERT_TRUE(db.data[3] == VKCS_MAGIC_3) << "magic byte 3";
    ASSERT_TRUE(db.data[4] == VKCS_VERSION) << "version";
    ASSERT_TRUE(db.data[5] == VKCS_CODEC_LZ4) << "codec_id";
    ASSERT_TRUE(db.data[6] == 0) << "flags should be clear when checksum is disabled";
    ASSERT_TRUE(db.data[7] == STREAM_KIND_RDB) << "stream_kind RDB";

    /* Decompress and verify round-trip */
    stream_decompressor_t sd;
    ASSERT_TRUE(streamDecompressorInit(&sd, ALGO_LZ4) == 0);

    uint8_t decompressed[256];
    size_t total_decompressed = 0;
    uint8_t *compressed_data = db.data + VKCS_ENVELOPE_SIZE;
    size_t compressed_len = sdslen((const char *)db.data) - VKCS_ENVELOPE_SIZE;
    size_t src_offset = 0;

    while (src_offset < compressed_len) {
        size_t consumed = 0;
        ssize_t produced = streamDecompressFeed(
            &sd, decompressed + total_decompressed,
            sizeof(decompressed) - total_decompressed,
            compressed_data + src_offset,
            compressed_len - src_offset, &consumed);
        ASSERT_TRUE(produced >= 0) << "decompression should not fail";
        total_decompressed += (size_t)produced;
        src_offset += consumed;
        if (consumed == 0 && produced == 0) break;
    }

    ASSERT_TRUE(total_decompressed == data_len) << "decompressed length should match original";
    ASSERT_TRUE(memcmp(decompressed, test_data, data_len) == 0) << "decompressed data should match original";

    streamDecompressorDestroy(&sd);
    stream_writer_destroy(t);
    dynamicBufFree(&db);
    return;
}

/* --- Test: a single large write is chunked internally without changing
 * the logical stream. This exercises the bounded scratch-buffer path. --- */
TEST(compression, streamWriterLargeSingleWrite) {
    const size_t payload_len = (1024 * 1024) + 4096;
    uint8_t *payload = (uint8_t *)zmalloc(payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)((i * 17 + 11) % 251);
    }

    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);
    ASSERT_TRUE(stream_writer_write(t, payload, payload_len) >= 0);
    ASSERT_TRUE(stream_writer_finish(t) == 0);
    stream_writer_destroy(t);

    mem_reader_t mr = {};
    mr.data = db.data;
    mr.len = sdslen((const char *)db.data);
    mr.max_chunk = 0;
    stream_reader_config_t rcfg = makeReaderConfig(STREAM_KIND_RDB, false, 64 * 1024);
    stream_reader_t *r = stream_reader_create(&rcfg, memReaderRead, &mr);
    ASSERT_TRUE(r != NULL);

    uint8_t *out = (uint8_t *)zmalloc(payload_len);
    size_t total = 0;
    while (total < payload_len) {
        ssize_t nread = stream_reader_read(r, out + total, payload_len - total);
        ASSERT_TRUE(nread > 0) << "stream_reader_read should keep making progress";
        total += (size_t)nread;
    }
    ASSERT_TRUE(memcmp(out, payload, payload_len) == 0);
    ASSERT_TRUE(stream_reader_read(r, out, 1) == 0) << "reader should stop at frame end";

    zfree(out);
    zfree(payload);
    stream_reader_destroy(r);
    dynamicBufFree(&db);
    return;
}

/* --- Test: small caller reads should still drain a compressed stream
 * correctly. This exercises the buffered decompressed window path. --- */
TEST(compression, streamReaderSmallReadsRoundTrip) {
    const size_t payload_len = 256 * 1024;
    uint8_t *payload = (uint8_t *)zmalloc(payload_len);
    ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)((i * 29 + 7) % 251);
    }

    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t wcfg = makeWriterConfig(ALGO_LZ4, -5, STREAM_KIND_RDB);
    stream_writer_t *w = stream_writer_create(&wcfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(w != NULL);
    ASSERT_TRUE(stream_writer_write(w, payload, payload_len) >= 0);
    ASSERT_TRUE(stream_writer_finish(w) == 0);
    stream_writer_destroy(w);

    mem_reader_t mr = {};
    mr.data = db.data;
    mr.len = sdslen((const char *)db.data);
    mr.max_chunk = 4096;
    stream_reader_config_t rcfg = makeReaderConfig(STREAM_KIND_RDB, false, 64 * 1024);
    stream_reader_t *r = stream_reader_create(&rcfg, memReaderRead, &mr);
    ASSERT_TRUE(r != NULL);

    uint8_t *out = (uint8_t *)zmalloc(payload_len);
    ASSERT_TRUE(out != NULL);
    size_t total = 0;
    while (total < payload_len) {
        size_t step = payload_len - total;
        if (step > 17) step = 17;
        ssize_t nread = stream_reader_read(r, out + total, step);
        ASSERT_TRUE(nread > 0) << "stream_reader_read should keep making progress";
        total += (size_t)nread;
    }

    ASSERT_TRUE(memcmp(out, payload, payload_len) == 0);
    ASSERT_TRUE(stream_reader_read(r, out, 1) == 0) << "reader should stop at frame end";

    zfree(out);
    zfree(payload);
    stream_reader_destroy(r);
    dynamicBufFree(&db);
    return;
}

/* --- Test: stream_writer_flush semantics (no-op before writes, valid mid-stream) --- */
TEST(compression, streamWriterFlushBehavior) {
    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);

    ASSERT_TRUE(stream_writer_flush(t) == 0) << "flush before write should be no-op success";
    ASSERT_TRUE(sdslen((const char *)db.data) == 0) << "flush before write should not emit bytes";

    ASSERT_TRUE(stream_writer_write(t, "first chunk", 11) >= 0);
    ASSERT_TRUE(stream_writer_flush(t) == 0);
    ASSERT_TRUE(stream_writer_write(t, "second chunk", 12) >= 0);
    ASSERT_TRUE(stream_writer_finish(t) == 0);

    ASSERT_TRUE(sdslen((const char *)db.data) > VKCS_ENVELOPE_SIZE);
    stream_decompressor_t sd;
    ASSERT_TRUE(streamDecompressorInit(&sd, ALGO_LZ4) == 0);

    uint8_t decompressed[256];
    size_t total_decompressed = 0;
    uint8_t *comp_data = db.data + VKCS_ENVELOPE_SIZE;
    size_t comp_len = sdslen((const char *)db.data) - VKCS_ENVELOPE_SIZE;
    size_t src_offset = 0;

    while (src_offset < comp_len) {
        size_t consumed = 0;
        ssize_t produced = streamDecompressFeed(
            &sd, decompressed + total_decompressed,
            sizeof(decompressed) - total_decompressed,
            comp_data + src_offset,
            comp_len - src_offset, &consumed);
        ASSERT_TRUE(produced >= 0);
        total_decompressed += (size_t)produced;
        src_offset += consumed;
        if (consumed == 0 && produced == 0) break;
    }

    ASSERT_TRUE(total_decompressed == 23);
    ASSERT_TRUE(memcmp(decompressed, "first chunksecond chunk", 23) == 0);

    streamDecompressorDestroy(&sd);
    stream_writer_destroy(t);
    dynamicBufFree(&db);
    return;
}

TEST(compression, streamWriterFlushAfterFinishIsNoop) {
    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);

    ASSERT_TRUE(stream_writer_write(t, "payload", 7) >= 0);
    ASSERT_TRUE(stream_writer_finish(t) == 0);
    size_t len_after_finish = sdslen((const char *)db.data);

    ASSERT_TRUE(stream_writer_flush(t) == 0) << "flush after finish should be a no-op success";
    ASSERT_TRUE(sdslen((const char *)db.data) == len_after_finish) << "flush after finish should not emit bytes";

    stream_writer_destroy(t);
    dynamicBufFree(&db);
    return;
}

TEST(compression, streamWriterCodecChecksumToggle) {
    const char *payload = "block checksum payload block checksum payload";

    for (bool codec_checksum : {false, true}) {
        dynamic_buf_t db;
        dynamicBufInit(&db);

        stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB,
                                                      codec_checksum);
        stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
        ASSERT_TRUE(t != NULL);
        ASSERT_TRUE(stream_writer_write(t, payload, strlen(payload)) >= 0);
        ASSERT_TRUE(stream_writer_finish(t) == 0);

        ASSERT_TRUE(sdslen((const char *)db.data) > VKCS_ENVELOPE_SIZE);
        ASSERT_TRUE(lz4FrameHasBlockChecksum(db.data + VKCS_ENVELOPE_SIZE,
                                             sdslen((const char *)db.data) - VKCS_ENVELOPE_SIZE) == codec_checksum)
            << "LZ4 frame should reflect configured codec checksum setting";

        stream_writer_destroy(t);
        dynamicBufFree(&db);
    }
}

/* --- Test: compress_rio_t write + finish round-trip --- */
TEST(compression, compressRioRoundTrip) {
    /* Use a buffer rio as the inner rio */
    sds buf = sdsempty();
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, buf);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    compress_rio_t cr;
    ASSERT_TRUE(rioInitWithCompress(&cr, &buffer_rio, &cfg) == 0);
    const char *test_data = "The quick brown fox jumps over the lazy dog. "
                            "Pack my box with five dozen liquor jugs.";
    size_t data_len = strlen(test_data);
    ASSERT_TRUE(rioWrite((rio *)&cr, test_data, data_len) != 0) << "rioWrite should succeed";

    /* Finalize and destroy */
    compress_rio_finish(&cr);

    /* Get the compressed output from the buffer rio */
    sds compressed = buffer_rio.io.buffer.ptr;
    size_t compressed_len = sdslen(compressed);
    ASSERT_TRUE(compressed_len > VKCS_ENVELOPE_SIZE) << "compressed output should exist";

    /* Verify VKCS envelope */
    ASSERT_TRUE(compressed[0] == (char)VKCS_MAGIC_0) << "magic V";
    ASSERT_TRUE(compressed[1] == (char)VKCS_MAGIC_1) << "magic K";
    ASSERT_TRUE(compressed[2] == (char)VKCS_MAGIC_2) << "magic C";
    ASSERT_TRUE(compressed[3] == (char)VKCS_MAGIC_3) << "magic S";

    /* Decompress and verify */
    stream_decompressor_t sd;
    ASSERT_TRUE(streamDecompressorInit(&sd, ALGO_LZ4) == 0);

    uint8_t decompressed[512];
    size_t total_decompressed = 0;
    uint8_t *comp_data = (uint8_t *)compressed + VKCS_ENVELOPE_SIZE;
    size_t comp_len = compressed_len - VKCS_ENVELOPE_SIZE;
    size_t src_offset = 0;

    while (src_offset < comp_len) {
        size_t consumed = 0;
        ssize_t produced = streamDecompressFeed(
            &sd, decompressed + total_decompressed,
            sizeof(decompressed) - total_decompressed,
            comp_data + src_offset,
            comp_len - src_offset, &consumed);
        ASSERT_TRUE(produced >= 0) << "decompression should not fail";
        total_decompressed += (size_t)produced;
        src_offset += consumed;
        if (consumed == 0 && produced == 0) break;
    }

    ASSERT_TRUE(total_decompressed == data_len) << "decompressed length should match";
    ASSERT_TRUE(memcmp(decompressed, test_data, data_len) == 0) << "decompressed data should match";

    streamDecompressorDestroy(&sd);
    compress_rio_destroy(&cr);
    sdsfree(compressed);
    return;
}

TEST(compression, compressRioTracksUncompressedChecksum) {
    sds buf = sdsempty();
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, buf);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    compress_rio_t cr;
    ASSERT_TRUE(rioInitWithCompress(&cr, &buffer_rio, &cfg) == 0);

    const char *payload = "checksum-payload-for-compressed-rio";
    size_t payload_len = strlen(payload);
    ASSERT_TRUE(rioWrite((rio *)&cr, payload, payload_len) != 0);

    rio expected = {};
    rioGenericUpdateChecksum(&expected, payload, payload_len);
    ASSERT_TRUE(cr.base.cksum == expected.cksum) << "compress_rio should track the checksum of uncompressed bytes";

    ASSERT_TRUE(compress_rio_finish(&cr) == 0);
    compress_rio_destroy(&cr);
    sdsfree(buffer_rio.io.buffer.ptr);
    return;
}

TEST(compression, compressRioPreservesSkipRdbChecksumFlag) {
    sds buf = sdsempty();
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, buf);
    buffer_rio.flags |= RIO_FLAG_SKIP_RDB_CHECKSUM;

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    compress_rio_t cr;
    ASSERT_TRUE(rioInitWithCompress(&cr, &buffer_rio, &cfg) == 0);
    ASSERT_TRUE((((rio *)&cr)->flags & RIO_FLAG_SKIP_RDB_CHECKSUM) != 0);

    const char *payload = "skip-checksum-payload";
    ASSERT_TRUE(rioWrite((rio *)&cr, payload, strlen(payload)) != 0);
    ASSERT_TRUE(cr.base.cksum == 0) << "skip-checksum should disable uncompressed RDB checksum tracking";

    ASSERT_TRUE(compress_rio_finish(&cr) == 0);
    compress_rio_destroy(&cr);
    sdsfree(buffer_rio.io.buffer.ptr);
    return;
}

TEST(compression, compressRioUsesCodecChecksumsInsteadOfRdbChecksumWhenEnabled) {
    sds buf = sdsempty();
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, buf);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB, true);
    compress_rio_t cr;
    ASSERT_TRUE(rioInitWithCompress(&cr, &buffer_rio, &cfg) == 0);
    ASSERT_TRUE((((rio *)&cr)->flags & RIO_FLAG_STREAMING_CODEC_CHECKSUM) != 0);

    const char *payload = "codec-checksum-enabled";
    ASSERT_TRUE(rioWrite((rio *)&cr, payload, strlen(payload)) != 0);
    ASSERT_TRUE(cr.base.cksum == 0)
        << "codec checksums should disable standard RDB checksum tracking for compressed RDB";

    ASSERT_TRUE(compress_rio_finish(&cr) == 0);
    compress_rio_destroy(&cr);
    sdsfree(buffer_rio.io.buffer.ptr);
    return;
}

TEST(compression, rioDecoratorsPreserveInnerType) {
    sds buf = sdsempty();
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, buf);

    stream_writer_config_t wcfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    compress_rio_t cr;
    ASSERT_TRUE(rioInitWithCompress(&cr, &buffer_rio, &wcfg) == 0);
    /* Decorators preserve the inner backend type via the type field.
     * Verify the decorator reports the same type as the wrapped rio. */
    ASSERT_TRUE(rioCheckType((rio *)&cr) == RIO_TYPE_BUFFER);
    compress_rio_destroy(&cr);
    sdsfree(buffer_rio.io.buffer.ptr);

    sds raw = sdsnew("plain-rdb-prefix");
    rio raw_rio;
    rioInitWithBuffer(&raw_rio, raw);
    stream_reader_config_t rcfg = makeReaderConfig(STREAM_KIND_RDB, true, 0);
    decompress_rio_t dr;
    ASSERT_TRUE(rioInitWithDecompress(&dr, &raw_rio, &rcfg, NULL) == DECOMPRESS_RIO_INIT_OK);
    ASSERT_TRUE(rioCheckType((rio *)&dr) == RIO_TYPE_BUFFER);
    decompress_rio_destroy(&dr);
    sdsfree(raw_rio.io.buffer.ptr);
}

/* --- Test: decompress_rio_t read round-trip --- */
TEST(compression, decompressRioRoundTrip) {
    /* First, produce compressed data using stream writer */
    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);

    const char *test_data = "Decompression rio test data. "
                            "This should round-trip through compress then decompress.";
    size_t data_len = strlen(test_data);
    stream_writer_write(t, test_data, data_len);
    stream_writer_finish(t);
    stream_writer_destroy(t);

    /* Create a buffer rio with the full VKCS-wrapped stream. */
    sds comp_sds = sdsnewlen(db.data, sdslen((const char *)db.data));
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, comp_sds);

    /* Create decompress rio */
    decompress_rio_t dr;
    ASSERT_TRUE(initVkcsRdbDecompressRio(&dr, &buffer_rio) == 0);

    /* Read decompressed data */
    char result[256];
    memset(result, 0, sizeof(result));
    ASSERT_TRUE(rioRead((rio *)&dr, result, data_len) != 0) << "rioRead should succeed";
    ASSERT_TRUE(memcmp(result, test_data, data_len) == 0) << "decompressed data should match original";

    decompress_rio_destroy(&dr);
    sdsfree(comp_sds);
    dynamicBufFree(&db);
    return;
}

TEST(compression, decompressRioTellTracksSourceProgress) {
    dynamic_buf_t db;
    dynamicBufInit(&db);

    std::string payload(4096, 'A');
    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);
    ASSERT_TRUE(stream_writer_write(t, payload.data(), payload.size()) >= 0);
    ASSERT_TRUE(stream_writer_finish(t) == 0);
    stream_writer_destroy(t);

    sds comp_sds = sdsnewlen(db.data, sdslen((const char *)db.data));
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, comp_sds);

    decompress_rio_t dr;
    ASSERT_TRUE(initVkcsRdbDecompressRio(&dr, &buffer_rio) == 0);

    char out[2048];
    ASSERT_TRUE(rioRead((rio *)&dr, out, sizeof(out)) != 0);
    ASSERT_TRUE(rioTell((rio *)&dr) == rioTell(&buffer_rio));
    ASSERT_TRUE((size_t)rioTell((rio *)&dr) < sizeof(out))
        << "decompress rio tell should track source bytes, not logical output bytes";

    decompress_rio_destroy(&dr);
    sdsfree(comp_sds);
    dynamicBufFree(&db);
}

TEST(compression, decompressRioClassifiesInput) {
    {
        const char *payload = "REDIS001remaining data after prefix";
        size_t payload_len = strlen(payload);
        sds buf = sdsnewlen(payload, payload_len);
        rio buffer_rio;
        rioInitWithBuffer(&buffer_rio, buf);

        stream_reader_config_t cfg = makeReaderConfig(STREAM_KIND_RDB, true, 0);
        decompress_rio_t dr;
        stream_reader_info_t info;
        ASSERT_TRUE(rioInitWithDecompress(&dr, &buffer_rio, &cfg, &info) == DECOMPRESS_RIO_INIT_OK);
        ASSERT_TRUE(info.compressed == 0) << "passthrough stream should not be compressed";

        char result[64];
        memset(result, 0, sizeof(result));
        ASSERT_TRUE(rioRead((rio *)&dr, result, payload_len) != 0) << "rioRead should succeed";
        ASSERT_TRUE(memcmp(result, payload, payload_len) == 0) << "payload should be replayed exactly";

        decompress_rio_destroy(&dr);
        sdsfree(buf);
    }

    {
        const uint8_t malformed[VKCS_ENVELOPE_SIZE] = {
            VKCS_MAGIC_0, VKCS_MAGIC_1, VKCS_MAGIC_2, VKCS_MAGIC_3,
            0, VKCS_CODEC_LZ4, 0, STREAM_KIND_RDB};
        sds buf = sdsnewlen(malformed, sizeof(malformed));
        rio buffer_rio;
        rioInitWithBuffer(&buffer_rio, buf);

        stream_reader_config_t cfg = makeReaderConfig(STREAM_KIND_RDB, true, 0);
        decompress_rio_t dr;
        ASSERT_TRUE(rioInitWithDecompress(&dr, &buffer_rio, &cfg, NULL) ==
                    DECOMPRESS_RIO_INIT_INCOMPATIBLE);

        sdsfree(buf);
    }
    return;
}

/* --- Test: compress_rio_finish is idempotent --- */
TEST(compression, compressRioFinishIdempotent) {
    sds buf = sdsempty();
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, buf);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    compress_rio_t cr;
    ASSERT_TRUE(rioInitWithCompress(&cr, &buffer_rio, &cfg) == 0);

    rioWrite((rio *)&cr, "test", 4);
    compress_rio_finish(&cr);
    size_t len_after_first = sdslen(buffer_rio.io.buffer.ptr);

    /* Second finish should be a no-op */
    compress_rio_finish(&cr);
    size_t len_after_second = sdslen(buffer_rio.io.buffer.ptr);
    ASSERT_TRUE(len_after_first == len_after_second) << "second finish should not produce more output";

    compress_rio_destroy(&cr);
    sdsfree(buffer_rio.io.buffer.ptr);
    return;
}

/* --- Test: compress_rio flush mid-stream does not end frame --- */
TEST(compression, compressRioFlushMidStream) {
    sds buf = sdsempty();
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, buf);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    compress_rio_t cr;
    ASSERT_TRUE(rioInitWithCompress(&cr, &buffer_rio, &cfg) == 0);

    /* Write some data */
    ASSERT_TRUE(rioWrite((rio *)&cr, "first chunk", 11) != 0);

    /* Flush mid-stream — should NOT end the frame */
    ASSERT_TRUE(rioFlush((rio *)&cr) != 0) << "flush should succeed";

    /* Write more data — should succeed because frame is still open */
    ASSERT_TRUE(rioWrite((rio *)&cr, "second chunk", 12) != 0) << "write after flush should succeed";

    /* Now finalize */
    compress_rio_finish(&cr);

    /* Verify the entire stream decompresses correctly */
    sds compressed = buffer_rio.io.buffer.ptr;
    size_t compressed_len = sdslen(compressed);
    ASSERT_TRUE(compressed_len > VKCS_ENVELOPE_SIZE);

    stream_decompressor_t sd;
    ASSERT_TRUE(streamDecompressorInit(&sd, ALGO_LZ4) == 0);

    uint8_t decompressed[256];
    size_t total_decompressed = 0;
    uint8_t *comp_data = (uint8_t *)compressed + VKCS_ENVELOPE_SIZE;
    size_t comp_len = compressed_len - VKCS_ENVELOPE_SIZE;
    size_t src_offset = 0;

    while (src_offset < comp_len) {
        size_t consumed = 0;
        ssize_t produced = streamDecompressFeed(
            &sd, decompressed + total_decompressed,
            sizeof(decompressed) - total_decompressed,
            comp_data + src_offset,
            comp_len - src_offset, &consumed);
        ASSERT_TRUE(produced >= 0) << "decompression should not fail";
        total_decompressed += (size_t)produced;
        src_offset += consumed;
        if (consumed == 0 && produced == 0) break;
    }

    ASSERT_TRUE(total_decompressed == 23) << "total decompressed should be 23 bytes";
    ASSERT_TRUE(memcmp(decompressed, "first chunksecond chunk", 23) == 0) << "decompressed should match concatenated input";

    streamDecompressorDestroy(&sd);
    compress_rio_destroy(&cr);
    sdsfree(compressed);
    return;
}

/* --- Test: rdbLoadProgressCallback does not cast write-side streaming rios
 * to decompress_rio_t. This protects save/async paths that also use
 * RIO_FLAG_STREAMING_COMPRESSION. --- */
TEST(compression, rdbLoadProgressCallbackStreamingGuard) {
    sds buf = sdsempty();
    rio inner;
    rioInitWithBuffer(&inner, buf);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    compress_rio_t cr;
    ASSERT_TRUE(rioInitWithCompress(&cr, &inner, &cfg) == 0);

    const char sample[] = "progress-guard";
    rdbLoadProgressCallback((rio *)&cr, sample, sizeof(sample) - 1);

    ASSERT_TRUE((cr.base.flags & RIO_FLAG_READ_ERROR) == 0) << "write-side streaming rio must not set read error";

    compress_rio_destroy(&cr);
    sdsfree(inner.io.buffer.ptr);
    return;
}

/* --- Test: decompress_rio with large payload (>64KB) exercises partial
 * consume in the large-chunk read path. Before the fix, unconsumed
 * compressed bytes were dropped between iterations, causing false EOF
 * or data corruption. --- */
TEST(compression, decompressRioLargePayload) {
    /* Generate a large payload (256KB) with a repeating pattern so
     * it's compressible but large enough to require multiple
     * decompression iterations. */
    const size_t payload_len = 256 * 1024;
    uint8_t *payload = (uint8_t *)zmalloc(payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i % 251); /* prime modulus for variety */
    }

    /* Compress via stream_writer */
    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);

    stream_writer_write(t, payload, payload_len);
    stream_writer_finish(t);
    stream_writer_destroy(t);

    /* Decompress via decompress_rio using the full VKCS-wrapped stream. */
    sds comp_sds = sdsnewlen(db.data, sdslen((const char *)db.data));
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, comp_sds);

    decompress_rio_t dr;
    ASSERT_TRUE(initVkcsRdbDecompressRio(&dr, &buffer_rio) == 0);

    /* Read in small chunks (4KB) to force multiple iterations through
     * the decompression state machine. */
    uint8_t *result = (uint8_t *)zmalloc(payload_len);
    size_t total_read = 0;
    while (total_read < payload_len) {
        size_t chunk = 4096;
        if (chunk > payload_len - total_read) chunk = payload_len - total_read;
        size_t ret = rioRead((rio *)&dr, result + total_read, chunk);
        ASSERT_TRUE(ret != 0) << "rioRead should succeed for large payload";
        total_read += chunk;
    }

    ASSERT_TRUE(memcmp(result, payload, payload_len) == 0) << "decompressed data should match original";

    decompress_rio_destroy(&dr);
    sdsfree(comp_sds);
    zfree(result);
    zfree(payload);
    dynamicBufFree(&db);
    return;
}

/* --- Test: decompressRioRead handles a large single rioRead request.
 * Verifies correctness for a single 128KB read through the buffered path. --- */
TEST(compression, decompressRioDirectPath) {
    /* Generate a large payload (128KB). */
    const size_t payload_len = 128 * 1024;
    uint8_t *payload = (uint8_t *)zmalloc(payload_len);
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)((i * 7 + 13) % 256);
    }

    /* Compress */
    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);

    stream_writer_write(t, payload, payload_len);
    stream_writer_finish(t);
    stream_writer_destroy(t);

    /* Decompress via decompress_rio with a single large read. */
    sds comp_sds = sdsnewlen(db.data, sdslen((const char *)db.data));
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, comp_sds);

    decompress_rio_t dr;
    ASSERT_TRUE(initVkcsRdbDecompressRio(&dr, &buffer_rio) == 0);

    uint8_t *result = (uint8_t *)zmalloc(payload_len);
    size_t ret = rioRead((rio *)&dr, result, payload_len);
    ASSERT_TRUE(ret != 0) << "single large rioRead should succeed";
    ASSERT_TRUE(memcmp(result, payload, payload_len) == 0) << "decompressed data should match original";

    decompress_rio_destroy(&dr);
    sdsfree(comp_sds);
    zfree(result);
    zfree(payload);
    dynamicBufFree(&db);
    return;
}

/* --- Test: stream_reader stops at the end of the compressed frame even if
 * the underlying source still has trailing bytes. This keeps caller-managed
 * framing viable on long-lived streams. --- */
TEST(compression, streamReaderStopsAtFrameEndBeforeTrailingBytes) {
    const char *payload = "stream-reader-frame-end";
    const size_t payload_len = strlen(payload);
    const char *trailer = "TRAILER-BYTES-AFTER-FRAME";
    const size_t trailer_len = strlen(trailer);

    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *w = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(w != NULL);
    ASSERT_TRUE(stream_writer_write(w, payload, payload_len) >= 0);
    ASSERT_TRUE(stream_writer_finish(w) == 0);
    stream_writer_destroy(w);

    sds input = sdsnewlen(db.data, sdslen((const char *)db.data));
    input = sdscatlen(input, trailer, trailer_len);

    mem_reader_t mr = {};
    mr.data = (const uint8_t *)input;
    mr.len = sdslen(input);
    mr.max_chunk = 3;
    stream_reader_config_t rcfg = makeReaderConfig(STREAM_KIND_RDB, false, 8);
    stream_reader_t *r = stream_reader_create(&rcfg, memReaderRead, &mr);
    ASSERT_TRUE(r != NULL);

    char out[128];
    memset(out, 0, sizeof(out));
    ASSERT_TRUE(stream_reader_read(r, out, payload_len) == (ssize_t)payload_len);
    ASSERT_TRUE(memcmp(out, payload, payload_len) == 0);

    /* The reader must stop cleanly at frame end instead of trying to decode
     * trailing bytes as part of the same compressed frame. */
    ASSERT_TRUE(stream_reader_read(r, out, sizeof(out)) == 0);

    stream_reader_destroy(r);
    sdsfree(input);
    dynamicBufFree(&db);
    return;
}

TEST(compression, streamReaderRejectsTruncatedFrameTrailer) {
    const size_t payload_len = 256;
    uint8_t payload[payload_len];
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *w = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(w != NULL);
    ASSERT_TRUE(stream_writer_write(w, payload, payload_len) >= 0);
    ASSERT_TRUE(stream_writer_finish(w) == 0);
    stream_writer_destroy(w);

    ASSERT_TRUE(sdslen((const char *)db.data) > VKCS_ENVELOPE_SIZE + 1);
    mem_reader_t mr = {};
    mr.data = db.data;
    mr.len = sdslen((const char *)db.data) - 1;
    mr.max_chunk = 7;
    stream_reader_config_t rcfg = makeReaderConfig(STREAM_KIND_RDB, false, 8);
    stream_reader_t *r = stream_reader_create(&rcfg, memReaderRead, &mr);
    ASSERT_TRUE(r != NULL);

    uint8_t out[payload_len];
    ASSERT_TRUE(stream_reader_read(r, out, payload_len) == (ssize_t)payload_len);
    ASSERT_TRUE(memcmp(out, payload, payload_len) == 0);
    ASSERT_TRUE(stream_reader_read(r, out, 1) < 0) << "EOF before frame end should be treated as corruption";
    ASSERT_TRUE(stream_reader_get_error(r) == STREAM_READER_ERROR_CORRUPT)
        << "truncated compressed frame should latch corruption, not I/O";

    stream_reader_destroy(r);
    dynamicBufFree(&db);
    return;
}

/* --- Test: stream_writer_write after finish is rejected.
 * Writes after finish must fail and must not emit bytes. --- */
TEST(compression, streamWriterWriteAfterFinish) {
    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);

    stream_writer_write(t, "hello", 5);
    stream_writer_finish(t);
    size_t len_after_finish = sdslen((const char *)db.data);

    /* Write after finish must fail and emit no output. */
    ASSERT_TRUE(stream_writer_write(t, "world", 5) < 0);
    ASSERT_TRUE(sdslen((const char *)db.data) == len_after_finish) << "write after finish should not produce output";

    /* Second finish — should also be a no-op */
    stream_writer_finish(t);
    ASSERT_TRUE(sdslen((const char *)db.data) == len_after_finish) << "second finish should not produce output";

    /* Verify the stream is still valid: one envelope + one frame */
    ASSERT_TRUE(sdslen((const char *)db.data) > VKCS_ENVELOPE_SIZE);
    stream_decompressor_t sd;
    ASSERT_TRUE(streamDecompressorInit(&sd, ALGO_LZ4) == 0);

    uint8_t decompressed[64];
    size_t total = 0;
    uint8_t *cdata = db.data + VKCS_ENVELOPE_SIZE;
    size_t comp_len = sdslen((const char *)db.data) - VKCS_ENVELOPE_SIZE;
    size_t off = 0;
    while (off < comp_len) {
        size_t consumed = 0;
        ssize_t produced = streamDecompressFeed(
            &sd, decompressed + total, sizeof(decompressed) - total,
            cdata + off, comp_len - off, &consumed);
        ASSERT_TRUE(produced >= 0);
        total += (size_t)produced;
        off += consumed;
        if (consumed == 0 && produced == 0) break;
    }

    ASSERT_TRUE(total == 5 && memcmp(decompressed, "hello", 5) == 0) << "should decompress to 'hello' only";

    streamDecompressorDestroy(&sd);
    stream_writer_destroy(t);
    dynamicBufFree(&db);
    return;
}

/* Test that two independent compress/decompress streams can coexist
 * without interfering with each other. Verifies no shared mutable state. */
TEST(compression, independentStreamsCoexist) {
    /* Create two independent compress streams with different data */
    dynamic_buf_t db1, db2;
    dynamicBufInit(&db1);
    dynamicBufInit(&db2);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t1 = stream_writer_create(&cfg, emitToDynamicBuf, &db1);
    stream_writer_t *t2 = stream_writer_create(&cfg, emitToDynamicBuf, &db2);
    ASSERT_TRUE(t1 != NULL && t2 != NULL);

    const char *data1 = "Stream one data - unique content for first stream AAAA";
    const char *data2 = "Stream two data - different content for second stream BBBB";

    /* Interleave writes to both streams */
    stream_writer_write(t1, data1, strlen(data1));
    stream_writer_write(t2, data2, strlen(data2));
    stream_writer_write(t1, data1, strlen(data1)); /* write again to stream 1 */
    stream_writer_write(t2, data2, strlen(data2)); /* write again to stream 2 */

    stream_writer_finish(t1);
    stream_writer_finish(t2);

    /* Decompress both and verify independently */
    for (int i = 0; i < 2; i++) {
        dynamic_buf_t *db = (i == 0) ? &db1 : &db2;
        const char *expected = (i == 0) ? data1 : data2;
        size_t expected_len = strlen(expected) * 2; /* written twice */

        sds comp = sdsnewlen(db->data, sdslen((const char *)db->data));
        rio buf_rio;
        rioInitWithBuffer(&buf_rio, comp);

        decompress_rio_t dr;
        ASSERT_TRUE(initVkcsRdbDecompressRio(&dr, &buf_rio) == 0);

        char result[256];
        memset(result, 0, sizeof(result));
        ASSERT_TRUE(rioRead((rio *)&dr, result, expected_len) != 0) << "rioRead should succeed for coexisting stream";
        ASSERT_TRUE(memcmp(result, expected, strlen(expected)) == 0) << "first half should match";
        ASSERT_TRUE(memcmp(result + strlen(expected), expected, strlen(expected)) == 0) << "second half should match";

        decompress_rio_destroy(&dr);
        sdsfree(comp);
    }

    stream_writer_destroy(t1);
    stream_writer_destroy(t2);
    dynamicBufFree(&db1);
    dynamicBufFree(&db2);
    return;
}

TEST(compression, streamWriterRepetitivePayloadRoundTrip) {
    /* Default block mode is independent.
     * Verify repetitive data still round-trips correctly. */
    dynamic_buf_t db;
    dynamicBufInit(&db);

    stream_writer_config_t cfg = makeWriterConfig(ALGO_LZ4, 0, STREAM_KIND_RDB);
    stream_writer_t *t = stream_writer_create(&cfg, emitToDynamicBuf, &db);
    ASSERT_TRUE(t != NULL);

    /* Write repetitive data to a single stream */
    char pattern[4096];
    memset(pattern, 'X', sizeof(pattern));
    for (int i = 0; i < 32; i++) {
        ASSERT_TRUE(stream_writer_write(t, pattern, sizeof(pattern)) >= 0);
    }
    ASSERT_TRUE(stream_writer_finish(t) == 0);

    sds comp = sdsnewlen(db.data, sdslen((const char *)db.data));
    rio buf_rio;
    rioInitWithBuffer(&buf_rio, comp);

    decompress_rio_t dr;
    ASSERT_TRUE(initVkcsRdbDecompressRio(&dr, &buf_rio) == 0);

    size_t total_len = sizeof(pattern) * 32;
    char *result = (char *)zmalloc(total_len);
    ASSERT_TRUE(rioRead((rio *)&dr, result, total_len) != 0) << "repetitive payload decompression should succeed";

    /* Verify all bytes match */
    for (size_t i = 0; i < total_len; i++) {
        ASSERT_TRUE(result[i] == 'X');
    }

    decompress_rio_destroy(&dr);
    sdsfree(comp);
    zfree(result);
    stream_writer_destroy(t);
    dynamicBufFree(&db);
    return;
}
