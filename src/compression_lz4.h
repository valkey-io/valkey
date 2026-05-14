/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_LZ4_H
#define COMPRESSION_LZ4_H

#include "compression.h"

int compressionLz4CompressorInit(streamCompressor *sc);
void compressionLz4CompressorDestroy(streamCompressor *sc);
int compressionLz4DecompressorInit(streamDecompressor *sd);
void compressionLz4DecompressorDestroy(streamDecompressor *sd);
size_t compressionLz4OutputBound(size_t input_len);

ssize_t compressionLz4CompressFeed(streamCompressor *sc,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   const uint8_t *input,
                                   size_t input_len,
                                   compressFlushMode flush_mode);

ssize_t compressionLz4DecompressFeed(streamDecompressor *sd,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     const uint8_t *input,
                                     size_t input_len,
                                     size_t *input_consumed);

#endif /* COMPRESSION_LZ4_H */
