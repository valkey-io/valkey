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

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#if ZSTD_VERSION_NUMBER < 10407
#error "Zstandard 1.4.7 or newer is required"
#endif

#define ZSTD_FRAME_OVERHEAD_MAX 22 /* 18-byte frame header + 4-byte content checksum. */

static void *zstdZmalloc(void *opaque, size_t size) {
    (void)opaque;
    return zmalloc(size);
}

static void zstdZfree(void *opaque, void *address) {
    (void)opaque;
    zfree(address);
}

static const ZSTD_customMem zstd_mem = {
    .customAlloc = zstdZmalloc,
    .customFree = zstdZfree,
    .opaque = NULL,
};

int compressionZstdCompressorInit(streamCompressor *sc) {
    ZSTD_CCtx *cctx = NULL;
    if (!sc) return C_ERR;

    cctx = ZSTD_createCCtx_advanced(zstd_mem);
    if (!cctx) return C_ERR;

    sc->ctx = cctx;
    return C_OK;
}

void compressionZstdCompressorFree(streamCompressor *sc) {
    if (!sc || !sc->ctx) return;
    ZSTD_freeCCtx((ZSTD_CCtx *)sc->ctx);
    sc->ctx = NULL;
}

int compressionZstdDecompressorInit(streamDecompressor *sd) {
    ZSTD_DCtx *dctx = NULL;
    if (!sd) return C_ERR;

    dctx = ZSTD_createDCtx_advanced(zstd_mem);
    if (!dctx) return C_ERR;

    if (sd->skip_codec_checksum_validation) {
        size_t ret = ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, ZSTD_d_ignoreChecksum);
        if (ZSTD_isError(ret)) {
            ZSTD_freeDCtx(dctx);
            return C_ERR;
        }
    }

    sd->ctx = dctx;
    sd->input_hint = ZSTD_DStreamInSize();
    return C_OK;
}

void compressionZstdDecompressorFree(streamDecompressor *sd) {
    if (!sd || !sd->ctx) return;
    ZSTD_freeDCtx((ZSTD_DCtx *)sd->ctx);
    sd->ctx = NULL;
}

size_t compressionZstdOutputBound(size_t input_len) {
    size_t data_bound = ZSTD_compressBound(input_len);
    size_t stream_bound = ZSTD_CStreamOutSize();
    if (ZSTD_isError(data_bound) || data_bound > SIZE_MAX - stream_bound) return 0;
    size_t bound = data_bound + stream_bound;
    if (bound > SIZE_MAX - ZSTD_FRAME_OVERHEAD_MAX) return 0;
    return bound + ZSTD_FRAME_OVERHEAD_MAX;
}

ssize_t compressionZstdCompressFeed(streamCompressor *sc,
                                    uint8_t *output,
                                    size_t output_capacity,
                                    const uint8_t *input,
                                    size_t input_len,
                                    compressFlushMode flush_mode) {
    if (!sc || !sc->ctx) return -1;
    size_t bound = compressionZstdOutputBound(input_len);
    if (bound == 0 || output_capacity < bound) {
        return -1;
    }

    ZSTD_CCtx *cctx = (ZSTD_CCtx *)sc->ctx;
    ZSTD_EndDirective directive;
    switch (flush_mode) {
    case COMPRESS_FLUSH_CONTINUE: directive = ZSTD_e_continue; break;
    case COMPRESS_FLUSH_END: directive = ZSTD_e_end; break;
    default: assert(0 && "invalid compressFlushMode"); return -1;
    }

    if (!sc->stream_started) {
        size_t ret = ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
        if (ZSTD_isError(ret)) goto zstd_error;
        ret = ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, sc->level);
        if (ZSTD_isError(ret)) goto zstd_error;
        ret = ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, sc->codec_checksum ? 1 : 0);
        if (ZSTD_isError(ret)) goto zstd_error;
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
        if (ZSTD_isError(ret)) goto zstd_error;
        if (out_buf.pos == output_capacity && (in_buf.pos < in_buf.size || ret != 0)) goto zstd_error;
    } while (in_buf.pos < in_buf.size || (directive != ZSTD_e_continue && ret != 0));

    if (out_buf.pos > (size_t)SSIZE_MAX) goto zstd_error;
    if (flush_mode == COMPRESS_FLUSH_END) sc->stream_started = false;
    return (ssize_t)out_buf.pos;

zstd_error:
    return -1;
}

ssize_t compressionZstdDecompressFeed(streamDecompressor *sd,
                                      uint8_t *output,
                                      size_t output_capacity,
                                      const uint8_t *input,
                                      size_t input_len,
                                      size_t *input_consumed) {
    if (!sd || !sd->ctx || !input_consumed) return -1;

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
