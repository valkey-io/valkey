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
    streamWriter *writer;
    bool finalized;
} compressRio;

/* Decompression rio wrapper. */
typedef struct {
    rio base; /* Must be first */
    rio *inner;
    streamReader *reader;
} decompressRio;

typedef enum {
    DECOMPRESS_RIO_INIT_ERROR = -1,
    DECOMPRESS_RIO_INIT_OK = 0,
    DECOMPRESS_RIO_INIT_INCOMPATIBLE = 1,
} decompressRioInitResult;

/* --- Rio Decorator API --- */
int rioInitWithCompress(compressRio *cr, rio *inner, const streamWriterConfig *cfg);
int compressRioFinish(compressRio *cr);
void compressRioDestroy(compressRio *cr);

/* Initialize and probe a decompression adapter in one step.
 * Returns OK for both passthrough and compressed streams, INCOMPATIBLE for
 * malformed/unexpected stream envelopes, and ERROR for I/O or setup failures. */
decompressRioInitResult rioInitWithDecompress(decompressRio *dr,
                                              rio *inner,
                                              const streamReaderConfig *cfg,
                                              streamReaderInfo *info);
streamReaderError decompressRioGetError(const decompressRio *dr);
/* Destroy the adapter without additional I/O. */
void decompressRioDestroy(decompressRio *dr);

#endif /* COMPRESSION_RIO_H */
