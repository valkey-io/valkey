/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_STREAM_H
#define COMPRESSION_STREAM_H

#include "compression.h"

/* VCS envelope:
 *   [0..2] magic "VCS"
 *   [3]    version (currently VCS_VERSION)
 *   [4]    codec id
 *   [5]    reserved (must be zero)
 *   [6]    stream kind
 *
 * All fields are single-byte. Future multi-byte fields must use
 * network byte order. Codec ids are stable wire values independent of
 * compressionAlgo. Readers reject unknown versions, codecs, stream kinds,
 * and nonzero reserved fields. */
#define VCS_MAGIC_0 0x56 /* 'V' */
#define VCS_MAGIC_1 0x43 /* 'C' */
#define VCS_MAGIC_2 0x53 /* 'S' */
#define VCS_MAGIC_SIZE 3
#define VCS_ENVELOPE_SIZE 7
#define VCS_VERSION 1
/* Byte offsets of each envelope field. */
#define VCS_OFFSET_VERSION 3
#define VCS_OFFSET_CODEC 4
#define VCS_OFFSET_RESERVED 5
#define VCS_OFFSET_STREAM_KIND 6

/* Stable wire codec identifier. */
#define VCS_CODEC_LZ4 0x01

/* Identifies an RDB payload in the envelope. */
#define VCS_STREAM_RDB 0x01

typedef int (*streamWriterWriteFn)(void *ctx, const uint8_t *data, size_t len);
/* Returns >0 bytes read, 0 on EOF, -1 on error. Partial reads allowed. */
typedef ssize_t (*streamReaderReadFn)(void *ctx, void *buf, size_t len);

/* ===== Writer ===== */

typedef enum {
    STREAM_WRITER_STATE_INITIAL = 0,
    STREAM_WRITER_STATE_ACTIVE,
    STREAM_WRITER_STATE_FINISHED,
    STREAM_WRITER_STATE_ERROR,
} streamWriterState;

typedef struct streamWriter {
    streamCompressor compressor;
    uint8_t *out_buf;
    size_t out_buf_size;
    streamWriterWriteFn write_cb;
    void *write_ctx;
    streamWriterState state;
} streamWriter;

/* Writer API. Init uses the codec's default compression level and configures
 * its integrity checks according to codec_checksum. Init, Write, Finish, and
 * write_cb use C_OK/C_ERR. The writer pushes compressed bytes to write_cb.
 * Errors are sticky: later operations fail without emitting bytes. */
int streamWriterInit(streamWriter *writer, compressionAlgo algo, bool codec_checksum, streamWriterWriteFn write_cb, void *write_ctx);
int streamWriterWrite(streamWriter *writer, const void *buf, size_t len);
/* Finalizes the frame. Repeated calls after successful completion are safe. */
int streamWriterFinish(streamWriter *writer);
/* Releases resources without implicitly finalizing the frame. */
void streamWriterFree(streamWriter *writer);

/* ===== Reader ===== */

/* Default decompressed-output buffer size. Tiny caller values are clamped up
 * so the decoder can always make forward progress without growing internal
 * state. The compressed-input buffer only needs to hold one LZ4 block. */
#define STREAM_READER_BUFFER_SIZE_DEFAULT (1024 * 1024)
#define STREAM_READER_BUFFER_SIZE_MIN (128 * 1024)
#define STREAM_READER_COMPRESSED_BUFFER_SIZE (128 * 1024)

/* When allow_passthrough is set, non-VCS input is forwarded as raw bytes;
 * otherwise it is rejected. */
typedef struct {
    bool allow_passthrough;
    bool skip_codec_checksum_validation;
    size_t buffer_size;
} streamReaderConfig;

typedef enum {
    STREAM_READER_ERROR_NONE = 0,
    STREAM_READER_ERROR_IO = 1,
    STREAM_READER_ERROR_INCOMPATIBLE = 2,
    STREAM_READER_ERROR_CORRUPT = 3,
    STREAM_READER_ERROR_INTERNAL = 4,
} streamReaderErrorKind;

typedef enum {
    STREAM_READER_STATE_PASSTHROUGH = 0,
    STREAM_READER_STATE_COMPRESSED,
    STREAM_READER_STATE_FINISHED,
} streamReaderState;

typedef struct streamReader {
    streamReaderReadFn read_cb;
    void *read_ctx;
    struct {
        uint8_t header[VCS_ENVELOPE_SIZE];
        size_t header_len;
    } probe;
    size_t probe_replay_pos; /* Passthrough bytes left to replay from probe. */
    size_t buffer_size;
    streamReaderErrorKind error_kind;
    streamReaderState state;

    streamDecompressor decompressor;

    uint8_t *compressed_buf;
    size_t compressed_buf_size;
    size_t compressed_buf_pos;
    size_t compressed_buf_len;

    uint8_t *decompressed_buf;
    size_t decompressed_buf_pos;
    size_t decompressed_buf_len;
} streamReader;

/* Reader API. Initialization returns C_OK/C_ERR, probes the source, and may
 * call read_cb. On success, detected_algo receives ALGO_NONE for passthrough
 * input or the detected codec; the output pointer is optional. On failure,
 * error_kind classifies the error and the reader remains safe to free. */
int streamReaderInit(streamReader *reader, const streamReaderConfig *cfg, streamReaderReadFn read_cb, void *read_ctx, compressionAlgo *detected_algo);
/* Returns up to len bytes, 0 on EOF, or -1 on error. An error after partial
 * output is reported on the next call. */
ssize_t streamReaderRead(streamReader *reader, void *buf, size_t len);
/* Completes and validates a compressed frame after the logical parser has
 * consumed its payload. It stops at the frame boundary without requiring
 * physical EOF, matching the plain RDB loader's treatment of trailing bytes.
 * Returns C_OK/C_ERR. */
int streamReaderFinish(streamReader *reader);
void streamReaderFree(streamReader *reader);

#endif /* COMPRESSION_STREAM_H */
