/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_BLOCK_INTERFACE_H
#define COMPRESSION_BLOCK_INTERFACE_H

#include "compression.h"

#include <stddef.h>
#include <stdint.h>

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
 * main thread has its own for decompressing on reads. Compression dictionaries
 * are the exception and are shared on purpose, because they are read only once
 * loaded.
 *
 * The backend for new compression is picked once at startup from
 * compression-mode and never changes while the process runs. Frames saved to
 * RDB can outlive the process, so each frame still records its algorithm (see
 * design-docs/inline-compression.md, section 6.4). */

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

/* Largest dictionary each backend can use, in bytes. 0 means no limit.
 *
 * LZ4 references only the last 64 KiB of a dictionary. See LZ4_loadDict() in
 * deps/lz4/lz4.h: "only the last 64 KB are loaded". */
#define BLOCK_COMPRESSOR_LZ4_DICT_MAX 65536
#define BLOCK_COMPRESSOR_ZSTD_DICT_MAX 0

/* Smallest dictionary LZ4 can use, in bytes.
 *
 * LZ4 drops a dictionary shorter than one hash unit, because it cannot hash it:
 * see "if (dictSize < (int)HASH_UNIT) return 0;" in LZ4_loadDict_internal() in
 * deps/lz4/lz4.c. HASH_UNIT is sizeof(reg_t), so 8 bytes on a 64-bit build and 4
 * on a 32-bit one. We use 8 on every build, so the limit does not change with the
 * word size. A dictionary that small saves nothing anyway. */
#define BLOCK_COMPRESSOR_LZ4_DICT_MIN 8

/* What the operator asked for. Copied into an instance and then frozen. */
typedef struct blockCompressorConfig {
    size_t min_input_len; /* compression-min-value-size. 0 means no lower limit. */
    size_t max_input_len; /* compression-max-value-size. 0 means no upper limit. */
} blockCompressorConfig;

/* A loaded dictionary. id is the value a frame writes to its dict_id field and
 * is never 0. cdict is the digested form from blockCompressorApi.dict_load() for
 * algorithm_id. Keeping the three together means a caller can not write one
 * dictionary's id with another dictionary's bytes.
 *
 * The dictionary registry (a later phase) owns these and keeps them alive while
 * frames use them. */
typedef struct blockCompressorDict {
    uint32_t id;
    uint8_t algorithm_id;
    void *cdict;
} blockCompressorDict;

/* Returns 1 when dict holds a usable dictionary: a non-zero id and loaded
 * data. It does not check that the dictionary fits a given frame or backend. */
static inline int blockCompressorDictIsValid(const blockCompressorDict *dict) {
    return dict != NULL && dict->id != 0 && dict->cdict != NULL;
}

/* The compression function table. One per backend, constant and shared.
 *
 * Every entry here is a function that calls into the backend library. Every entry
 * is required unless the comment says it may be NULL.
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

    /* Trains a dictionary from n samples.
     *
     * samples is one buffer holding every sample back to back, with no
     * separators. sizes[i] is the length of sample i, so the samples are read in
     * order using those lengths.
     *
     * Writes the dictionary into dict_out, which holds dict_cap bytes. Returns
     * the number of bytes written, or -1 on failure.
     *
     * May be NULL. LZ4 has no trainer of its own and its headers say to use
     * Zstandard's dictionary builder instead. A backend without a trainer leaves
     * this NULL, and the dictionary layer supplies the bytes another way. */
    int (*train)(blockCompressor *instance,
                 const void *samples,
                 const size_t *sizes,
                 unsigned n,
                 void *dict_out,
                 size_t dict_cap);

    /* Turns raw dictionary bytes into the digested form the library wants.
     *
     * The result is read only and safe to use from several threads at the same
     * time, which is why it takes no instance. It does not borrow dict_buf: the
     * caller may free dict_buf as soon as this returns.
     *
     * Returns NULL on failure. A backend may also refuse a length it cannot use:
     * LZ4 refuses anything under BLOCK_COMPRESSOR_LZ4_DICT_MIN bytes, and keeps only
     * the last BLOCK_COMPRESSOR_LZ4_DICT_MAX bytes of a longer one. */
    void *(*dict_load)(const void *dict_buf, size_t len);
    void (*dict_free)(void *cdict);

    /* Largest output compress() can produce for input_len input bytes, judged
     * only against the library's own limits. Returns 0 when the library cannot
     * handle that length.
     *
     * Callers use instance->max_output_size() instead, which also applies the
     * operator's range. */
    size_t (*lib_max_output_size)(size_t input_len);

    /* Compresses srclen bytes from src into dst, using cdict.
     *
     * dst must hold at least instance->max_output_size() bytes. Returns the number of
     * bytes written, or 0 on failure. The caller then shrinks the buffer to that
     * length, because the bound is generous. */
    size_t (*compress)(blockCompressor *instance,
                       void *cdict,
                       const void *src,
                       size_t srclen,
                       void *dst,
                       size_t dstcap);

    /* Decompresses body_len bytes from body into dst, using cdict.
     *
     * cdict must be the same dictionary version the frame was built with. The
     * frame's dict_id field is what selects it. dstcap comes from the frame's
     * uncompressed_len field, so it is exact.
     *
     * Returns the number of bytes written, or 0 on failure. Frames loaded from
     * RDB come from outside the process, so a failure can mean bad input. The
     * caller must handle it and must not assert. */
    size_t (*decompress)(blockCompressor *instance,
                         void *cdict,
                         const void *body,
                         size_t body_len,
                         void *dst,
                         size_t dstcap);

    /* Called when a frame built with cdict is freed, so the backend can drop any
     * library state it keeps for that frame. The dictionary registry does its own
     * user counting, which is not this hook's job. May be NULL when a backend
     * keeps no per-frame state, which is the case for LZ4. */
    void (*release)(blockCompressor *instance, void *cdict);
} blockCompressorApi;

/* What callers hold. One per thread, never shared. Immutable except for state,
 * which only the backend touches, and last_error. */
struct blockCompressor {
    /* The backend's functions, fixed at construction. */
    const blockCompressorApi *api;

    /* Plain data about the backend, copied in at construction. */
    const char *name;     /* "lz4" or "zstd", for logs and INFO */
    compressionAlgo id;   /* which backend this is */
    size_t dict_max_size; /* largest dictionary the backend can use, 0 = no limit */
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
