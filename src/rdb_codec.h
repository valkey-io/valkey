/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_RDB_CODEC_H
#define VALKEY_RDB_CODEC_H

#include <stddef.h>
#include "sds.h"

typedef enum {
    RDBC_RAW = 0,
    RDBC_LZ4 = 1,
    RDBC_LZF = 2
} rdb_codec_t;

typedef struct rdb_codec_ctx rdb_codec_ctx;

rdb_codec_ctx *rdbCodecCreate(rdb_codec_t codec);
void rdbCodecFree(rdb_codec_ctx *ctx);

/* Compress 'src' -> grows 'dst' as needed; may return RAW passthrough if ratio poor. */
int rdbCodecCompress(rdb_codec_ctx *ctx,
                     const void *src, size_t src_len,
                     sds *dst, rdb_codec_t *actual_codec);

/* Decompress depending on 'codec' -> into 'dst' (sds). */
int rdbCodecDecompress(rdb_codec_t codec,
                       const void *src, size_t src_len,
                       sds *dst);

#endif /* VALKEY_RDB_CODEC_H */
