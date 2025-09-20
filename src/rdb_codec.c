/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rdb_codec.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include "lzf.h"
#include "lz4.h"
#include "zmalloc.h"

#ifndef C_OK
#define C_OK 0
#endif
#ifndef C_ERR
#define C_ERR -1
#endif
#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

struct rdb_codec_ctx {
    rdb_codec_t codec;
    LZ4_stream_t *lz4_stream;
};

static int rdbCodecEnsureOutputBuffer(sds *buf) {
    if (*buf == NULL) {
        *buf = sdsempty();
        if (*buf == NULL) return C_ERR;
    } else {
        sdsclear(*buf);
    }
    return C_OK;
}

static int rdbCodecEnsureCapacity(sds *buf, size_t needed) {
    if (needed == 0) return C_OK;
    size_t len = sdslen(*buf);
    size_t avail = sdsavail(*buf);
    if (len + avail >= needed) return C_OK;
    size_t add = needed - len;
    *buf = sdsMakeRoomFor(*buf, add);
    return *buf ? C_OK : C_ERR;
}

static int rdbCodecStoreRaw(const void *src, size_t src_len, sds *dst, rdb_codec_t *actual_codec) {
    if (src_len > 0) {
        *dst = sdscpylen(*dst, src, src_len);
        if (*dst == NULL) return C_ERR;
    } else {
        sdsclear(*dst);
    }
    if (actual_codec) *actual_codec = RDBC_RAW;
    return C_OK;
}

static int rdbCodecShouldFallback(size_t cmp_len, size_t src_len) {
    if (src_len == 0) return 1;
    size_t threshold = src_len - (src_len >> 6);
    return cmp_len >= threshold;
}

static int rdbCodecCompressLZ4(rdb_codec_ctx *ctx, const void *src, size_t src_len, sds *dst, rdb_codec_t *actual_codec) {
    UNUSED(ctx);
    if (src_len > (size_t)LZ4_MAX_INPUT_SIZE || src_len > (size_t)INT_MAX) {
        return rdbCodecStoreRaw(src, src_len, dst, actual_codec);
    }

    int input_size = (int)src_len;
    int bound = LZ4_compressBound(input_size);
    if (bound <= 0) return rdbCodecStoreRaw(src, src_len, dst, actual_codec);
    if (rdbCodecEnsureCapacity(dst, (size_t)bound) == C_ERR) return C_ERR;

    char *out = *dst;
    int cmp_len = LZ4_compress_default(src, out, input_size, bound);
    if (cmp_len <= 0) return rdbCodecStoreRaw(src, src_len, dst, actual_codec);

    if (rdbCodecShouldFallback((size_t)cmp_len, src_len)) {
        return rdbCodecStoreRaw(src, src_len, dst, actual_codec);
    }

    sdssetlen(*dst, (size_t)cmp_len);
    out[cmp_len] = '\0';
    if (actual_codec) *actual_codec = RDBC_LZ4;
    return C_OK;
}

static int rdbCodecCompressLZF(const void *src, size_t src_len, sds *dst, rdb_codec_t *actual_codec) {
    size_t capacity = src_len > 0 ? src_len : 1;
    if (rdbCodecEnsureCapacity(dst, capacity) == C_ERR) return C_ERR;
    char *out = *dst;

    size_t cmp_len = lzf_compress(src, src_len, out, capacity);
    if (cmp_len == 0 || rdbCodecShouldFallback(cmp_len, src_len)) {
        return rdbCodecStoreRaw(src, src_len, dst, actual_codec);
    }

    sdssetlen(*dst, cmp_len);
    out[cmp_len] = '\0';
    if (actual_codec) *actual_codec = RDBC_LZF;
    return C_OK;
}

static int rdbCodecDecompressRaw(const void *src, size_t src_len, sds *dst) {
    return rdbCodecStoreRaw(src, src_len, dst, NULL);
}

static int rdbCodecDecompressLZF(const void *src, size_t src_len, sds *dst) {
    if (src_len == 0) {
        sdsclear(*dst);
        return C_OK;
    }

    size_t desired = sdsalloc(*dst);
    if (desired < src_len) desired = src_len;
    if (desired == 0) desired = src_len ? src_len : 64;

    for (size_t attempt = 0; attempt < 16; attempt++) {
        if (desired == 0) desired = 64;
        if (rdbCodecEnsureCapacity(dst, desired) == C_ERR) return C_ERR;
        char *buf = *dst;
        size_t capacity = sdsalloc(*dst);

        errno = 0;
        size_t out_len = lzf_decompress(src, src_len, buf, capacity);
        if (out_len > 0 || (out_len == 0 && errno == 0)) {
            sdssetlen(*dst, out_len);
            buf[out_len] = '\0';
            return C_OK;
        }
        if (errno != E2BIG) break;
        if (capacity >= SIZE_MAX) break;
        desired = capacity >= SIZE_MAX / 2 ? SIZE_MAX : capacity * 2;
        if (desired == capacity) break;
    }

    return C_ERR;
}

static int rdbCodecDecompressLZ4(const void *src, size_t src_len, sds *dst) {
    if (src_len == 0) {
        sdsclear(*dst);
        return C_OK;
    }
    if (src_len > (size_t)INT_MAX) return C_ERR;

    size_t desired = sdsalloc(*dst);
    if (desired < src_len) desired = src_len;
    if (desired == 0) {
        if (src_len > SIZE_MAX / 4) desired = (size_t)INT_MAX;
        else desired = src_len * 4;
        if (desired < src_len) desired = src_len;
    }
    if (desired == 0) desired = 64;
    if (desired > (size_t)INT_MAX) desired = (size_t)INT_MAX;

    for (size_t attempt = 0; attempt < 16; attempt++) {
        if (desired == 0) desired = 64;
        if (desired > (size_t)INT_MAX) desired = (size_t)INT_MAX;
        if (rdbCodecEnsureCapacity(dst, desired) == C_ERR) return C_ERR;
        char *buf = *dst;
        size_t capacity = sdsalloc(*dst);
        int dest_capacity = capacity > (size_t)INT_MAX ? INT_MAX : (int)capacity;

        int result = LZ4_decompress_safe(src, buf, (int)src_len, dest_capacity);
        if (result >= 0) {
            sdssetlen(*dst, (size_t)result);
            buf[result] = '\0';
            return C_OK;
        }
        if (dest_capacity == INT_MAX) break;
        size_t new_desired = capacity >= SIZE_MAX / 2 ? SIZE_MAX : capacity * 2;
        if (new_desired > (size_t)INT_MAX) new_desired = (size_t)INT_MAX;
        if (new_desired <= capacity) break;
        desired = new_desired;
    }

    return C_ERR;
}

rdb_codec_ctx *rdbCodecCreate(rdb_codec_t codec) {
    rdb_codec_ctx *ctx = zmalloc(sizeof(*ctx));
    if (ctx == NULL) return NULL;
    ctx->codec = codec;
    ctx->lz4_stream = NULL;

    if (codec == RDBC_LZ4) {
        ctx->lz4_stream = LZ4_createStream();
        if (ctx->lz4_stream == NULL) {
            zfree(ctx);
            return NULL;
        }
    }

    return ctx;
}

void rdbCodecFree(rdb_codec_ctx *ctx) {
    if (ctx == NULL) return;
    if (ctx->lz4_stream) LZ4_freeStream(ctx->lz4_stream);
    zfree(ctx);
}

int rdbCodecCompress(rdb_codec_ctx *ctx,
                     const void *src, size_t src_len,
                     sds *dst, rdb_codec_t *actual_codec) {
    if (ctx == NULL || dst == NULL || (src == NULL && src_len > 0)) return C_ERR;
    if (rdbCodecEnsureOutputBuffer(dst) == C_ERR) return C_ERR;

    if (ctx->codec == RDBC_RAW || src_len == 0) {
        return rdbCodecStoreRaw(src, src_len, dst, actual_codec);
    }

    switch (ctx->codec) {
    case RDBC_LZ4:
        return rdbCodecCompressLZ4(ctx, src, src_len, dst, actual_codec);
    case RDBC_LZF:
        return rdbCodecCompressLZF(src, src_len, dst, actual_codec);
    default:
        return rdbCodecStoreRaw(src, src_len, dst, actual_codec);
    }
}

int rdbCodecDecompress(rdb_codec_t codec,
                       const void *src, size_t src_len,
                       sds *dst) {
    if (dst == NULL || (src == NULL && src_len > 0)) return C_ERR;
    if (rdbCodecEnsureOutputBuffer(dst) == C_ERR) return C_ERR;

    switch (codec) {
    case RDBC_RAW:
        return rdbCodecDecompressRaw(src, src_len, dst);
    case RDBC_LZ4:
        return rdbCodecDecompressLZ4(src, src_len, dst);
    case RDBC_LZF:
        return rdbCodecDecompressLZF(src, src_len, dst);
    default:
        return C_ERR;
    }
}
