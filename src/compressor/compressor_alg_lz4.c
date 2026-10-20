/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The LZ4 backend for inline in-memory value compression. See compressor_alg.h
 * for the interface and design-docs/inline-compression.md section 7 for why the
 * interface has this shape.
 *
 * Three choices in this file are worth explaining.
 *
 * 1. We use the LZ4 block API, not lz4frame.
 *    compression_lz4.c uses lz4frame, because the RDB and replication byte stream
 *    needs framing. We do not. Our own compressionFrame header already holds the
 *    uncompressed length and the dictionary id, and LZ4F_HEADER_SIZE_MAX is 19
 *    bytes. That is a lot next to a 256-byte value, and it would buy nothing.
 *
 * 2. The dictionary is digested once, in dict_load.
 *    LZ4_loadDictSlow builds the match tables, which is the costly part. We pay
 *    it once per dictionary version. Each compress call then calls
 *    LZ4_attach_dictionary, which only points the working stream at the loaded
 *    one. No copy and no table rebuild per value.
 *
 * 3. Every allocation goes through zmalloc, never LZ4's own malloc.
 *    This is a memory-saving feature, so all of its memory must show up in
 *    used_memory. compression_lz4.c does the same for lz4frame.
 *
 * Threads: an instance belongs to one thread, so its working stream needs no lock
 * and no atomics. A cdict is read only once dict_load returns, so any number of
 * instances on any number of threads may share one. LZ4_attach_dictionary only
 * writes to the working stream in the instance and only reads the dictionary. */

#include "compressor/compressor_alg.h"
#include "server.h" /* C_OK, C_ERR */
#include "serverassert.h"
#include "zmalloc.h"
#include <limits.h>
#include <string.h>

#include <lz4.h>

/* Acceleration factor for LZ4_compress_fast_continue(). 1 gives the same speed
 * and ratio as LZ4_compress_default(). Higher values trade ratio for speed, which
 * is the wrong trade here: this feature exists to save memory. */
#define COMPRESSOR_LZ4_ACCELERATION 1

/* The scratch an instance keeps in instance->state. Private to one thread.
 *
 * sizeof(LZ4_stream_t) is 16416 bytes, so this is built once per instance and
 * reused for every value. Building one per value would mean zeroing 16 KiB to
 * compress a few hundred bytes. */
typedef struct lz4State {
    void *cstream_buf;     /* zmalloc'd storage that cstream lives in */
    LZ4_stream_t *cstream; /* working stream, reset before every compress */
} lz4State;

/* One digested dictionary version. Read only once dict_load returns, so it is
 * shared by every instance on every thread. */
typedef struct lz4Dict {
    char *dict;            /* our own copy, at most COMPRESSOR_LZ4_DICT_MAX bytes */
    int dict_len;          /* length of dict */
    void *dstream_buf;     /* zmalloc'd storage that dstream lives in */
    LZ4_stream_t *dstream; /* dict digested by LZ4_loadDictSlow */
} lz4Dict;

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

static int lz4StateNew(compressorAlg *instance) {
    lz4State *st = zcalloc(sizeof(*st));
    st->cstream = lz4StreamNew(&st->cstream_buf);
    if (st->cstream == NULL) {
        zfree(st);
        return C_ERR;
    }
    instance->state = st;
    return C_OK;
}

static void lz4StateFree(compressorAlg *instance) {
    lz4State *st = instance->state;
    if (st == NULL) return;
    zfree(st->cstream_buf);
    zfree(st);
    instance->state = NULL;
}

static void *lz4DictLoad(const void *dict_buf, size_t len) {
    if (dict_buf == NULL || len < COMPRESSOR_LZ4_DICT_MIN) return NULL;

    /* Keep only the tail LZ4 can actually use. Compression and decompression must
     * see the same bytes, and both read this one copy, so they cannot disagree. */
    if (len > COMPRESSOR_LZ4_DICT_MAX) {
        dict_buf = (const char *)dict_buf + (len - COMPRESSOR_LZ4_DICT_MAX);
        len = COMPRESSOR_LZ4_DICT_MAX;
    }

    lz4Dict *d = zcalloc(sizeof(*d));
    d->dict = zmalloc(len);
    memcpy(d->dict, dict_buf, len);
    d->dict_len = (int)len;

    d->dstream = lz4StreamNew(&d->dstream_buf);
    if (d->dstream == NULL) goto fail;

    /* LZ4_loadDictSlow costs more CPU than LZ4_loadDict but gives a slightly
     * better ratio. We pay it once and reuse the result for every value, so it is
     * the right side of that trade. Only LZ4_loadDict and LZ4_loadDictSlow
     * produce a stream that LZ4_attach_dictionary accepts. */
    if (LZ4_loadDictSlow(d->dstream, d->dict, d->dict_len) != d->dict_len) goto fail;

    return d;

fail:
    zfree(d->dstream_buf);
    zfree(d->dict);
    zfree(d);
    return NULL;
}

static void lz4DictFree(void *cdict) {
    lz4Dict *d = cdict;
    if (d == NULL) return;
    zfree(d->dstream_buf);
    zfree(d->dict);
    zfree(d);
}

static size_t lz4LibMaxOutputSize(size_t input_len) {
    if (input_len == 0 || input_len > (size_t)LZ4_MAX_INPUT_SIZE) return 0;
    int max_size = LZ4_compressBound((int)input_len);
    if (max_size <= 0) return 0;
    return (size_t)max_size;
}

static size_t lz4Compress(compressorAlg *instance,
                          void *cdict,
                          const void *src,
                          size_t srclen,
                          void *dst,
                          size_t dstcap) {
    assert(instance != NULL && src != NULL && dst != NULL);
    lz4State *st = instance->state;
    lz4Dict *d = cdict;

    instance->last_error = COMPRESSOR_ERR_NONE;

    if (srclen == 0 || srclen > (size_t)LZ4_MAX_INPUT_SIZE) {
        instance->last_error = COMPRESSOR_ERR_BAD_SIZE;
        return 0;
    }
    if (dstcap > (size_t)INT_MAX) dstcap = (size_t)INT_MAX;

    /* Reset, then attach. LZ4_attach_dictionary drops the dictionary again at the
     * end of the next compress call, so both steps belong to every call. Passing
     * NULL unsets any dictionary, which is the no-dictionary path. */
    LZ4_resetStream_fast(st->cstream);
    LZ4_attach_dictionary(st->cstream, d != NULL ? d->dstream : NULL);

    int written = LZ4_compress_fast_continue(st->cstream,
                                             src,
                                             dst,
                                             (int)srclen,
                                             (int)dstcap,
                                             COMPRESSOR_LZ4_ACCELERATION);
    if (written <= 0) {
        /* Note 5 on LZ4_compress_fast_continue(): after an error the stream is
         * invalid and may only be reset or freed. Re-initialize it, because
         * LZ4_resetStream_fast() is documented as unsafe on garbage state. */
        st->cstream = LZ4_initStream(st->cstream_buf, sizeof(LZ4_stream_t));
        assert(st->cstream != NULL);
        instance->last_error = COMPRESSOR_ERR_COMPRESS;
        return 0;
    }
    return (size_t)written;
}

static size_t lz4Decompress(compressorAlg *instance,
                            void *cdict,
                            const void *body,
                            size_t body_len,
                            void *dst,
                            size_t dstcap) {
    assert(instance != NULL && body != NULL && dst != NULL);
    lz4Dict *d = cdict;

    instance->last_error = COMPRESSOR_ERR_NONE;

    if (body_len == 0 || body_len > (size_t)INT_MAX || dstcap == 0 || dstcap > (size_t)INT_MAX) {
        instance->last_error = COMPRESSOR_ERR_BAD_SIZE;
        return 0;
    }

    /* LZ4 decompresses without any scratch, so instance->state is unused here. zstd
     * needs a ZSTD_DCtx, which is why the interface passes the instance. */
    int written;
    if (d != NULL) {
        written = LZ4_decompress_safe_usingDict(body, dst, (int)body_len, (int)dstcap, d->dict, d->dict_len);
    } else {
        written = LZ4_decompress_safe(body, dst, (int)body_len, (int)dstcap);
    }
    if (written <= 0) {
        instance->last_error = COMPRESSOR_ERR_DECOMPRESS;
        return 0;
    }
    return (size_t)written;
}

const compressorApi compressorApiLz4 = {
    .state_new = lz4StateNew,
    .state_free = lz4StateFree,
    /* No trainer. LZ4 has none of its own and its headers point at Zstandard's
     * dictionary builder instead. The dictionary layer supplies the bytes. */
    .train = NULL,
    .dict_load = lz4DictLoad,
    .dict_free = lz4DictFree,
    .lib_max_output_size = lz4LibMaxOutputSize,
    .compress = lz4Compress,
    .decompress = lz4Decompress,
    /* No per-frame library state to drop. */
    .release = NULL,
};
