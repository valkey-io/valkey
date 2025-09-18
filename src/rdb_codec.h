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
    RDBC_CODEC_RAW = 0,
    RDBC_CODEC_LZF = 1,
    RDBC_CODEC_LZ4 = 2
} rdb_codec_t;

#ifndef RDBC_RAW
#define RDBC_RAW RDBC_CODEC_RAW
#endif
#ifndef RDBC_LZF
#define RDBC_LZF RDBC_CODEC_LZF
#endif
#ifndef RDBC_LZ4
#define RDBC_LZ4 RDBC_CODEC_LZ4
#endif

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
