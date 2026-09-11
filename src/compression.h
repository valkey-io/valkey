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
} compressFlushMode;

/* Returns a static algorithm name for logs and config output. */
const char *compressionAlgoName(compressionAlgo algo);

/* ===== Compressor ===== */

typedef struct {
    compressionAlgo algo;
    int level; /* 0 selects the codec default. */
    void *ctx;
    bool stream_started;
    bool codec_checksum;
} streamCompressor;

/* Compressor lifecycle. Codec dispatch used by streamWriter; the writer owns
 * sticky error state while these functions manage only codec state. */
int streamCompressorInit(streamCompressor *compressor, compressionAlgo algo, int level, bool codec_checksum);
size_t streamCompressorOutputBound(const streamCompressor *compressor, size_t input_len);
/* Feeds raw input into the compressor and writes compressed bytes to output.
 * Called repeatedly to build a complete frame: COMPRESS_FLUSH_CONTINUE keeps
 * buffering and COMPRESS_FLUSH_END closes the frame. output must be at least
 * streamCompressorOutputBound(compressor, input_len) bytes. Returns bytes written,
 * or -1 on error. */
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
    size_t input_hint; /* Preferred compressed bytes for next feed, 0 if unknown. */
} streamDecompressor;

/* Decompressor lifecycle. Codec dispatch used by streamReader; the reader owns
 * buffering and sticky error state. */
int streamDecompressorInit(streamDecompressor *decompressor,
                           compressionAlgo algo,
                           bool skip_codec_checksum_validation);
ssize_t streamDecompressorFeed(streamDecompressor *decompressor,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed);
void streamDecompressorFree(streamDecompressor *decompressor);

#endif /* COMPRESSION_H */
