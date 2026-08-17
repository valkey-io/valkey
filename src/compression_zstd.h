/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_ZSTD_H
#define COMPRESSION_ZSTD_H

#include "compression.h"

int compressionZstdCompressorInit(streamCompressor *sc);
void compressionZstdCompressorFree(streamCompressor *sc);
int compressionZstdDecompressorInit(streamDecompressor *sd);
void compressionZstdDecompressorFree(streamDecompressor *sd);
size_t compressionZstdOutputBound(size_t input_len);

ssize_t compressionZstdCompressFeed(streamCompressor *sc,
                                    uint8_t *output,
                                    size_t output_capacity,
                                    const uint8_t *input,
                                    size_t input_len,
                                    compressFlushMode flush_mode);

ssize_t compressionZstdDecompressFeed(streamDecompressor *sd,
                                      uint8_t *output,
                                      size_t output_capacity,
                                      const uint8_t *input,
                                      size_t input_len,
                                      size_t *input_consumed);

#endif /* COMPRESSION_ZSTD_H */
