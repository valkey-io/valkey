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
} compressionAlgo;

typedef enum {
    FLUSH_CONTINUE = 0, /* Buffer internally. */
    FLUSH_SYNC = 1,     /* Drain buffered bytes, keep frame open. */
    FLUSH_END = 2,      /* Finalize frame. */
} compressFlushMode;

typedef struct {
    compressionAlgo algo;
    int level;
    void *ctx;
    bool stream_started;
    /* Sticky failure. Already-emitted frame bytes cannot be unsent, so the
     * caller must tear the stream down rather than retry. */
    bool errored;
    bool codec_checksum;
} streamCompressor;

typedef struct {
    compressionAlgo algo;
    bool errored;
    bool frame_done;
    void *ctx;
    size_t input_hint; /* Preferred compressed bytes for next feed, 0 if unknown. */
} streamDecompressor;

bool compressionAlgoSupportsStreaming(compressionAlgo algo);
const char *compressionAlgoName(compressionAlgo algo);

int streamCompressorInit(streamCompressor *sc, compressionAlgo algo, int level);
void streamCompressorDestroy(streamCompressor *sc);

int streamDecompressorInit(streamDecompressor *sd, compressionAlgo algo);
void streamDecompressorDestroy(streamDecompressor *sd);

bool streamDecompressorFrameDone(const streamDecompressor *sd);
size_t streamDecompressorInputHint(const streamDecompressor *sd);

/* Conservative bound covering header + data + flush/end overhead, so the
 * caller can size one scratch buffer for all flush modes. */
size_t streamCompressOutputBound(const streamCompressor *sc, size_t input_len);

/* Returns bytes written, or -1 on error. */
ssize_t streamCompressFeed(streamCompressor *sc,
                           uint8_t *output,
                           size_t output_capacity,
                           const uint8_t *input,
                           size_t input_len,
                           compressFlushMode flush_mode);

/* Returns bytes written, or -1 on error. *input_consumed is set so the caller
 * can retain any unconsumed suffix and retry with more output space. */
ssize_t streamDecompressFeed(streamDecompressor *sd,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             size_t *input_consumed);

#endif /* COMPRESSION_H */
