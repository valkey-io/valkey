/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <string.h>
#include <unistd.h>

extern "C" {
#include "compression.h"
#include "compression_stream.h"
#include "server.h"
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
    size_t fail_after_pos;
    bool fail_after_reads;
    int fail_after_success_reads;
    int success_reads;
    int calls;
    int overread_on_call;
} MemReader;

typedef struct {
    int calls;
    int fail_on_call;
} FailingEmitter;

static streamReaderConfig makeReaderConfig(bool allow_passthrough,
                                           size_t buffer_size,
                                           bool skip_codec_checksum_validation) {
    streamReaderConfig cfg = {};
    cfg.allow_passthrough = allow_passthrough;
    cfg.skip_codec_checksum_validation = skip_codec_checksum_validation;
    cfg.buffer_size = buffer_size;
    return cfg;
}

static ssize_t memReaderRead(void *ctx, void *buf, size_t len) {
    MemReader *r = (MemReader *)ctx;
    r->calls++;
    if (r->overread_on_call > 0 && r->calls == r->overread_on_call) return (ssize_t)(len + 1);
    if (r->fail_after_reads && r->success_reads >= r->fail_after_success_reads) return -1;
    if (r->fail_after_pos > 0 && r->pos >= r->fail_after_pos) return -1;
    if (r->pos >= r->len) return 0;

    size_t avail = r->len - r->pos;
    size_t n = len < avail ? len : avail;
    if (r->max_chunk && n > r->max_chunk) n = r->max_chunk;
    if (r->fail_after_pos > 0) {
        size_t until_failure = r->fail_after_pos - r->pos;
        if (n > until_failure) n = until_failure;
    }

    memcpy(buf, r->data + r->pos, n);
    r->pos += n;
    r->success_reads++;
    return (ssize_t)n;
}

static ssize_t noProgressDecompressorFeed(streamDecompressor *decompressor,
                                          uint8_t *output,
                                          size_t output_capacity,
                                          const uint8_t *input,
                                          size_t input_len,
                                          size_t *input_consumed) {
    (void)output;
    (void)output_capacity;
    (void)input;
    (void)input_len;
    decompressor->input_hint = 0;
    *input_consumed = 0;
    return 0;
}

static ssize_t overconsumingDecompressorFeed(streamDecompressor *decompressor,
                                             uint8_t *output,
                                             size_t output_capacity,
                                             const uint8_t *input,
                                             size_t input_len,
                                             size_t *input_consumed) {
    (void)decompressor;
    (void)output;
    (void)output_capacity;
    (void)input;
    *input_consumed = input_len + 1;
    return 0;
}

static ssize_t overproducingDecompressorFeed(streamDecompressor *decompressor,
                                             uint8_t *output,
                                             size_t output_capacity,
                                             const uint8_t *input,
                                             size_t input_len,
                                             size_t *input_consumed) {
    (void)decompressor;
    (void)output;
    (void)input;
    (void)input_len;
    *input_consumed = 0;
    return (ssize_t)(output_capacity + 1);
}

static ssize_t decompressAll(streamDecompressor *decompressor,
                             const uint8_t *input,
                             size_t input_len,
                             uint8_t *output,
                             size_t output_capacity) {
    size_t input_offset = 0;
    size_t output_len = 0;

    while (input_offset < input_len && !decompressor->frame_done) {
        size_t consumed = 0;
        ssize_t produced = streamDecompressorFeed(
            decompressor, output + output_len, output_capacity - output_len,
            input + input_offset, input_len - input_offset, &consumed);
        if (produced < 0 || consumed > input_len - input_offset ||
            (size_t)produced > output_capacity - output_len) {
            return -1;
        }
        input_offset += consumed;
        output_len += (size_t)produced;
        if (consumed == 0 && produced == 0) return -1;
    }
    if (!decompressor->frame_done || input_offset != input_len) return -1;
    return (ssize_t)output_len;
}

/* ===================================================================
 * Streaming compression/decompression tests
 * =================================================================== */

TEST(CompressionTest, streamCompressorOutputBound) {
    const size_t input_sizes[] = {0, 1, 1024, 64 * 1024};
    const compressFlushMode flush_modes[] = {
        COMPRESS_FLUSH_CONTINUE,
        COMPRESS_FLUSH_END,
    };
    const size_t max_input_size = input_sizes[sizeof(input_sizes) / sizeof(input_sizes[0]) - 1];
    uint8_t *input = (uint8_t *)zmalloc(max_input_size);
    for (size_t i = 0; i < max_input_size; i++) input[i] = (uint8_t)(i % 251);

    for (size_t i = 0; i < sizeof(input_sizes) / sizeof(input_sizes[0]); i++) {
        for (size_t j = 0; j < sizeof(flush_modes) / sizeof(flush_modes[0]); j++) {
            streamCompressor compressor;
            ASSERT_EQ(streamCompressorInit(&compressor, ALGO_LZ4, 0, false), C_OK);
            size_t bound = streamCompressorOutputBound(&compressor, input_sizes[i]);
            uint8_t *output = (uint8_t *)zmalloc(bound);

            EXPECT_GE(streamCompressorFeed(&compressor, output, bound,
                                           input, input_sizes[i], flush_modes[j]),
                      0)
                << "input size " << input_sizes[i] << ", flush mode " << flush_modes[j];

            zfree(output);
            streamCompressorFree(&compressor);
        }
    }

    zfree(input);
}

TEST(CompressionTest, streamReaderClassifiesProbeInputs) {
    static const uint8_t plain_input[] = {'H', 'E', 'L', 'L', 'O'};
    static const uint8_t strict_non_vcs[VCS_ENVELOPE_SIZE] = {'R', 'E', 'D', 'I', 'S', '0', '0'};

    struct {
        const char *name;
        const uint8_t *input;
        size_t input_len;
        size_t max_chunk;
        bool allow_passthrough;
        bool expect_probe_ok;
        streamReaderErrorKind expected_error;
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
        {"non-VCS with passthrough disabled",
         strict_non_vcs,
         sizeof(strict_non_vcs),
         0,
         false,
         false,
         STREAM_READER_ERROR_INCOMPATIBLE,
         0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        MemReader mr = {};
        mr.data = cases[i].input;
        mr.len = cases[i].input_len;
        mr.max_chunk = cases[i].max_chunk;
        streamReaderConfig cfg = makeReaderConfig(cases[i].allow_passthrough,
                                                  STREAM_READER_BUFFER_SIZE_DEFAULT,
                                                  false);
        streamReader t;
        compressionAlgo algo = ALGO_NONE;
        if (cases[i].expect_probe_ok) {
            ASSERT_EQ(streamReaderInit(&t, &cfg, memReaderRead, &mr, &algo), C_OK) << cases[i].name;
            ASSERT_EQ(algo, ALGO_NONE) << cases[i].name;

            uint8_t out[16] = {0};
            ASSERT_EQ(streamReaderRead(&t, out, cases[i].expected_read_len), (ssize_t)cases[i].expected_read_len)
                << cases[i].name;
            EXPECT_EQ(memcmp(out, cases[i].input, cases[i].expected_read_len), 0) << cases[i].name;
            ASSERT_EQ(streamReaderRead(&t, out, sizeof(out)), 0) << cases[i].name;
            ASSERT_EQ(streamReaderFinish(&t), C_OK) << cases[i].name;
            ASSERT_EQ(t.state, STREAM_READER_STATE_FINISHED) << cases[i].name;
        } else {
            ASSERT_EQ(streamReaderInit(&t, &cfg, memReaderRead, &mr, &algo), C_ERR) << cases[i].name;
            ASSERT_EQ(t.error_kind, cases[i].expected_error) << cases[i].name;

            uint8_t out[8] = {0};
            ASSERT_EQ(streamReaderRead(&t, out, sizeof(out)), -1) << cases[i].name;
        }

        streamReaderFree(&t);
    }
}

TEST(CompressionTest, streamReaderRejectsEveryTruncatedVcsEnvelope) {
    const uint8_t envelope[VCS_ENVELOPE_SIZE] = {
        VCS_MAGIC_0,
        VCS_MAGIC_1,
        VCS_MAGIC_2,
        VCS_VERSION,
        VCS_CODEC_LZ4,
        0,
        VCS_STREAM_RDB,
    };

    for (size_t prefix_len = 1; prefix_len < VCS_ENVELOPE_SIZE; prefix_len++) {
        MemReader mr = {};
        mr.data = envelope;
        mr.len = prefix_len;
        mr.max_chunk = 1;
        streamReaderConfig cfg = makeReaderConfig(true, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
        streamReader reader;
        compressionAlgo algo = ALGO_NONE;
        ASSERT_EQ(streamReaderInit(&reader, &cfg, memReaderRead, &mr, &algo), C_ERR)
            << "accepted VCS prefix length " << prefix_len;
        ASSERT_EQ(reader.error_kind, STREAM_READER_ERROR_INCOMPATIBLE)
            << "VCS prefix length " << prefix_len;
        streamReaderFree(&reader);
    }

    MemReader empty = {};
    streamReaderConfig cfg = makeReaderConfig(true, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
    streamReader reader;
    compressionAlgo algo = ALGO_LZ4;
    ASSERT_EQ(streamReaderInit(&reader, &cfg, memReaderRead, &empty, &algo), C_OK);
    ASSERT_EQ(algo, ALGO_NONE);
    uint8_t out = 0;
    ASSERT_EQ(streamReaderRead(&reader, &out, 1), 0);
    streamReaderFree(&reader);
}

TEST(CompressionTest, streamReaderClampsSmallBuffer) {
    const uint8_t input[] = {'R', 'D', 'B'};
    MemReader source = {};
    source.data = input;
    source.len = sizeof(input);
    streamReaderConfig cfg = makeReaderConfig(true, 1, false);
    streamReader reader;
    ASSERT_EQ(streamReaderInit(&reader, &cfg, memReaderRead, &source, NULL), C_OK);
    ASSERT_EQ(reader.buffer_size, (size_t)STREAM_READER_BUFFER_SIZE_MIN);
    ASSERT_EQ(streamReaderFinish(&reader), C_OK);
    streamReaderFree(&reader);
}

TEST(CompressionTest, streamReaderClassifiesCodecContractViolationsAsInternalErrors) {
    typedef ssize_t (*feedFn)(streamDecompressor *, uint8_t *, size_t, const uint8_t *, size_t, size_t *);
    struct {
        const char *name;
        feedFn feed;
    } cases[] = {
        {"consumed input exceeds supplied input", overconsumingDecompressorFeed},
        {"produced output exceeds supplied capacity", overproducingDecompressorFeed},
    };
    const uint8_t input[] = {
        VCS_MAGIC_0,
        VCS_MAGIC_1,
        VCS_MAGIC_2,
        VCS_VERSION,
        VCS_CODEC_LZ4,
        0,
        VCS_STREAM_RDB,
        0,
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        MockValkey mock;
        EXPECT_CALL(mock, streamDecompressorFeed(_, _, _, _, _, _))
            .WillOnce(Invoke(cases[i].feed));

        MemReader source = {};
        source.data = input;
        source.len = sizeof(input);
        streamReaderConfig cfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_MIN, false);
        streamReader reader;
        ASSERT_EQ(streamReaderInit(&reader, &cfg, memReaderRead, &source, NULL), C_OK) << cases[i].name;

        uint8_t output;
        EXPECT_EQ(streamReaderRead(&reader, &output, 1), -1) << cases[i].name;
        EXPECT_EQ(reader.error_kind, STREAM_READER_ERROR_INTERNAL) << cases[i].name;
        streamReaderFree(&reader);
    }
}

TEST(CompressionTest, streamReaderRejectsFullInputBufferWithoutCodecProgress) {
    MockValkey mock;
    EXPECT_CALL(mock, streamDecompressorFeed(_, _, _, _, _, _))
        .WillRepeatedly(Invoke(noProgressDecompressorFeed));

    size_t input_len = VCS_ENVELOPE_SIZE + STREAM_READER_COMPRESSED_BUFFER_SIZE + 1;
    uint8_t *input = (uint8_t *)zmalloc(input_len);
    memset(input, 0, input_len);
    const uint8_t envelope[VCS_ENVELOPE_SIZE] = {
        VCS_MAGIC_0,
        VCS_MAGIC_1,
        VCS_MAGIC_2,
        VCS_VERSION,
        VCS_CODEC_LZ4,
        0,
        VCS_STREAM_RDB,
    };
    memcpy(input, envelope, sizeof(envelope));

    MemReader source = {};
    source.data = input;
    source.len = input_len;
    streamReaderConfig cfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
    streamReader reader;
    ASSERT_EQ(streamReaderInit(&reader, &cfg, memReaderRead, &source, NULL), C_OK);

    uint8_t output;
    EXPECT_EQ(streamReaderRead(&reader, &output, 1), -1);
    EXPECT_EQ(reader.error_kind, STREAM_READER_ERROR_CORRUPT);
    EXPECT_EQ(reader.compressed_buf_len, reader.compressed_buf_size);
    EXPECT_LT(reader.compressed_buf_size, reader.buffer_size);
    EXPECT_LT(source.pos, source.len);

    streamReaderFree(&reader);
    zfree(input);
}

/* ===================================================================
 * Tests for stream writer API and rio decorators
 * =================================================================== */

typedef struct {
    uint8_t *data;
} DynamicBuf;

static void dynamicBufInit(DynamicBuf *db) {
    db->data = (uint8_t *)sdsempty();
}

static void dynamicBufFree(DynamicBuf *db) {
    if (db->data) sdsfree((sds)db->data);
    db->data = NULL;
}

static int emitToDynamicBuf(void *ctx, const uint8_t *data, size_t len) {
    DynamicBuf *db = (DynamicBuf *)ctx;
    db->data = (uint8_t *)sdscatlen((sds)db->data, data, len);
    return db->data != NULL ? C_OK : C_ERR;
}

static int failSelectedEmit(void *ctx, const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    FailingEmitter *emitter = (FailingEmitter *)ctx;
    emitter->calls++;
    return emitter->calls == emitter->fail_on_call ? C_ERR : C_OK;
}

static ssize_t testRioBufferReadSome(rio *r, void *buf, size_t len) {
    size_t buflen = sdslen(r->io.buffer.ptr);
    if (r->io.buffer.pos < 0 || (size_t)r->io.buffer.pos >= buflen) return 0;

    size_t available = buflen - (size_t)r->io.buffer.pos;
    size_t read_len = available < len ? available : len;
    memcpy(buf, r->io.buffer.ptr + r->io.buffer.pos, read_len);
    r->io.buffer.pos += read_len;
    return (ssize_t)read_len;
}

static void enableTestRioBufferPartialReads(rio *r) {
    r->read_some = testRioBufferReadSome;
}

static int initVcsRdbStreamReader(streamReader *reader, rio *r) {
    enableTestRioBufferPartialReads(r);
    return rdbInitStreamReader(r, reader, false, NULL) == RDB_STREAM_READER_INIT_OK ? C_OK : C_ERR;
}

static size_t rio_update_calls = 0;

static void countRioUpdateCalls(rio *r, const void *buf, size_t len) {
    (void)r;
    (void)buf;
    (void)len;
    rio_update_calls++;
}

static int emitToRioBackend(void *ctx, const uint8_t *data, size_t len) {
    return rioWriteRaw((rio *)ctx, data, len) ? C_OK : C_ERR;
}

static int attachCompressionWriter(rio *r, streamWriter *writer) {
    if (streamWriterInit(writer, ALGO_LZ4, true, emitToRioBackend, r) == C_ERR) return C_ERR;
    rioAttachStreamWriter(r, writer);
    return C_OK;
}

static int finishCompressionWriter(rio *r, streamWriter *writer) {
    if (streamWriterFinish(writer) == C_ERR) {
        r->flags |= RIO_FLAG_WRITE_ERROR;
        return C_ERR;
    }
    if (rioFlushRaw(r)) return C_OK;
    r->flags |= RIO_FLAG_WRITE_ERROR;
    return C_ERR;
}

static void freeCompressionWriter(rio *r, streamWriter *writer) {
    rioDetachStreamWriter(r);
    streamWriterFree(writer);
}

typedef enum {
    TEST_COMPRESSION_LAYER_CODEC = 0,
    TEST_COMPRESSION_LAYER_STREAM,
    TEST_COMPRESSION_LAYER_RIO,
} testCompressionLayer;

typedef struct {
    const char *name;
    testCompressionLayer writer_layer;
    testCompressionLayer reader_layer;
    size_t payload_len;
    size_t reader_chunk;
    size_t source_chunk;
} compressionRoundTripCase;

static void fillRoundTripPayload(uint8_t *payload, size_t len) {
    uint32_t state = 0x6d2b79f5;
    for (size_t i = 0; i < len; i++) {
        state = state * 1664525u + 1013904223u;
        payload[i] = (uint8_t)(state >> 24);
    }
}

static int encodeRoundTripPayload(testCompressionLayer layer,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  sds *encoded) {
    *encoded = NULL;

    if (layer == TEST_COMPRESSION_LAYER_CODEC) {
        streamCompressor compressor;
        if (streamCompressorInit(&compressor, ALGO_LZ4, 0, true) == C_ERR) return C_ERR;

        size_t bound = streamCompressorOutputBound(&compressor, payload_len);
        sds output = sdsMakeRoomFor(sdsempty(), bound);
        ssize_t output_len = streamCompressorFeed(&compressor, (uint8_t *)output, bound,
                                                  payload, payload_len, COMPRESS_FLUSH_END);
        streamCompressorFree(&compressor);
        if (output_len <= 0) {
            sdsfree(output);
            return C_ERR;
        }
        sdsIncrLen(output, output_len);
        *encoded = output;
        return C_OK;
    }

    if (layer == TEST_COMPRESSION_LAYER_STREAM) {
        DynamicBuf output;
        dynamicBufInit(&output);
        streamWriter writer;
        if (streamWriterInit(&writer, ALGO_LZ4, true, emitToDynamicBuf, &output) == C_ERR) {
            dynamicBufFree(&output);
            return C_ERR;
        }
        int result = streamWriterWrite(&writer, payload, payload_len);
        if (result == C_OK && writer.state != STREAM_WRITER_STATE_ACTIVE) result = C_ERR;
        if (result == C_OK) result = streamWriterFinish(&writer);
        if (result == C_OK && writer.state != STREAM_WRITER_STATE_FINISHED) result = C_ERR;
        streamWriterFree(&writer);
        if (result == C_ERR) {
            dynamicBufFree(&output);
            return C_ERR;
        }
        *encoded = (sds)output.data;
        return C_OK;
    }

    sds output = sdsempty();
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, output);
    streamWriter writer;
    if (attachCompressionWriter(&buffer_rio, &writer) == C_ERR) {
        streamWriterFree(&writer);
        sdsfree(buffer_rio.io.buffer.ptr);
        return C_ERR;
    }

    int result = rioWrite(&buffer_rio, payload, payload_len) ? C_OK : C_ERR;
    if (result == C_OK) result = finishCompressionWriter(&buffer_rio, &writer);
    if (result == C_OK && buffer_rio.processed_bytes != payload_len) result = C_ERR;
    if (result == C_OK && buffer_rio.stream_processed_bytes != sdslen(buffer_rio.io.buffer.ptr)) result = C_ERR;
    freeCompressionWriter(&buffer_rio, &writer);
    if (result == C_ERR) {
        sdsfree(buffer_rio.io.buffer.ptr);
        return C_ERR;
    }
    *encoded = buffer_rio.io.buffer.ptr;
    return C_OK;
}

static int decodeRoundTripPayload(testCompressionLayer layer,
                                  const sds encoded,
                                  bool has_envelope,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  size_t reader_chunk,
                                  size_t source_chunk) {
    uint8_t *output = (uint8_t *)zmalloc(payload_len);
    int result = C_ERR;

    if (layer == TEST_COMPRESSION_LAYER_CODEC) {
        size_t offset = has_envelope ? VCS_ENVELOPE_SIZE : 0;
        streamDecompressor decompressor;
        if (streamDecompressorInit(&decompressor, ALGO_LZ4, false) == C_ERR) {
            zfree(output);
            return C_ERR;
        }
        ssize_t output_len = decompressAll(&decompressor,
                                           (const uint8_t *)encoded + offset,
                                           sdslen(encoded) - offset,
                                           output,
                                           payload_len);
        streamDecompressorFree(&decompressor);
        if (output_len == (ssize_t)payload_len && memcmp(output, payload, payload_len) == 0) result = C_OK;
        zfree(output);
        return result;
    }

    if (layer == TEST_COMPRESSION_LAYER_STREAM) {
        MemReader source = {};
        source.data = (const uint8_t *)encoded;
        source.len = sdslen(encoded);
        source.max_chunk = source_chunk;
        streamReaderConfig cfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_MIN, false);
        streamReader reader;
        if (streamReaderInit(&reader, &cfg, memReaderRead, &source, NULL) == C_ERR) {
            zfree(output);
            return C_ERR;
        }

        size_t total = 0;
        while (total < payload_len) {
            size_t chunk = reader_chunk == 0 ? payload_len - total : reader_chunk;
            if (chunk > payload_len - total) chunk = payload_len - total;
            ssize_t nread = streamReaderRead(&reader, output + total, chunk);
            if (nread <= 0) break;
            total += (size_t)nread;
        }
        if (total == payload_len &&
            memcmp(output, payload, payload_len) == 0 &&
            streamReaderRead(&reader, output, 1) == 0 &&
            streamReaderFinish(&reader) == C_OK) {
            result = C_OK;
        }
        streamReaderFree(&reader);
        zfree(output);
        return result;
    }

    sds input = sdsdup(encoded);
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, input);
    streamReader reader;
    if (initVcsRdbStreamReader(&reader, &buffer_rio) == C_ERR) {
        sdsfree(input);
        zfree(output);
        return C_ERR;
    }

    size_t total = 0;
    while (total < payload_len) {
        size_t chunk = reader_chunk == 0 ? payload_len - total : reader_chunk;
        if (chunk > payload_len - total) chunk = payload_len - total;
        if (!rioRead(&buffer_rio, output + total, chunk)) break;
        total += chunk;
    }
    if (total == payload_len &&
        memcmp(output, payload, payload_len) == 0 &&
        streamReaderFinish(&reader) == C_OK) {
        result = C_OK;
    }
    rdbFreeStreamReader(&buffer_rio, &reader);
    sdsfree(input);
    zfree(output);
    return result;
}

TEST(CompressionTest, compressionLayersRoundTrip) {
    const compressionRoundTripCase cases[] = {
        {"codec", TEST_COMPRESSION_LAYER_CODEC, TEST_COMPRESSION_LAYER_CODEC, 64, 0, 0},
        {"stream writer", TEST_COMPRESSION_LAYER_STREAM, TEST_COMPRESSION_LAYER_CODEC, 256, 0, 0},
        {"large stream write", TEST_COMPRESSION_LAYER_STREAM, TEST_COMPRESSION_LAYER_STREAM,
         (1024 * 1024) + 4096, 0, 0},
        {"small stream reads", TEST_COMPRESSION_LAYER_STREAM, TEST_COMPRESSION_LAYER_STREAM,
         256 * 1024, 17, 4096},
        {"rio writer", TEST_COMPRESSION_LAYER_RIO, TEST_COMPRESSION_LAYER_CODEC, 256, 0, 0},
        {"rio reader", TEST_COMPRESSION_LAYER_STREAM, TEST_COMPRESSION_LAYER_RIO, 256, 0, 0},
        {"large rio reader", TEST_COMPRESSION_LAYER_STREAM, TEST_COMPRESSION_LAYER_RIO,
         256 * 1024, 4096, 0},
    };
    const uint8_t expected_envelope[VCS_ENVELOPE_SIZE] = {
        VCS_MAGIC_0,
        VCS_MAGIC_1,
        VCS_MAGIC_2,
        VCS_VERSION,
        VCS_CODEC_LZ4,
        0,
        VCS_STREAM_RDB,
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t *payload = (uint8_t *)zmalloc(cases[i].payload_len);
        fillRoundTripPayload(payload, cases[i].payload_len);
        sds encoded = NULL;
        ASSERT_EQ(encodeRoundTripPayload(cases[i].writer_layer, payload,
                                         cases[i].payload_len, &encoded),
                  C_OK)
            << cases[i].name;

        bool has_envelope = cases[i].writer_layer != TEST_COMPRESSION_LAYER_CODEC;
        if (has_envelope) {
            ASSERT_GE(sdslen(encoded), (size_t)VCS_ENVELOPE_SIZE) << cases[i].name;
            EXPECT_EQ(memcmp(encoded, expected_envelope, sizeof(expected_envelope)), 0) << cases[i].name;
        }
        EXPECT_EQ(decodeRoundTripPayload(cases[i].reader_layer, encoded, has_envelope,
                                         payload, cases[i].payload_len,
                                         cases[i].reader_chunk, cases[i].source_chunk),
                  C_OK)
            << cases[i].name;

        sdsfree(encoded);
        zfree(payload);
    }
}

TEST(CompressionTest, streamReaderClassifiesSourceCallbackFailuresAsIoErrors) {
    MemReader failed_source = {};
    failed_source.fail_after_reads = true;
    failed_source.fail_after_success_reads = 0;
    streamReaderConfig failed_cfg = makeReaderConfig(true, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
    streamReader failed_reader;
    ASSERT_EQ(streamReaderInit(&failed_reader, &failed_cfg, memReaderRead, &failed_source, NULL), C_ERR);
    ASSERT_EQ(failed_reader.error_kind, STREAM_READER_ERROR_IO);
    streamReaderFree(&failed_reader);

    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter writer;
    ASSERT_EQ(streamWriterInit(&writer, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);
    ASSERT_EQ(streamWriterWrite(&writer, "source callback contract", 24), C_OK);
    ASSERT_EQ(streamWriterFinish(&writer), C_OK);
    streamWriterFree(&writer);

    const int overread_calls[] = {1, 2, 3};
    for (size_t i = 0; i < sizeof(overread_calls) / sizeof(overread_calls[0]); i++) {
        int overread_on_call = overread_calls[i];
        MemReader source = {};
        source.data = db.data;
        source.len = sdslen((const char *)db.data);
        source.overread_on_call = overread_on_call;
        streamReaderConfig rcfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
        streamReader reader;
        int init_result = streamReaderInit(&reader, &rcfg, memReaderRead, &source, NULL);
        if (overread_on_call <= 2) {
            ASSERT_EQ(init_result, C_ERR) << "callback call " << overread_on_call;
            ASSERT_EQ(reader.error_kind, STREAM_READER_ERROR_IO)
                << "callback call " << overread_on_call;
            streamReaderFree(&reader);
            continue;
        }
        ASSERT_EQ(init_result, C_OK);

        uint8_t out[24];
        ASSERT_EQ(streamReaderRead(&reader, out, sizeof(out)), -1) << "callback call " << overread_on_call;
        ASSERT_EQ(reader.error_kind, STREAM_READER_ERROR_IO) << "callback call " << overread_on_call;
        ASSERT_EQ(streamReaderRead(&reader, out, sizeof(out)), -1) << "I/O error must remain sticky";
        streamReaderFree(&reader);
    }

    dynamicBufFree(&db);
}

TEST(CompressionTest, streamReaderRejectsInvalidEnvelopeFields) {
    const uint8_t good[VCS_ENVELOPE_SIZE] = {
        VCS_MAGIC_0,
        VCS_MAGIC_1,
        VCS_MAGIC_2,
        VCS_VERSION,
        VCS_CODEC_LZ4,
        0,
        VCS_STREAM_RDB,
    };
    struct {
        const char *name;
        size_t offset;
        uint8_t value;
    } cases[] = {
        {"magic", 0, 'X'},
        {"version", VCS_OFFSET_VERSION, VCS_VERSION + 1},
        {"unknown codec", VCS_OFFSET_CODEC, 0x7f},
        {"reserved byte", VCS_OFFSET_RESERVED, 1},
        {"stream kind", VCS_OFFSET_STREAM_KIND, 0x7f},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t mutated[VCS_ENVELOPE_SIZE];
        memcpy(mutated, good, sizeof(mutated));
        mutated[cases[i].offset] = cases[i].value;

        MemReader source = {};
        source.data = mutated;
        source.len = sizeof(mutated);
        streamReaderConfig cfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
        streamReader reader;
        ASSERT_EQ(streamReaderInit(&reader, &cfg, memReaderRead, &source, NULL), C_ERR) << cases[i].name;
        ASSERT_EQ(reader.error_kind, STREAM_READER_ERROR_INCOMPATIBLE) << cases[i].name;
        streamReaderFree(&reader);
    }
}

/* Regression for partial output followed by a source read error. The partial
 * bytes are returned, but the error must remain sticky for the next read. */
TEST(CompressionTest, streamReaderPartialThenErrorSetsErrored) {
    const size_t payload_len = 256 * 1024;
    uint8_t *payload = (uint8_t *)zmalloc(payload_len);
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < payload_len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        payload[i] = (uint8_t)(x & 0xFF);
    }

    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter w;
    ASSERT_EQ(streamWriterInit(&w, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);
    ASSERT_EQ(streamWriterWrite(&w, payload, payload_len), C_OK);
    ASSERT_EQ(streamWriterFinish(&w), C_OK);
    streamWriterFree(&w);

    MemReader fr = {};
    fr.data = db.data;
    fr.len = sdslen((const char *)db.data);
    fr.max_chunk = 4096;
    fr.fail_after_pos = VCS_ENVELOPE_SIZE + STREAM_READER_BUFFER_SIZE_MIN;
    streamReaderConfig rcfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_MIN, false);
    streamReader r;
    ASSERT_EQ(streamReaderInit(&r, &rcfg, memReaderRead, &fr, NULL), C_OK);

    const size_t out_len = payload_len;
    uint8_t *out = (uint8_t *)zmalloc(out_len);
    ssize_t n1 = streamReaderRead(&r, out, out_len);
    ASSERT_GT(n1, 0) << "first read should return partial output";
    ASSERT_LT(n1, (ssize_t)out_len) << "injected read error should stop the first read early";
    EXPECT_EQ(memcmp(out, payload, (size_t)n1), 0);
    ASSERT_EQ(r.error_kind, STREAM_READER_ERROR_IO) << "error must be latched before returning partial output";
    ASSERT_EQ(streamReaderRead(&r, out, out_len), -1) << "second read should fail immediately";
    ASSERT_EQ(r.error_kind, STREAM_READER_ERROR_IO);

    streamReaderFree(&r);

    /* Passthrough mode should also preserve partial bytes when source read
     * fails after probe/prefix buffering, then latch sticky error state. */
    const uint8_t plain[] = "NOTVCS-passthrough-regression";
    MemReader fr_passthrough = {};
    fr_passthrough.data = plain;
    fr_passthrough.len = sizeof(plain) - 1;
    fr_passthrough.max_chunk = 0;
    fr_passthrough.fail_after_reads = true;
    fr_passthrough.fail_after_success_reads = 1; /* probe succeeds, next read fails */
    streamReaderConfig pass_cfg = makeReaderConfig(true, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
    streamReader rp;
    ASSERT_EQ(streamReaderInit(&rp, &pass_cfg, memReaderRead, &fr_passthrough, NULL), C_OK);

    uint8_t pass_out[64];
    ssize_t p1 = streamReaderRead(&rp, pass_out, sizeof(pass_out));
    ASSERT_GT(p1, 0) << "passthrough first read should return partial output";
    EXPECT_EQ(memcmp(pass_out, plain, (size_t)p1), 0) << "passthrough partial bytes should match input prefix";
    ASSERT_EQ(streamReaderRead(&rp, pass_out, sizeof(pass_out)), -1)
        << "passthrough second read should fail immediately";
    ASSERT_EQ(rp.error_kind, STREAM_READER_ERROR_IO);
    streamReaderFree(&rp);
    zfree(out);

    dynamicBufFree(&db);
    zfree(payload);
}

TEST(CompressionTest, streamWriterRejectsUnsupportedAlgorithms) {
    const compressionAlgo algorithms[] = {ALGO_NONE, ALGO_LZF};
    DynamicBuf db;
    dynamicBufInit(&db);

    for (size_t i = 0; i < sizeof(algorithms) / sizeof(algorithms[0]); i++) {
        streamWriter writer;
        EXPECT_EQ(streamWriterInit(&writer, algorithms[i], true, emitToDynamicBuf, &db), C_ERR)
            << compressionAlgoName(algorithms[i]);
        streamWriterFree(&writer);
    }

    dynamicBufFree(&db);
}

TEST(CompressionTest, streamWriterFinishProducesAValidEmptyStream) {
    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter writer;
    ASSERT_EQ(streamWriterInit(&writer, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);

    ASSERT_EQ(streamWriterWrite(&writer, NULL, 0), C_OK);
    ASSERT_EQ(sdslen((const char *)db.data), 0u) << "empty writes must stay lazy";
    ASSERT_EQ(streamWriterFinish(&writer), C_OK);
    ASSERT_GT(sdslen((const char *)db.data), (size_t)VCS_ENVELOPE_SIZE);
    streamWriterFree(&writer);

    MemReader source = {};
    source.data = db.data;
    source.len = sdslen((const char *)db.data);
    source.max_chunk = 1;
    streamReaderConfig rcfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
    streamReader reader;
    ASSERT_EQ(streamReaderInit(&reader, &rcfg, memReaderRead, &source, NULL), C_OK);
    uint8_t out = 0;
    ASSERT_EQ(streamReaderRead(&reader, &out, 1), 0);
    ASSERT_EQ(streamReaderFinish(&reader), C_OK);
    streamReaderFree(&reader);
    dynamicBufFree(&db);
}

TEST(CompressionTest, streamWriterSinkFailuresAreSticky) {
    const int failing_calls[] = {1, 2};
    for (size_t i = 0; i < sizeof(failing_calls) / sizeof(failing_calls[0]); i++) {
        int fail_on_call = failing_calls[i];
        FailingEmitter emitter = {0, fail_on_call};
        streamWriter writer;
        ASSERT_EQ(streamWriterInit(&writer, ALGO_LZ4, true, failSelectedEmit, &emitter), C_OK);

        ASSERT_EQ(streamWriterWrite(&writer, "payload", 7), C_ERR) << "sink call " << fail_on_call;
        ASSERT_EQ(writer.state, STREAM_WRITER_STATE_ERROR);
        ASSERT_EQ(streamWriterWrite(&writer, "retry", 5), C_ERR);
        ASSERT_EQ(streamWriterFinish(&writer), C_ERR);
        ASSERT_EQ(emitter.calls, fail_on_call) << "an errored writer must not emit more bytes";
        streamWriterFree(&writer);
    }

    FailingEmitter emitter = {0, 0};
    streamWriter writer;
    ASSERT_EQ(streamWriterInit(&writer, ALGO_LZ4, true, failSelectedEmit, &emitter), C_OK);
    ASSERT_EQ(streamWriterWrite(&writer, "payload", 7), C_OK);
    emitter.fail_on_call = emitter.calls + 1;
    ASSERT_EQ(streamWriterFinish(&writer), C_ERR);
    int calls_after_failure = emitter.calls;
    ASSERT_EQ(streamWriterFinish(&writer), C_ERR);
    ASSERT_EQ(streamWriterWrite(&writer, "retry", 5), C_ERR);
    ASSERT_EQ(emitter.calls, calls_after_failure) << "a failed finish must remain failed";
    streamWriterFree(&writer);
}

TEST(CompressionTest, checksumBypassSkipsOnlyCodecVerification) {
    const char *payload = "checksum bypass payload";
    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter writer;
    ASSERT_EQ(streamWriterInit(&writer, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);
    ASSERT_EQ(streamWriterWrite(&writer, payload, strlen(payload)), C_OK);
    ASSERT_EQ(streamWriterFinish(&writer), C_OK);
    streamWriterFree(&writer);

    ASSERT_GT(sdslen((const char *)db.data), (size_t)VCS_ENVELOPE_SIZE + 4);
    db.data[sdslen((const char *)db.data) - 1] ^= 1;

    const bool skip_checksum_cases[] = {false, true};
    for (size_t i = 0; i < sizeof(skip_checksum_cases) / sizeof(skip_checksum_cases[0]); i++) {
        bool skip_codec_checksum_validation = skip_checksum_cases[i];
        MemReader source = {};
        source.data = db.data;
        source.len = sdslen((const char *)db.data);
        streamReaderConfig rcfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_DEFAULT,
                                                   skip_codec_checksum_validation);
        streamReader reader;
        ASSERT_EQ(streamReaderInit(&reader, &rcfg, memReaderRead, &source, NULL), C_OK);

        char out[64] = {0};
        ASSERT_EQ(streamReaderRead(&reader, out, strlen(payload)), (ssize_t)strlen(payload));
        EXPECT_EQ(memcmp(out, payload, strlen(payload)), 0);
        ASSERT_EQ(streamReaderFinish(&reader), skip_codec_checksum_validation ? C_OK : C_ERR);
        streamReaderFree(&reader);
    }

    MemReader truncated = {};
    truncated.data = db.data;
    truncated.len = sdslen((const char *)db.data) - 5;
    streamReaderConfig bypass = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_DEFAULT, true);
    streamReader reader;
    ASSERT_EQ(streamReaderInit(&reader, &bypass, memReaderRead, &truncated, NULL), C_OK);
    char out[64] = {0};
    ASSERT_EQ(streamReaderRead(&reader, out, strlen(payload)), (ssize_t)strlen(payload));
    ASSERT_EQ(streamReaderFinish(&reader), C_ERR)
        << "checksum bypass must not bypass exact frame-end validation";
    streamReaderFree(&reader);

    dynamicBufFree(&db);
}

TEST(CompressionTest, streamReaderFinishAcceptsClosedFrame) {
    const char *payload = "validate frame end payload";
    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter w;
    ASSERT_EQ(streamWriterInit(&w, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);
    ASSERT_EQ(streamWriterWrite(&w, payload, strlen(payload)), C_OK);
    ASSERT_EQ(streamWriterFinish(&w), C_OK);

    MemReader reader_ctx = {};
    reader_ctx.data = db.data;
    reader_ctx.len = sdslen((const char *)db.data);
    reader_ctx.max_chunk = 7;
    streamReaderConfig rcfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
    streamReader reader;
    ASSERT_EQ(streamReaderInit(&reader, &rcfg, memReaderRead, &reader_ctx, NULL), C_OK);
    ASSERT_EQ(reader.state, STREAM_READER_STATE_COMPRESSED);

    char out[64];
    ASSERT_EQ(streamReaderRead(&reader, out, strlen(payload)), (ssize_t)strlen(payload));
    ASSERT_EQ(reader.state, STREAM_READER_STATE_COMPRESSED);
    EXPECT_EQ(memcmp(out, payload, strlen(payload)), 0);
    ASSERT_EQ(streamReaderFinish(&reader), C_OK);
    ASSERT_EQ(reader.state, STREAM_READER_STATE_FINISHED);
    ASSERT_EQ(streamReaderFinish(&reader), C_OK);
    ASSERT_EQ(streamReaderRead(&reader, out, sizeof(out)), 0);

    streamReaderFree(&reader);
    streamWriterFree(&w);
    dynamicBufFree(&db);
}

TEST(CompressionTest, streamReaderFinishRejectsUnreadDecodedBytes) {
    const char *payload = "payload with unread decoded suffix";
    const size_t payload_len = strlen(payload);
    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter w;
    ASSERT_EQ(streamWriterInit(&w, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);
    ASSERT_EQ(streamWriterWrite(&w, payload, payload_len), C_OK);
    ASSERT_EQ(streamWriterFinish(&w), C_OK);

    MemReader reader_ctx = {};
    reader_ctx.data = db.data;
    reader_ctx.len = sdslen((const char *)db.data);
    streamReaderConfig rcfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_DEFAULT, false);
    streamReader reader;
    ASSERT_EQ(streamReaderInit(&reader, &rcfg, memReaderRead, &reader_ctx, NULL), C_OK);

    char out[8];
    ASSERT_EQ(streamReaderRead(&reader, out, sizeof(out)), (ssize_t)sizeof(out));
    EXPECT_EQ(memcmp(out, payload, sizeof(out)), 0);
    ASSERT_EQ(streamReaderFinish(&reader), C_ERR);
    ASSERT_EQ(reader.error_kind, STREAM_READER_ERROR_CORRUPT);

    streamReaderFree(&reader);
    streamWriterFree(&w);
    dynamicBufFree(&db);
}

TEST(CompressionTest, rioCompressionWriterDoesNotOwnRdbChecksumPolicy) {
    const bool checksum_cases[] = {false, true};
    for (size_t i = 0; i < sizeof(checksum_cases) / sizeof(checksum_cases[0]); i++) {
        bool inner_skips_checksum = checksum_cases[i];
        sds buf = sdsempty();
        rio inner;
        rioInitWithBuffer(&inner, buf);
        if (inner_skips_checksum) inner.flags |= RIO_FLAG_SKIP_RDB_CHECKSUM;

        streamWriter writer;
        ASSERT_EQ(attachCompressionWriter(&inner, &writer), 0);
        ASSERT_TRUE(inner.update_cksum == NULL);
        ASSERT_EQ((inner.flags & RIO_FLAG_SKIP_RDB_CHECKSUM) != 0, inner_skips_checksum);
        ASSERT_NE(rioWrite(&inner, "checksum policy", 15), 0u);
        ASSERT_EQ(inner.cksum, 0u);
        ASSERT_EQ(finishCompressionWriter(&inner, &writer), 0);

        freeCompressionWriter(&inner, &writer);
        sdsfree(inner.io.buffer.ptr);
    }
}

TEST(CompressionTest, rioStreamReaderTellTracksSourceProgress) {
    DynamicBuf db;
    dynamicBufInit(&db);

    char payload[4096];
    memset(payload, 'A', sizeof(payload));
    streamWriter t;
    ASSERT_EQ(streamWriterInit(&t, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);
    ASSERT_EQ(streamWriterWrite(&t, payload, sizeof(payload)), C_OK);
    ASSERT_EQ(streamWriterFinish(&t), C_OK);
    streamWriterFree(&t);

    sds comp_sds = sdsnewlen(db.data, sdslen((const char *)db.data));
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, comp_sds);

    streamReader reader;
    ASSERT_EQ(initVcsRdbStreamReader(&reader, &buffer_rio), 0);

    char out[2048];
    ASSERT_NE(rioRead(&buffer_rio, out, sizeof(out)), 0u);
    ASSERT_EQ((size_t)rioTell(&buffer_rio), buffer_rio.stream_processed_bytes);
    ASSERT_LT((size_t)rioTell(&buffer_rio), sizeof(out))
        << "rio tell should track source bytes, not logical output bytes";

    rdbFreeStreamReader(&buffer_rio, &reader);
    sdsfree(comp_sds);
    dynamicBufFree(&db);
}

TEST(CompressionTest, rioStreamReaderHonorsMaxProcessingChunk) {
    const size_t payload_len = 1024;
    const size_t chunk_size = 128;
    uint8_t payload[payload_len];
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i % 251);
    }

    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter writer;
    ASSERT_EQ(streamWriterInit(&writer, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);
    ASSERT_EQ(streamWriterWrite(&writer, payload, payload_len), C_OK);
    ASSERT_EQ(streamWriterFinish(&writer), C_OK);
    streamWriterFree(&writer);

    sds compressed = sdsnewlen(db.data, sdslen((const char *)db.data));
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, compressed);
    streamReader reader;
    ASSERT_EQ(initVcsRdbStreamReader(&reader, &buffer_rio), 0);

    buffer_rio.max_processing_chunk = chunk_size;
    buffer_rio.update_cksum = countRioUpdateCalls;
    rio_update_calls = 0;
    uint8_t result[payload_len];
    ASSERT_NE(rioRead(&buffer_rio, result, payload_len), 0u);
    ASSERT_EQ(rio_update_calls, payload_len / chunk_size);
    ASSERT_EQ(buffer_rio.processed_bytes, payload_len);
    EXPECT_EQ(memcmp(result, payload, payload_len), 0);

    rdbFreeStreamReader(&buffer_rio, &reader);
    sdsfree(compressed);
    dynamicBufFree(&db);
}

TEST(CompressionTest, rdbStreamReaderRewindsPlainFileAfterProbe) {
    const char *payload = "REDIS001remaining data after prefix";
    size_t payload_len = strlen(payload);
    FILE *fp = tmpfile();
    ASSERT_NE(fp, (FILE *)NULL);
    ASSERT_EQ(fwrite(payload, 1, payload_len, fp), payload_len);
    rewind(fp);

    rio file_rio;
    rioInitWithFile(&file_rio, fp);
    streamReader reader;
    compressionAlgo algo = ALGO_NONE;
    ASSERT_EQ(rdbInitStreamReader(&file_rio, &reader, false, &algo), RDB_STREAM_READER_INIT_OK);
    ASSERT_EQ(algo, ALGO_NONE);
    ASSERT_EQ(file_rio.stream_reader, (streamReader *)NULL);
    ASSERT_EQ(file_rio.stream_processed_bytes, 0u);
    ASSERT_EQ(ftello(fp), 0);

    char result[64];
    memset(result, 0, sizeof(result));
    ASSERT_NE(rioRead(&file_rio, result, payload_len), 0u);
    EXPECT_EQ(memcmp(result, payload, payload_len), 0);

    rdbFreeStreamReader(&file_rio, &reader);
    fclose(fp);
}

TEST(CompressionTest, rdbStreamReaderFallsBackWhenPlainFileCannotRewind) {
    const char *payload = "REDIS001remaining data after prefix";
    size_t payload_len = strlen(payload);
    int pipe_fds[2];
    ASSERT_EQ(pipe(pipe_fds), 0);
    ASSERT_EQ(write(pipe_fds[1], payload, payload_len), (ssize_t)payload_len);
    ASSERT_EQ(close(pipe_fds[1]), 0);

    FILE *fp = fdopen(pipe_fds[0], "r");
    ASSERT_NE(fp, (FILE *)NULL);
    rio file_rio;
    rioInitWithFile(&file_rio, fp);
    streamReader reader;
    compressionAlgo algo = ALGO_NONE;
    ASSERT_EQ(rdbInitStreamReader(&file_rio, &reader, false, &algo), RDB_STREAM_READER_INIT_OK);
    ASSERT_EQ(algo, ALGO_NONE);
    ASSERT_EQ(file_rio.stream_reader, &reader);

    char result[64];
    memset(result, 0, sizeof(result));
    ASSERT_NE(rioRead(&file_rio, result, payload_len), 0u);
    EXPECT_EQ(memcmp(result, payload, payload_len), 0);

    rdbFreeStreamReader(&file_rio, &reader);
    fclose(fp);
}

TEST(CompressionTest, rioStreamReaderClassifiesInput) {
    {
        const char *payload = "REDIS001remaining data after prefix";
        size_t payload_len = strlen(payload);
        sds buf = sdsnewlen(payload, payload_len);
        rio buffer_rio;
        rioInitWithBuffer(&buffer_rio, buf);
        enableTestRioBufferPartialReads(&buffer_rio);

        streamReader reader;
        compressionAlgo algo = ALGO_NONE;
        ASSERT_EQ(rdbInitStreamReader(&buffer_rio, &reader, false, &algo), RDB_STREAM_READER_INIT_OK);
        ASSERT_EQ(algo, ALGO_NONE) << "passthrough stream should not be compressed";
        ASSERT_EQ(buffer_rio.stream_reader, &reader)
            << "non-file sources should retain the passthrough fallback";

        char result[64];
        memset(result, 0, sizeof(result));
        ASSERT_NE(rioRead(&buffer_rio, result, payload_len), 0u) << "rioRead should succeed";
        EXPECT_EQ(memcmp(result, payload, payload_len), 0) << "payload should be replayed exactly";

        rdbFreeStreamReader(&buffer_rio, &reader);
        sdsfree(buf);
    }

    {
        const uint8_t malformed[VCS_ENVELOPE_SIZE] = {
            VCS_MAGIC_0, VCS_MAGIC_1, VCS_MAGIC_2, 0, VCS_CODEC_LZ4, 0, VCS_STREAM_RDB};
        sds buf = sdsnewlen(malformed, sizeof(malformed));
        rio buffer_rio;
        rioInitWithBuffer(&buffer_rio, buf);
        enableTestRioBufferPartialReads(&buffer_rio);

        streamReader reader;
        ASSERT_EQ(rdbInitStreamReader(&buffer_rio, &reader, false, NULL),
                  RDB_STREAM_READER_INIT_INCOMPATIBLE);

        sdsfree(buf);
    }
}

TEST(CompressionTest, rioCompressionWriterFinishIdempotent) {
    sds buf = sdsempty();
    rio buffer_rio;
    rioInitWithBuffer(&buffer_rio, buf);

    streamWriter writer;
    ASSERT_EQ(attachCompressionWriter(&buffer_rio, &writer), 0);

    ASSERT_NE(rioWrite(&buffer_rio, "test", 4), 0u);
    ASSERT_EQ(finishCompressionWriter(&buffer_rio, &writer), 0);
    size_t len_after_first = sdslen(buffer_rio.io.buffer.ptr);

    ASSERT_EQ(finishCompressionWriter(&buffer_rio, &writer), 0);
    size_t len_after_second = sdslen(buffer_rio.io.buffer.ptr);
    ASSERT_EQ(len_after_first, len_after_second) << "second finish should not produce more output";

    freeCompressionWriter(&buffer_rio, &writer);
    sdsfree(buffer_rio.io.buffer.ptr);
}

/* The reader stops at the frame boundary so callers can manage subsequent
 * bytes on a long-lived stream. */
TEST(CompressionTest, streamReaderFinishStopsAtFrameEndBeforeTrailingBytes) {
    const char *payload = "stream-reader-frame-end";
    const size_t payload_len = strlen(payload);
    const char *trailer = "TRAILER-BYTES-AFTER-FRAME";
    const size_t trailer_len = strlen(trailer);

    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter w;
    ASSERT_EQ(streamWriterInit(&w, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);
    ASSERT_EQ(streamWriterWrite(&w, payload, payload_len), C_OK);
    ASSERT_EQ(streamWriterFinish(&w), C_OK);
    streamWriterFree(&w);
    size_t frame_len = sdslen((const char *)db.data);

    sds input = sdsnewlen(db.data, sdslen((const char *)db.data));
    input = sdscatlen(input, trailer, trailer_len);

    MemReader mr = {};
    mr.data = (const uint8_t *)input;
    mr.len = sdslen(input);
    mr.max_chunk = 0;
    streamReaderConfig rcfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_MIN, false);
    streamReader r;
    ASSERT_EQ(streamReaderInit(&r, &rcfg, memReaderRead, &mr, NULL), C_OK);

    char out[128];
    memset(out, 0, sizeof(out));
    ASSERT_EQ(streamReaderRead(&r, out, payload_len), (ssize_t)payload_len);
    EXPECT_EQ(memcmp(out, payload, payload_len), 0);

    ASSERT_EQ(streamReaderFinish(&r), C_OK);
    ASSERT_EQ(r.error_kind, STREAM_READER_ERROR_NONE);
    ASSERT_EQ(mr.pos, frame_len) << "streamReader must not consume bytes after the LZ4 frame";

    streamReaderFree(&r);
    sdsfree(input);
    dynamicBufFree(&db);
}

TEST(CompressionTest, streamReaderRejectsTruncatedFrameTrailer) {
    const size_t payload_len = 256;
    uint8_t payload[payload_len];
    for (size_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter w;
    ASSERT_EQ(streamWriterInit(&w, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);
    ASSERT_EQ(streamWriterWrite(&w, payload, payload_len), C_OK);
    ASSERT_EQ(streamWriterFinish(&w), C_OK);
    streamWriterFree(&w);

    ASSERT_GT(sdslen((const char *)db.data), (size_t)VCS_ENVELOPE_SIZE + 1);
    MemReader mr = {};
    mr.data = db.data;
    mr.len = sdslen((const char *)db.data) - 1;
    mr.max_chunk = 7;
    streamReaderConfig rcfg = makeReaderConfig(false, STREAM_READER_BUFFER_SIZE_MIN, false);
    streamReader r;
    ASSERT_EQ(streamReaderInit(&r, &rcfg, memReaderRead, &mr, NULL), C_OK);

    uint8_t out[payload_len];
    ASSERT_EQ(streamReaderRead(&r, out, payload_len), (ssize_t)payload_len);
    EXPECT_EQ(memcmp(out, payload, payload_len), 0);
    ASSERT_LT(streamReaderRead(&r, out, 1), 0) << "EOF before frame end should be treated as corruption";
    ASSERT_EQ(r.error_kind, STREAM_READER_ERROR_CORRUPT)
        << "truncated compressed frame should latch corruption, not I/O";

    streamReaderFree(&r);
    dynamicBufFree(&db);
}

TEST(CompressionTest, streamWriterWriteAfterFinish) {
    DynamicBuf db;
    dynamicBufInit(&db);
    streamWriter t;
    ASSERT_EQ(streamWriterInit(&t, ALGO_LZ4, true, emitToDynamicBuf, &db), C_OK);

    ASSERT_EQ(streamWriterWrite(&t, "hello", 5), C_OK);
    ASSERT_EQ(streamWriterFinish(&t), C_OK);
    size_t len_after_finish = sdslen((const char *)db.data);

    ASSERT_EQ(streamWriterWrite(&t, "world", 5), C_ERR);
    ASSERT_EQ(sdslen((const char *)db.data), len_after_finish) << "write after finish should not produce output";

    ASSERT_EQ(streamWriterFinish(&t), C_OK);
    ASSERT_EQ(sdslen((const char *)db.data), len_after_finish) << "second finish should not produce output";

    ASSERT_GT(sdslen((const char *)db.data), (size_t)VCS_ENVELOPE_SIZE);
    streamDecompressor sd;
    ASSERT_EQ(streamDecompressorInit(&sd, ALGO_LZ4, false), C_OK);

    uint8_t decompressed[64];
    uint8_t *cdata = db.data + VCS_ENVELOPE_SIZE;
    size_t comp_len = sdslen((const char *)db.data) - VCS_ENVELOPE_SIZE;
    ssize_t total = decompressAll(&sd, cdata, comp_len, decompressed, sizeof(decompressed));

    ASSERT_EQ(total, 5);
    EXPECT_EQ(memcmp(decompressed, "hello", 5), 0) << "should decompress to 'hello' only";

    streamDecompressorFree(&sd);
    streamWriterFree(&t);
    dynamicBufFree(&db);
}
