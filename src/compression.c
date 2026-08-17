/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression.h"
#include "compression_lz4.h"
#ifdef HAVE_ZSTD
#include "compression_zstd.h"
#endif
#include "server.h"
#include "serverassert.h"
#include <string.h>

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

/* ===== Compressor ===== */

int streamCompressorInit(streamCompressor *compressor,
                         compressionAlgo algo,
                         int level,
                         bool codec_checksum) {
    memset(compressor, 0, sizeof(*compressor));
    compressor->algo = algo;
    compressor->level = level;
    compressor->codec_checksum = codec_checksum;

    switch (algo) {
    case ALGO_LZ4:
        return compressionLz4CompressorInit(compressor);
    case ALGO_ZSTD:
#ifdef HAVE_ZSTD
        return compressionZstdCompressorInit(compressor);
#else
        return C_ERR;
#endif
    default:
        return C_ERR;
    }
}

size_t streamCompressorOutputBound(const streamCompressor *compressor, size_t input_len) {
    switch (compressor->algo) {
    case ALGO_LZ4:
        return compressionLz4OutputBound(input_len);
    case ALGO_ZSTD:
#ifdef HAVE_ZSTD
        return compressionZstdOutputBound(input_len);
#else
        panic("Zstandard support is not compiled in");
#endif
    default:
        panic("Unsupported stream compression algorithm: %d", compressor->algo);
    }
}

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
#ifdef HAVE_ZSTD
        return compressionZstdCompressFeed(compressor, output, output_capacity, input, input_len, flush_mode);
#else
        panic("Zstandard support is not compiled in");
#endif
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
#ifdef HAVE_ZSTD
        compressionZstdCompressorFree(compressor);
#endif
        break;
    default:
        break;
    }
}

/* ===== Decompressor ===== */

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
#ifdef HAVE_ZSTD
        return compressionZstdDecompressorInit(decompressor);
#else
        return C_ERR;
#endif
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
#ifdef HAVE_ZSTD
        return compressionZstdDecompressFeed(decompressor, output, output_capacity,
                                             input, input_len, input_consumed);
#else
        panic("Zstandard support is not compiled in");
#endif
    default:
        panic("Unsupported stream decompression algorithm: %d", decompressor->algo);
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
#ifdef HAVE_ZSTD
        compressionZstdDecompressorFree(decompressor);
#endif
        break;
    default:
        panic("Unsupported stream decompression algorithm: %d", decompressor->algo);
    }
}
