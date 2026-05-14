/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_lz4.h"
#include <limits.h>
#include <lz4frame.h>

/* Shared bound-calc preferences. The actual compress level and checksum mode
 * are overridden per stream before LZ4F_compressBegin. */
static const LZ4F_preferences_t lz4f_prefs = {
    .frameInfo = {
        .blockChecksumFlag = LZ4F_blockChecksumEnabled,
        .contentChecksumFlag = LZ4F_noContentChecksum,
        .blockSizeID = LZ4F_max64KB,
        .blockMode = LZ4F_blockIndependent,
    },
    .compressionLevel = 0,
};

int compressionLz4CompressorInit(streamCompressor *sc) {
    if (!sc) return -1;
    LZ4F_cctx *cctx = NULL;
    if (LZ4F_isError(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION))) return -1;
    sc->ctx = cctx;
    return 0;
}

void compressionLz4CompressorDestroy(streamCompressor *sc) {
    if (!sc || !sc->ctx) return;
    LZ4F_freeCompressionContext((LZ4F_cctx *)sc->ctx);
    sc->ctx = NULL;
}

int compressionLz4DecompressorInit(streamDecompressor *sd) {
    if (!sd) return -1;
    LZ4F_dctx *dctx = NULL;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION))) return -1;
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
    return LZ4F_compressBound(input_len, &lz4f_prefs) + LZ4F_HEADER_SIZE_MAX + LZ4F_compressBound(0, &lz4f_prefs);
}

ssize_t compressionLz4CompressFeed(streamCompressor *sc,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   const uint8_t *input,
                                   size_t input_len,
                                   compressFlushMode flush_mode) {
    if (!sc || !sc->ctx) return -1;

    LZ4F_cctx *cctx = (LZ4F_cctx *)sc->ctx;
    size_t offset = 0;

    /* All capacity-shortage early returns below are retriable: they happen
     * before LZ4F mutates its own state, so the caller can grow the buffer
     * and retry without breaking the frame. LZ4F errors after that point
     * latch sc->errored — no mid-stream retry is possible. */

    if (!sc->stream_started) {
        LZ4F_preferences_t prefs = lz4f_prefs;
        prefs.compressionLevel = sc->level;
        prefs.frameInfo.blockChecksumFlag = sc->codec_checksum
                                                ? LZ4F_blockChecksumEnabled
                                                : LZ4F_noBlockChecksum;
        size_t r = LZ4F_compressBegin(cctx, output, output_capacity, &prefs);
        if (LZ4F_isError(r)) return -1;
        offset = r;
        sc->stream_started = true;
    }

    if (input_len > 0) {
        if (offset >= output_capacity) return -1;
        size_t r = LZ4F_compressUpdate(cctx, output + offset, output_capacity - offset, input, input_len, NULL);
        if (LZ4F_isError(r)) goto lz4_error;
        offset += r;
    }

    if (flush_mode == FLUSH_SYNC) {
        if (offset >= output_capacity) return -1;
        size_t r = LZ4F_flush(cctx, output + offset, output_capacity - offset, NULL);
        if (LZ4F_isError(r)) goto lz4_error;
        offset += r;
    } else if (flush_mode == FLUSH_END) {
        if (offset >= output_capacity) return -1;
        size_t r = LZ4F_compressEnd(cctx, output + offset, output_capacity - offset, NULL);
        if (LZ4F_isError(r)) goto lz4_error;
        offset += r;
        sc->stream_started = false;
    }

    if (offset > (size_t)SSIZE_MAX) goto lz4_error;
    return (ssize_t)offset;

lz4_error:
    sc->errored = true;
    return -1;
}

ssize_t compressionLz4DecompressFeed(streamDecompressor *sd,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     const uint8_t *input,
                                     size_t input_len,
                                     size_t *input_consumed) {
    if (!sd || !sd->ctx || !input_consumed) return -1;

    LZ4F_dctx *dctx = (LZ4F_dctx *)sd->ctx;
    size_t dst_size = output_capacity;
    size_t src_size = input_len;
    size_t ret = LZ4F_decompress(dctx, output, &dst_size, input, &src_size, NULL);
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
