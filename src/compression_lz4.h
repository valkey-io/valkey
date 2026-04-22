/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_LZ4_H
#define COMPRESSION_LZ4_H

#include "compression.h"

int compressionLz4CompressorInit(stream_compressor_t *sc);
void compressionLz4CompressorDestroy(stream_compressor_t *sc);
int compressionLz4DecompressorInit(stream_decompressor_t *sd);
void compressionLz4DecompressorDestroy(stream_decompressor_t *sd);
size_t compressionLz4OutputBound(size_t input_len);
ssize_t compressionLz4CompressFeed(stream_compressor_t *sc,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   const uint8_t *input,
                                   size_t input_len,
                                   compress_flush_mode_t flush_mode);
ssize_t compressionLz4DecompressFeed(stream_decompressor_t *sd,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     const uint8_t *input,
                                     size_t input_len,
                                     size_t *input_consumed);

#endif /* COMPRESSION_LZ4_H */
