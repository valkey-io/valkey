/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_LZ4_H
#define COMPRESSION_LZ4_H

#include "compression.h"

/* Initialize an LZ4 compression context owned by `sc`; returns 0 on success or
 * -1 on invalid input/allocation failure. The caller owns `sc` storage and must
 * destroy a successfully initialized context. */
int compressionLz4CompressorInit(streamCompressor *sc);

/* Release the LZ4 compression context owned by `sc`; accepts NULL or an
 * uninitialized context and leaves `sc->ctx` NULL. */
void compressionLz4CompressorDestroy(streamCompressor *sc);

/* Initialize an LZ4 decompression context owned by `sd`; returns 0 on success
 * or -1 on invalid input/allocation failure. The caller owns `sd` storage and
 * must destroy a successfully initialized context. */
int compressionLz4DecompressorInit(streamDecompressor *sd);

/* Release the LZ4 decompression context owned by `sd`; accepts NULL or an
 * uninitialized context and leaves `sd->ctx` NULL. */
void compressionLz4DecompressorDestroy(streamDecompressor *sd);

/* Return a conservative per-call output capacity for `input_len`, including
 * frame header and flush/end overhead so callers can reuse one scratch buffer
 * across write, sync-flush, and finish calls. */
size_t compressionLz4OutputBound(size_t input_len);

/* Feed input to the LZ4 frame writer. Returns produced bytes, or -1 on failure;
 * capacity shortages before LZ4 state changes are retriable, while LZ4 errors
 * latch `sc->errored` and require tearing down the stream. */
ssize_t compressionLz4CompressFeed(streamCompressor *sc,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   const uint8_t *input,
                                   size_t input_len,
                                   compressFlushMode flush_mode);

/* Feed compressed bytes to the LZ4 frame reader. Returns produced bytes, or -1
 * and latches `sd->errored` on invalid input; `input_consumed` is set to bytes
 * consumed so callers can retain any unconsumed suffix and retry with output
 * space instead of dropping input. */
ssize_t compressionLz4DecompressFeed(streamDecompressor *sd,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     const uint8_t *input,
                                     size_t input_len,
                                     size_t *input_consumed);

#endif /* COMPRESSION_LZ4_H */
