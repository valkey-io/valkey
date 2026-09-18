/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_H
#define COMPRESSION_H

#include "fmacros.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    ALGO_NONE = 0,
    ALGO_LZF = 1, /* Per-string LZF inside the RDB payload (legacy). */
    ALGO_LZ4 = 2,
    ALGO_ZSTD = 3,
} compressionAlgo;

typedef enum {
    COMPRESS_FLUSH_CONTINUE = 0, /* Buffer internally. */
    COMPRESS_FLUSH_END = 1,      /* Finalize frame. */
    COMPRESS_FLUSH_SYNC = 2,     /* Drain buffered bytes, keep frame open. */
} compressFlushMode;

const char *compressionAlgoName(compressionAlgo algo);

/* ===== Compressor ===== */

#define STREAM_CHECKSUM_BLOCK (1u << 0)
#define STREAM_CHECKSUM_CONTENT (1u << 1)

typedef struct {
    compressionAlgo algo;
    int level; /* 0 selects the codec default. */
    void *ctx;
    size_t ctx_memory; /* Bytes held by ctx via the codec's custom allocator, for memory accounting. */
    bool stream_started;
    uint8_t checksum_flags;
} streamCompressor;

int streamCompressorInit(streamCompressor *compressor, compressionAlgo algo, int level, uint8_t checksum_flags);
size_t streamCompressorOutputBound(const streamCompressor *compressor, size_t input_len);
ssize_t streamCompressorFeed(streamCompressor *compressor,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             compressFlushMode flush_mode);
void streamCompressorFree(streamCompressor *compressor);

/* ===== Decompressor ===== */

typedef struct {
    compressionAlgo algo;
    bool frame_done;
    bool skip_codec_checksum_validation;
    void *ctx;
    size_t ctx_memory; /* Bytes held by ctx via the codec's custom allocator, for memory accounting. */
    size_t input_hint; /* Preferred compressed bytes for next feed, 0 if unknown. */
} streamDecompressor;

int streamDecompressorInit(streamDecompressor *decompressor,
                           compressionAlgo algo,
                           bool skip_codec_checksum_validation);
ssize_t streamDecompressorFeed(streamDecompressor *decompressor,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed);
/* Start the next frame while retaining codec allocations. Only codecs whose
 * wire format permits concatenated frames implement this operation. */
int streamDecompressorReset(streamDecompressor *decompressor);
void streamDecompressorFree(streamDecompressor *decompressor);

#endif /* COMPRESSION_H */
