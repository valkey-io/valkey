/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The LZ4 backend for inline in-memory value compression. See
 * compression_block_interface.h for the interface.
 *
 * Two choices in this file are worth explaining.
 *
 * 1. We use the LZ4 block API, not lz4frame.
 *    compression_lz4.c uses lz4frame, because the RDB and replication byte stream
 *    needs framing. We do not. The header that the caller stores with each value
 *    already holds the uncompressed length, and LZ4F_HEADER_SIZE_MAX is 19
 *    bytes. That is a lot next to a 256-byte value, and it would buy nothing.
 *
 * 2. Every allocation goes through zmalloc, never LZ4's own malloc.
 *    This is a memory-saving feature, so all of its memory must show up in
 *    used_memory. compression_lz4.c does the same for lz4frame.
 *
 * Threads: an instance belongs to one thread, so its working stream needs no lock
 * and no atomics. */

#include "fmacros.h" /* must come before any system header */
#include "compression_block_interface.h"
#include "server.h" /* C_OK, C_ERR */
#include "serverassert.h"
#include "zmalloc.h"
#include <limits.h>

#include <lz4.h>

/* Acceleration factor for LZ4_compress_fast_continue(). 1 gives the same speed
 * and ratio as LZ4_compress_default(). Higher values trade ratio for speed, which
 * is the wrong trade here: this feature exists to save memory. */
#define BLOCK_COMPRESSOR_LZ4_ACCELERATION 1

/* The scratch an instance keeps in instance->state. Private to one thread.
 *
 * sizeof(LZ4_stream_t) is 16416 bytes, so this is built once per instance and
 * reused for every value. Building one per value would mean zeroing 16 KiB to
 * compress a few hundred bytes. */
typedef struct lz4State {
    void *cstream_buf;     /* zmalloc'd storage that cstream lives in */
    LZ4_stream_t *cstream; /* working stream, reset before every compress */
} lz4State;

/* Allocates one LZ4_stream_t through zmalloc and initializes it. Returns NULL on
 * failure, and sets *buf_out either way, so the caller can always free it. */
static LZ4_stream_t *lz4StreamNew(void **buf_out) {
    void *buf = zmalloc(sizeof(LZ4_stream_t));
    LZ4_stream_t *stream = LZ4_initStream(buf, sizeof(LZ4_stream_t));
    if (stream == NULL) {
        zfree(buf);
        *buf_out = NULL;
        return NULL;
    }
    *buf_out = buf;
    return stream;
}

static int lz4StateNew(blockCompressor *instance) {
    lz4State *st = zcalloc(sizeof(*st));
    st->cstream = lz4StreamNew(&st->cstream_buf);
    if (st->cstream == NULL) {
        zfree(st);
        return C_ERR;
    }
    instance->state = st;
    return C_OK;
}

static void lz4StateFree(blockCompressor *instance) {
    lz4State *st = instance->state;
    if (st == NULL) return;
    zfree(st->cstream_buf);
    zfree(st);
    instance->state = NULL;
}

static size_t lz4LibMaxOutputSize(size_t input_len) {
    if (input_len == 0 || input_len > (size_t)LZ4_MAX_INPUT_SIZE) return 0;
    int max_size = LZ4_compressBound((int)input_len);
    if (max_size <= 0) return 0;
    return (size_t)max_size;
}

static size_t lz4Compress(blockCompressor *instance,
                          const void *src,
                          size_t srclen,
                          void *dst,
                          size_t dstcap) {
    assert(instance != NULL && src != NULL && dst != NULL);
    lz4State *st = instance->state;

    instance->last_error = BLOCK_COMPRESSOR_ERR_NONE;

    if (srclen == 0 || srclen > (size_t)LZ4_MAX_INPUT_SIZE) {
        instance->last_error = BLOCK_COMPRESSOR_ERR_BAD_SIZE;
        return 0;
    }
    if (dstcap > (size_t)INT_MAX) dstcap = (size_t)INT_MAX;

    /* Reset before every call, so each value is compressed on its own and no
     * bytes from the previous value are used as history. */
    LZ4_resetStream_fast(st->cstream);

    int written = LZ4_compress_fast_continue(st->cstream,
                                             src,
                                             dst,
                                             (int)srclen,
                                             (int)dstcap,
                                             BLOCK_COMPRESSOR_LZ4_ACCELERATION);
    if (written <= 0) {
        /* Note 5 on LZ4_compress_fast_continue(): after an error the stream is
         * invalid and may only be reset or freed. Re-initialize it, because
         * LZ4_resetStream_fast() is documented as unsafe on garbage state. */
        st->cstream = LZ4_initStream(st->cstream_buf, sizeof(LZ4_stream_t));
        assert(st->cstream != NULL);
        instance->last_error = BLOCK_COMPRESSOR_ERR_COMPRESS;
        return 0;
    }
    return (size_t)written;
}

static size_t lz4Decompress(blockCompressor *instance,
                            const void *body,
                            size_t body_len,
                            void *dst,
                            size_t dstcap) {
    assert(instance != NULL && body != NULL && dst != NULL);

    instance->last_error = BLOCK_COMPRESSOR_ERR_NONE;

    if (body_len == 0 || body_len > (size_t)INT_MAX || dstcap == 0 || dstcap > (size_t)INT_MAX) {
        instance->last_error = BLOCK_COMPRESSOR_ERR_BAD_SIZE;
        return 0;
    }

    /* LZ4 decompresses without any scratch, so instance->state is unused here. zstd
     * needs a ZSTD_DCtx, which is why the interface passes the instance. */
    int written = LZ4_decompress_safe(body, dst, (int)body_len, (int)dstcap);
    if (written <= 0) {
        instance->last_error = BLOCK_COMPRESSOR_ERR_DECOMPRESS;
        return 0;
    }
    return (size_t)written;
}

const blockCompressorApi blockCompressorApiLz4 = {
    .state_new = lz4StateNew,
    .state_free = lz4StateFree,
    .lib_max_output_size = lz4LibMaxOutputSize,
    .compress = lz4Compress,
    .decompress = lz4Decompress,
};
