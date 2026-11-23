/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_zstd.h"
#include "server.h"
#include "serverassert.h"
#include "zmalloc.h"
#include <limits.h>

bool compressionZstdIsSupported(void) {
#ifdef HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

#ifdef HAVE_ZSTD

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#if ZSTD_VERSION_NUMBER < 10407
#error "Zstandard 1.4.7 or newer is required"
#endif

#define COMPRESSION_ZSTD_FRAME_OVERHEAD (ZSTD_FRAMEHEADERSIZE_MAX + 4)

/* Zstd allocates its streaming tables lazily. Keep the owning stream's
 * counter equal to the usable bytes held by the codec context so client
 * memory accounting includes those allocations. */
static void *zstdZmalloc(void *opaque, size_t size) {
    void *ptr = zmalloc(size);
    *(size_t *)opaque += zmalloc_size(ptr);
    return ptr;
}

static void zstdZfree(void *opaque, void *address) {
    if (address == NULL) return;
    *(size_t *)opaque -= zmalloc_size(address);
    zfree(address);
}

/* The counter's address is captured by the context for its whole lifetime, so
 * the owning stream struct must not move between init and free. */
static ZSTD_customMem zstdCustomMem(size_t *ctx_memory) {
    ZSTD_customMem mem = {
        .customAlloc = zstdZmalloc,
        .customFree = zstdZfree,
        .opaque = ctx_memory,
    };
    return mem;
}

int compressionZstdCompressorInit(streamCompressor *sc) {
    sc->ctx = ZSTD_createCCtx_advanced(zstdCustomMem(&sc->ctx_memory));
    return sc->ctx != NULL ? C_OK : C_ERR;
}

void compressionZstdCompressorFree(streamCompressor *sc) {
    if (sc->ctx) {
        ZSTD_freeCCtx((ZSTD_CCtx *)sc->ctx);
        sc->ctx = NULL;
    }
}

int compressionZstdDecompressorInit(streamDecompressor *sd) {
    ZSTD_DCtx *dctx = ZSTD_createDCtx_advanced(zstdCustomMem(&sd->ctx_memory));
    if (!dctx) return C_ERR;

    if (sd->skip_codec_checksum_validation) {
        size_t ret = ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, ZSTD_d_ignoreChecksum);
        /* This advanced parameter is best effort. If the linked library
         * rejects it, keep normal checksum validation instead of failing an
         * otherwise valid stream. */
        if (ZSTD_isError(ret)) sd->skip_codec_checksum_validation = false;
    }

    sd->ctx = dctx;
    sd->input_hint = ZSTD_FRAMEHEADERSIZE_MIN(ZSTD_f_zstd1);
    return C_OK;
}

void compressionZstdDecompressorFree(streamDecompressor *sd) {
    if (sd->ctx) {
        ZSTD_freeDCtx((ZSTD_DCtx *)sd->ctx);
        sd->ctx = NULL;
    }
}

size_t compressionZstdOutputBound(size_t input_len) {
    size_t data_bound = ZSTD_compressBound(input_len);
    size_t stream_bound = ZSTD_CStreamOutSize();
    if (ZSTD_isError(data_bound) || data_bound > SIZE_MAX - stream_bound) return 0;
    size_t bound = data_bound + stream_bound;
    if (bound > SIZE_MAX - COMPRESSION_ZSTD_FRAME_OVERHEAD) return 0;
    return bound + COMPRESSION_ZSTD_FRAME_OVERHEAD;
}

ssize_t compressionZstdCompressFeed(streamCompressor *sc,
                                    uint8_t *output,
                                    size_t output_capacity,
                                    const uint8_t *input,
                                    size_t input_len,
                                    compressFlushMode flush_mode) {
    assert(sc->ctx != NULL);
    size_t bound = compressionZstdOutputBound(input_len);
    if (bound == 0 || output_capacity < bound) return -1;

    ZSTD_CCtx *cctx = (ZSTD_CCtx *)sc->ctx;
    ZSTD_EndDirective directive;
    switch (flush_mode) {
    case COMPRESS_FLUSH_CONTINUE: directive = ZSTD_e_continue; break;
    case COMPRESS_FLUSH_SYNC: directive = ZSTD_e_flush; break;
    case COMPRESS_FLUSH_END: directive = ZSTD_e_end; break;
    default: panic("Invalid compression flush mode: %d", flush_mode);
    }

    if (!sc->stream_started) {
        size_t ret = ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
        if (ZSTD_isError(ret)) return -1;
        ret = ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, sc->level);
        if (ZSTD_isError(ret)) return -1;
        ret = ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag,
                                     sc->checksum_flags & STREAM_CHECKSUM_CONTENT ? 1 : 0);
        if (ZSTD_isError(ret)) return -1;
        sc->stream_started = true;
    }

    uint8_t empty_sentinel = 0;
    ZSTD_inBuffer in_buf = {
        .src = input ? input : &empty_sentinel,
        .size = input_len,
        .pos = 0,
    };
    ZSTD_outBuffer out_buf = {
        .dst = output,
        .size = output_capacity,
        .pos = 0,
    };

    size_t ret = 0;
    do {
        ret = ZSTD_compressStream2(cctx, &out_buf, &in_buf, directive);
        if (ZSTD_isError(ret)) return -1;
        if (out_buf.pos == output_capacity && (in_buf.pos < in_buf.size || ret != 0)) return -1;
    } while (in_buf.pos < in_buf.size || (directive != ZSTD_e_continue && ret != 0));

    if (out_buf.pos > (size_t)SSIZE_MAX) return -1;
    if (flush_mode == COMPRESS_FLUSH_END) sc->stream_started = false;
    return (ssize_t)out_buf.pos;
}

ssize_t compressionZstdDecompressFeed(streamDecompressor *sd,
                                      uint8_t *output,
                                      size_t output_capacity,
                                      const uint8_t *input,
                                      size_t input_len,
                                      size_t *input_consumed) {
    assert(sd->ctx != NULL);
    *input_consumed = 0;
    if (sd->frame_done) return 0;

    ZSTD_DCtx *dctx = (ZSTD_DCtx *)sd->ctx;
    uint8_t empty_sentinel = 0;
    ZSTD_inBuffer in_buf = {
        .src = input ? input : &empty_sentinel,
        .size = input_len,
        .pos = 0,
    };
    ZSTD_outBuffer out_buf = {
        .dst = output,
        .size = output_capacity,
        .pos = 0,
    };

    size_t ret = ZSTD_decompressStream(dctx, &out_buf, &in_buf);
    if (ZSTD_isError(ret)) return -1;

    *input_consumed = in_buf.pos;
    sd->input_hint = ret;
    if (ret == 0) sd->frame_done = true;
    if (out_buf.pos > (size_t)SSIZE_MAX) return -1;
    return (ssize_t)out_buf.pos;
}

int compressionZstdDecompressorReset(streamDecompressor *sd) {
    assert(sd->ctx != NULL);
    size_t ret = ZSTD_DCtx_reset((ZSTD_DCtx *)sd->ctx, ZSTD_reset_session_only);
    if (ZSTD_isError(ret)) return C_ERR;
    sd->frame_done = false;
    sd->input_hint = ZSTD_FRAMEHEADERSIZE_MIN(ZSTD_f_zstd1);
    return C_OK;
}

#else

int compressionZstdCompressorInit(streamCompressor *sc) {
    UNUSED(sc);
    return C_ERR;
}

void compressionZstdCompressorFree(streamCompressor *sc) {
    UNUSED(sc);
}

int compressionZstdDecompressorInit(streamDecompressor *sd) {
    UNUSED(sd);
    return C_ERR;
}

void compressionZstdDecompressorFree(streamDecompressor *sd) {
    UNUSED(sd);
}

size_t compressionZstdOutputBound(size_t input_len) {
    UNUSED(input_len);
    serverPanic("Zstandard support is not compiled in");
}

ssize_t compressionZstdCompressFeed(streamCompressor *sc,
                                    uint8_t *output,
                                    size_t output_capacity,
                                    const uint8_t *input,
                                    size_t input_len,
                                    compressFlushMode flush_mode) {
    UNUSED(sc);
    UNUSED(output);
    UNUSED(output_capacity);
    UNUSED(input);
    UNUSED(input_len);
    UNUSED(flush_mode);
    serverPanic("Zstandard support is not compiled in");
}

ssize_t compressionZstdDecompressFeed(streamDecompressor *sd,
                                      uint8_t *output,
                                      size_t output_capacity,
                                      const uint8_t *input,
                                      size_t input_len,
                                      size_t *input_consumed) {
    UNUSED(sd);
    UNUSED(output);
    UNUSED(output_capacity);
    UNUSED(input);
    UNUSED(input_len);
    UNUSED(input_consumed);
    serverPanic("Zstandard support is not compiled in");
}

int compressionZstdDecompressorReset(streamDecompressor *sd) {
    UNUSED(sd);
    return C_ERR;
}

#endif
