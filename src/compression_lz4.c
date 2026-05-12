/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_lz4.h"
#include <limits.h>
#include <lz4frame.h>

/* Shared LZ4F preferences template.
 * - Used by streamCompressOutputBound() for bounds.
 * - Copied and selectively overridden in streamCompressFeed() before
 *   LZ4F_compressBegin() (compression level and checksum mode).
 *
 * Bounds are computed with block-independent mode and block checksums enabled
 * so the returned capacity is safe for both checksum settings. */
static const LZ4F_preferences_t lz4f_prefs = {
    .frameInfo = {
        .blockChecksumFlag = LZ4F_blockChecksumEnabled,
        .contentChecksumFlag = LZ4F_noContentChecksum,
        .blockSizeID = LZ4F_max64KB,
        .blockMode = LZ4F_blockIndependent,
    },
    .compressionLevel = 0, /* bound calculation uses 0 (worst-case); actual
                            * compression uses sc->level via a local copy */
};

int compressionLz4CompressorInit(streamCompressor *sc) {
    LZ4F_cctx *cctx = NULL;
    LZ4F_errorCode_t err;

    if (!sc) return -1;

    err = LZ4F_createCompressionContext(&cctx, LZ4F_VERSION);
    if (LZ4F_isError(err)) return -1;

    sc->ctx = cctx;
    return 0;
}

void compressionLz4CompressorDestroy(streamCompressor *sc) {
    if (!sc || !sc->ctx) return;

    LZ4F_freeCompressionContext((LZ4F_cctx *)sc->ctx);
    sc->ctx = NULL;
}

int compressionLz4DecompressorInit(streamDecompressor *sd) {
    LZ4F_dctx *dctx = NULL;
    LZ4F_errorCode_t err;

    if (!sd) return -1;

    err = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(err)) return -1;

    sd->ctx = dctx;
    sd->input_hint = LZ4F_HEADER_SIZE_MIN;
    return 0;
}

void compressionLz4DecompressorDestroy(streamDecompressor *sd) {
    if (!sd || !sd->ctx) return;

    LZ4F_freeDecompressionContext((LZ4F_dctx *)sd->ctx);
    sd->ctx = NULL;
}

size_t compressionLz4OutputBound(size_t input_len) {
    /* Conservative worst-case: data bound + frame header + flush/end overhead.
     * Always includes all components so the caller can allocate once and reuse
     * for any flush mode and frame state. */
    return LZ4F_compressBound(input_len, &lz4f_prefs) + LZ4F_HEADER_SIZE_MAX + LZ4F_compressBound(0, &lz4f_prefs);
}

ssize_t compressionLz4CompressFeed(streamCompressor *sc,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   const uint8_t *input,
                                   size_t input_len,
                                   compressFlushMode flush_mode) {
    LZ4F_cctx *cctx;
    size_t offset = 0;

    if (!sc || !sc->ctx) return -1;

    cctx = (LZ4F_cctx *)sc->ctx;

    /* Begin frame on first call */
    if (!sc->stream_started) {
        /* Local copy of shared prefs so we can set the actual level
         * and checksum mode per-stream. */
        LZ4F_preferences_t prefs = lz4f_prefs;
        size_t r;

        prefs.compressionLevel = sc->level;
        prefs.frameInfo.blockChecksumFlag = sc->codec_checksum
                                                ? LZ4F_blockChecksumEnabled
                                                : LZ4F_noBlockChecksum;
        r = LZ4F_compressBegin(cctx, output, output_capacity, &prefs);
        if (LZ4F_isError(r)) {
            /* compressBegin failure before any frame bytes are emitted is
             * recoverable — the LZ4F context is still clean. Caller can
             * retry with a larger buffer. Don't set errored. */
            return -1;
        }
        offset = r;
        sc->stream_started = true;
    }

    /* Compress input data */
    if (input_len > 0) {
        size_t r;

        /* Capacity shortage before calling into LZ4F is retriable because the
         * codec state has not changed yet. */
        if (offset >= output_capacity) return -1;
        r = LZ4F_compressUpdate(cctx, output + offset, output_capacity - offset,
                                input, input_len, NULL);
        if (LZ4F_isError(r)) goto lz4_error;
        offset += r;
    }

    /* Handle flush/end modes */
    if (flush_mode == FLUSH_SYNC) {
        size_t r;

        /* Capacity shortage before calling into LZ4F is retriable because the
         * codec state has not changed yet. */
        if (offset >= output_capacity) return -1;
        r = LZ4F_flush(cctx, output + offset, output_capacity - offset, NULL);
        if (LZ4F_isError(r)) goto lz4_error;
        offset += r;
    } else if (flush_mode == FLUSH_END) {
        size_t r;

        /* Capacity shortage before calling into LZ4F is retriable because the
         * codec state has not changed yet. */
        if (offset >= output_capacity) return -1;
        r = LZ4F_compressEnd(cctx, output + offset, output_capacity - offset, NULL);
        if (LZ4F_isError(r)) goto lz4_error;
        offset += r;
        sc->stream_started = false;
    }

    if (offset > (size_t)SSIZE_MAX) goto lz4_error;
    return (ssize_t)offset;

lz4_error:
    /* LZ4F state is undefined after an error (lz4frame.h line 325).
     * Mark permanently failed — no mid-stream retry is possible because
     * already-emitted frame bytes cannot be unsent. The caller must
     * tear down the stream (disconnect replica / abort RDB save). */
    sc->errored = true;
    return -1;
}

ssize_t compressionLz4DecompressFeed(streamDecompressor *sd,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     const uint8_t *input,
                                     size_t input_len,
                                     size_t *input_consumed) {
    LZ4F_dctx *dctx;
    size_t dst_size;
    size_t src_size;
    size_t ret;

    if (!sd || !sd->ctx || !input_consumed) return -1;

    dctx = (LZ4F_dctx *)sd->ctx;
    dst_size = output_capacity;
    src_size = input_len;
    ret = LZ4F_decompress(dctx, output, &dst_size, input, &src_size, NULL);
    if (LZ4F_isError(ret)) {
        sd->errored = true;
        return -1;
    }
    *input_consumed = src_size;
    sd->input_hint = ret;
    if (ret == 0) sd->frame_done = true;
    if (dst_size > (size_t)SSIZE_MAX) {
        sd->errored = true;
        return -1;
    }
    return (ssize_t)dst_size;
}
