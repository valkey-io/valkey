/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression.h"
#include "compression_lz4.h"
#include "compression_zstd.h"
#include "server.h"
#include "serverassert.h"
#include <string.h>

/* Returns a static algorithm name for logs and config output. */
const char *compressionAlgoName(compressionAlgo algo) {
    switch (algo) {
    case ALGO_NONE:
        return "none";
    case ALGO_LZF:
        return "lzf";
    case ALGO_LZ4:
        return "lz4";
    case ALGO_ZSTD:
        return "zstd";
    default:
        return "unknown";
    }
}

bool streamCodecIsSupported(compressionAlgo algo) {
    switch (algo) {
    case ALGO_LZ4:
        return true;
    case ALGO_ZSTD:
        return compressionZstdIsSupported();
    default:
        return false;
    }
}

uint8_t streamCodecIntegrityChecksumFlags(compressionAlgo algo) {
    switch (algo) {
    case ALGO_LZ4:
        return STREAM_CHECKSUM_BLOCK;
    case ALGO_ZSTD:
        return STREAM_CHECKSUM_CONTENT;
    default:
        panic("Unsupported stream compression algorithm: %d", algo);
    }
}

bool streamCodecNeedsBoundedFramesForIntegrity(compressionAlgo algo) {
    switch (algo) {
    case ALGO_LZ4:
        return false;
    case ALGO_ZSTD:
        return true;
    default:
        panic("Unsupported stream compression algorithm: %d", algo);
    }
}

/* ===== Compressor ===== */

/* Compressor lifecycle. Codec dispatch used by streamWriter and by the
 * replication write path; callers own sticky error state while these
 * functions manage only codec state. checksum_flags is a bitwise combination
 * of STREAM_CHECKSUM_* values. */
int streamCompressorInit(streamCompressor *compressor,
                         compressionAlgo algo,
                         int level,
                         uint8_t checksum_flags) {
    memset(compressor, 0, sizeof(*compressor));
    compressor->algo = algo;
    compressor->level = level;
    compressor->checksum_flags = checksum_flags;

    switch (algo) {
    case ALGO_LZ4:
        return compressionLz4CompressorInit(compressor);
    case ALGO_ZSTD:
        return compressionZstdCompressorInit(compressor);
    default:
        return C_ERR;
    }
}

size_t streamCompressorOutputBound(const streamCompressor *compressor, size_t input_len) {
    switch (compressor->algo) {
    case ALGO_LZ4:
        return compressionLz4OutputBound(input_len);
    case ALGO_ZSTD:
        return compressionZstdOutputBound(input_len);
    default:
        panic("Unsupported stream compression algorithm: %d", compressor->algo);
    }
}

/* Feeds raw input into the compressor and writes compressed bytes to output.
 * Called repeatedly to build a complete frame: COMPRESS_FLUSH_CONTINUE keeps
 * buffering, COMPRESS_FLUSH_SYNC drains buffered bytes but leaves the frame
 * open, and COMPRESS_FLUSH_END closes it. output must be at least
 * streamCompressorOutputBound(compressor, input_len) bytes. Returns bytes
 * written, or -1 on error. */
ssize_t streamCompressorFeed(streamCompressor *compressor,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             compressFlushMode flush_mode) {
    switch (compressor->algo) {
    case ALGO_LZ4:
        return compressionLz4CompressFeed(compressor, output, output_capacity, input, input_len, flush_mode);
    case ALGO_ZSTD:
        return compressionZstdCompressFeed(compressor, output, output_capacity, input, input_len, flush_mode);
    default:
        panic("Unsupported stream compression algorithm: %d", compressor->algo);
    }
}

void streamCompressorFree(streamCompressor *compressor) {
    switch (compressor->algo) {
    case ALGO_LZ4:
        compressionLz4CompressorFree(compressor);
        break;
    case ALGO_ZSTD:
        compressionZstdCompressorFree(compressor);
        break;
    default:
        break;
    }
}

/* ===== Decompressor ===== */

/* Codec dispatch shared by the pull and push stream readers. */
int streamDecompressorInit(streamDecompressor *decompressor,
                           compressionAlgo algo,
                           bool skip_codec_checksum_validation) {
    memset(decompressor, 0, sizeof(*decompressor));
    decompressor->algo = algo;
    decompressor->skip_codec_checksum_validation = skip_codec_checksum_validation;

    switch (algo) {
    case ALGO_LZ4:
        return compressionLz4DecompressorInit(decompressor);
    case ALGO_ZSTD:
        return compressionZstdDecompressorInit(decompressor);
    default:
        return C_ERR;
    }
}

ssize_t streamDecompressorFeed(streamDecompressor *decompressor,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed) {
    *input_consumed = 0;
    if (decompressor->frame_done) return 0;

    switch (decompressor->algo) {
    case ALGO_LZ4:
        return compressionLz4DecompressFeed(decompressor, output, output_capacity,
                                            input, input_len, input_consumed);
    case ALGO_ZSTD:
        return compressionZstdDecompressFeed(decompressor, output, output_capacity,
                                             input, input_len, input_consumed);
    default:
        panic("Unsupported stream decompression algorithm: %d", decompressor->algo);
    }
}

int streamDecompressorReset(streamDecompressor *decompressor) {
    switch (decompressor->algo) {
    case ALGO_LZ4:
        return compressionLz4DecompressorReset(decompressor);
    case ALGO_ZSTD:
        return compressionZstdDecompressorReset(decompressor);
    default:
        return C_ERR;
    }
}

void streamDecompressorFree(streamDecompressor *decompressor) {
    switch (decompressor->algo) {
    case ALGO_NONE:
        break;
    case ALGO_LZ4:
        compressionLz4DecompressorFree(decompressor);
        break;
    case ALGO_ZSTD:
        compressionZstdDecompressorFree(decompressor);
        break;
    default:
        panic("Unsupported stream decompression algorithm: %d", decompressor->algo);
    }
}
