/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_LZ4_H
#define COMPRESSION_LZ4_H

#include "compression.h"

/* Compressor lifecycle. Init returns C_OK/C_ERR. OutputBound includes enough
 * space for frame start and any requested flush mode. Feed returns -1 on codec
 * failure; input may be NULL when input_len is zero. */
int compressionLz4CompressorInit(streamCompressor *compressor);
size_t compressionLz4OutputBound(size_t input_len);
ssize_t compressionLz4CompressFeed(streamCompressor *compressor,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   const uint8_t *input,
                                   size_t input_len,
                                   compressFlushMode flush_mode);
void compressionLz4CompressorFree(streamCompressor *compressor);

/* Decompressor lifecycle. Init returns C_OK/C_ERR. Feed returns produced bytes
 * or -1 and reports consumed compressed bytes through input_consumed. A zero
 * return does not by itself mean frame end; callers inspect
 * decompressor->frame_done. */
int compressionLz4DecompressorInit(streamDecompressor *decompressor);
ssize_t compressionLz4DecompressFeed(streamDecompressor *decompressor,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     const uint8_t *input,
                                     size_t input_len,
                                     size_t *input_consumed);
void compressionLz4DecompressorFree(streamDecompressor *decompressor);

#endif /* COMPRESSION_LZ4_H */
