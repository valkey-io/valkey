/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_BLOCK_INTERFACE_H
#define COMPRESSION_BLOCK_INTERFACE_H

#include "compression.h"

#include <stddef.h>

/* Block compression: one whole value in, one compressed buffer out. There is no
 * stream state between calls and no container header, so the output is as
 * small as the library can make it. This is the block API of LZ4 (lz4.h). The
 * stream compression in compression.h is different: it uses the LZ4 frame
 * format (lz4frame.h), with headers and checksums, for the RDB and replication
 * byte stream.
 *
 * There are three objects here.
 *
 *   blockCompressorApi     The compression function table. Every entry calls
 *                          straight into a backend library such as lz4 or zstd.
 *                          Nothing else lives here: no names, no limits, no
 *                          data. Shared, constant, and never allocated.
 *   blockCompressorConfig  What the operator asked for.
 *   blockCompressor        What callers hold. Made by blockCompressorCreate(),
 *                          released by blockCompressorFree(). Holds the
 *                          function table, all the plain data, and the
 *                          backend's private scratch.
 *
 * An instance is immutable apart from its scratch buffer and its error code.
 * Nothing changes an instance after it is built. A CONFIG SET that touches a
 * compression setting builds a new instance and drops the old one.
 *
 * An instance belongs to one thread and is never shared. That is what keeps the
 * whole thing free of locks and atomics: each worker thread has its own, and the
 * main thread has its own for decompressing on reads.
 *
 * The backend for new compression is picked once at startup from
 * compression-mode and never changes while the process runs. Frames saved to
 * RDB can outlive the process, so each frame still records its algorithm. */

typedef struct blockCompressor blockCompressor;

/* The algorithm ids are compressionAlgo from compression.h (ALGO_LZ4,
 * ALGO_ZSTD), the same ids that stream compression uses. They are stored with
 * every compressed value, so their numbers never change. ALGO_NONE and
 * ALGO_LZF have no block backend. */

/* Failure codes, shared by every backend. They land in
 * blockCompressor.last_error. The backend libraries report failure with a
 * return code and have no error codes of their own, so these are ours.
 *
 * They are shared rather than per backend because none of them is specific to a
 * library, and one shared list means one place to map a code to text. */
typedef enum {
    BLOCK_COMPRESSOR_ERR_NONE = 0,
    BLOCK_COMPRESSOR_ERR_BAD_SIZE,   /* length out of range for this backend */
    BLOCK_COMPRESSOR_ERR_COMPRESS,   /* the library refused to compress */
    BLOCK_COMPRESSOR_ERR_DECOMPRESS, /* the library refused to decompress */
} blockCompressorErr;

/* What the operator asked for. Copied into an instance and then frozen. */
typedef struct blockCompressorConfig {
    size_t min_input_len; /* compression-min-value-size. 0 means no lower limit. */
    size_t max_input_len; /* compression-max-value-size. 0 means no upper limit. */
} blockCompressorConfig;

/* The compression function table. One per backend, constant and shared.
 *
 * Every entry here is a function that calls into the backend library, and every
 * entry is required.
 *
 * Sizes are size_t. compress() and decompress() return 0 on failure, which is
 * unambiguous because the caller never passes an empty source: the smallest value
 * the sweeper accepts is compression-min-value-size, 256 bytes by default. After
 * a 0 return, read instance->last_error and pass it to blockCompressorStrerror(). */
typedef struct blockCompressorApi {
    /* Scratch lifecycle, called by blockCompressorCreate() and blockCompressorFree(). The
     * backend keeps its scratch in instance->state. state_new() returns C_OK or
     * C_ERR. The scratch is private to the instance, so private to one thread. */
    int (*state_new)(blockCompressor *instance);
    void (*state_free)(blockCompressor *instance);

    /* Largest output compress() can produce for input_len input bytes, judged
     * only against the library's own limits. Returns 0 when the library cannot
     * handle that length.
     *
     * Callers use instance->max_output_size() instead, which also applies the
     * operator's range. */
    size_t (*lib_max_output_size)(size_t input_len);

    /* Compresses srclen bytes from src into dst.
     *
     * dst must hold at least instance->max_output_size() bytes. Returns the number of
     * bytes written, or 0 on failure. The caller then shrinks the buffer to that
     * length, because the bound is generous. */
    size_t (*compress)(blockCompressor *instance,
                       const void *src,
                       size_t srclen,
                       void *dst,
                       size_t dstcap);

    /* Decompresses body_len bytes from body into dst.
     *
     * dstcap comes from the frame's uncompressed_len field, so it is exact.
     *
     * Returns the number of bytes written, or 0 on failure. Frames loaded from
     * RDB come from outside the process, so a failure can mean bad input. The
     * caller must handle it and must not assert. */
    size_t (*decompress)(blockCompressor *instance,
                         const void *body,
                         size_t body_len,
                         void *dst,
                         size_t dstcap);
} blockCompressorApi;

/* What callers hold. One per thread, never shared. Immutable except for state,
 * which only the backend touches, and last_error. */
struct blockCompressor {
    /* The backend's functions, fixed at construction. */
    const blockCompressorApi *api;

    /* Plain data about the backend, copied in at construction. */
    const char *name;   /* "lz4" or "zstd", for logs and INFO */
    compressionAlgo id; /* which backend this is */
    blockCompressorConfig config;

    /* Backend scratch, private to this instance. Only the backend touches it. */
    void *state;

    /* Code from the last failed call on this instance, else BLOCK_COMPRESSOR_ERR_NONE. */
    int last_error;

    /* Largest output compress() can produce for input_len input bytes. The caller
     * uses this to size the destination before compressing. The real output is
     * almost always much smaller, so the caller must shrink the buffer afterwards.
     *
     * Returns 0 when the value must not be compressed at all. That happens when
     * input_len falls outside instance->config, or when the backend itself cannot
     * handle that length. The caller treats both the same way, so it does not need
     * to know which one it was. */
    size_t (*max_output_size)(const blockCompressor *instance, size_t input_len);
};

/* Builds a blockCompressor. Returns NULL for ALGO_NONE, for an unknown id,
 * for a backend this build does not include, and on allocation failure.
 * ALGO_ZSTD returns NULL until zstd is vendored.
 *
 * config is copied, so the caller may free or change it right after. Passing NULL
 * means no size limits.
 *
 * Each thread that compresses or decompresses calls this once and keeps the
 * result. To change a setting, build a new instance and free the old one. */
blockCompressor *blockCompressorCreate(compressionAlgo id, const blockCompressorConfig *config);
void blockCompressorFree(blockCompressor *c);

/* Maps a failure code to static text. Never returns NULL. */
const char *blockCompressorStrerror(int err);

#endif /* COMPRESSION_BLOCK_INTERFACE_H */
