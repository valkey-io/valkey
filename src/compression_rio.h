/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_RIO_H
#define COMPRESSION_RIO_H

#include "compression_stream.h"
#include "rio.h"

/* Compression rio wrapper. */
typedef struct {
    rio base; /* Must be first — allows casting to (rio *) */
    rio *inner;
    stream_writer_t *writer;
    bool finalized;
} compress_rio_t;

/* Decompression rio wrapper. */
typedef struct {
    rio base; /* Must be first */
    rio *inner;
    stream_reader_t *reader;
} decompress_rio_t;

typedef enum {
    DECOMPRESS_RIO_INIT_ERROR = -1,
    DECOMPRESS_RIO_INIT_OK = 0,
    DECOMPRESS_RIO_INIT_INCOMPATIBLE = 1,
} decompress_rio_init_result_t;

/* --- Rio Decorator API --- */
int rioInitWithCompress(compress_rio_t *cr, rio *inner, const stream_writer_config_t *cfg);
int compress_rio_finish(compress_rio_t *cr);
void compress_rio_destroy(compress_rio_t *cr);

/* Initialize and probe a decompression adapter in one step.
 * Returns OK for both passthrough and compressed streams, INCOMPATIBLE for
 * malformed/unexpected stream envelopes, and ERROR for I/O or setup failures. */
decompress_rio_init_result_t rioInitWithDecompress(decompress_rio_t *dr,
                                                   rio *inner,
                                                   const stream_reader_config_t *cfg,
                                                   stream_reader_info_t *info);
stream_reader_error_t decompress_rio_get_error(const decompress_rio_t *dr);
/* Destroy the adapter without additional I/O. */
void decompress_rio_destroy(decompress_rio_t *dr);

#endif /* COMPRESSION_RIO_H */
