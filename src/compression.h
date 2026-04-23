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

/* --- Algorithm identifiers --- */
typedef enum {
    ALGO_NONE = 0x00, /* Disabled */
    ALGO_LZF = 0x01,  /* Per-string LZF (RDB only, existing behavior) */
    ALGO_LZ4 = 0x02,
} compression_algo_t;

/* --- Flush modes for streaming compression --- */
typedef enum {
    FLUSH_CONTINUE = 0, /* Buffer internally */
    FLUSH_SYNC = 1,     /* Emit all buffered data, keep frame open */
    FLUSH_END = 2,      /* Finalize frame */
} compress_flush_mode_t;

/* --- Streaming compressor context --- */
typedef struct {
    compression_algo_t algo;
    int level;
    void *ctx; /* Private codec-specific compressor context. */
    bool stream_started;
    bool errored;        /* Permanently failed — algorithm state is undefined after
                          * an error. All subsequent streamCompressFeed calls return
                          * -1 immediately. The caller must tear down the stream
                          * (disconnect replica / abort RDB save). No mid-stream
                          * retry is possible because already-emitted frame bytes
                          * cannot be unsent. */
    bool codec_checksum; /* Codec-native integrity toggle.
                          * For LZ4 streams this maps to per-block checksums. */
} stream_compressor_t;

/* --- Streaming decompressor context --- */
typedef struct {
    compression_algo_t algo;
    void *ctx;       /* Private codec-specific decompressor context. */
    bool errored;    /* Permanently failed — algorithm state is undefined after
                      * an error. All subsequent streamDecompressFeed calls
                      * return -1 immediately until reinitialized. */
    bool frame_done; /* Set when the codec frame is fully decoded.
                      * Subsequent streamDecompressFeed calls return 0
                      * immediately (no more output). */
} stream_decompressor_t;

/* Returns true if the algorithm supports streaming codec framing. */
bool compressionAlgoSupportsStreaming(compression_algo_t algo);

/* Returns a stable name for logging/debugging. */
const char *compressionAlgoName(compression_algo_t algo);

/* --- Streaming compression API --- */

/* Initialize/destroy streaming compressor. */
int streamCompressorInit(stream_compressor_t *sc, compression_algo_t algo, int level);
void streamCompressorDestroy(stream_compressor_t *sc);

/* Initialize/destroy streaming decompressor. */
int streamDecompressorInit(stream_decompressor_t *sd, compression_algo_t algo);
void streamDecompressorDestroy(stream_decompressor_t *sd);

/* Returns true when the codec frame has been fully decoded. */
bool streamDecompressorFrameDone(const stream_decompressor_t *sd);

/* Return upper bound on compressed output size.
 * The bound is conservative: it includes frame header, data, and flush/end
 * overhead so the caller can allocate once and reuse for any flush mode. */
size_t streamCompressOutputBound(const stream_compressor_t *sc, size_t input_len);

/* Feed data through streaming compressor.
 * flush_mode: FLUSH_CONTINUE, FLUSH_SYNC, or FLUSH_END.
 * Returns bytes written to output, 0 for no output, -1 on error. */
ssize_t streamCompressFeed(stream_compressor_t *sc,
                           uint8_t *output,
                           size_t output_capacity,
                           const uint8_t *input,
                           size_t input_len,
                           compress_flush_mode_t flush_mode);

/* Feed compressed data through streaming decompressor.
 * Returns bytes written to output, 0 for no output, -1 on error.
 * *input_consumed is set to the number of compressed bytes consumed.
 * Fatal errors latch stream_decompressor_t.errored until reinit. */
ssize_t streamDecompressFeed(stream_decompressor_t *sd,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             size_t *input_consumed);

#endif /* COMPRESSION_H */
