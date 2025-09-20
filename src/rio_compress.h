/*
 * RIO compression filter for framed RDB output.
 *
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_RIO_COMPRESS_H
#define VALKEY_RIO_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

#include "rio.h"
#include "rdb_codec.h"
#include "sds.h"

struct rdb_frame_opts;
typedef struct rdb_frame_opts rdb_frame_opts;

typedef struct rio_compress {
    rio *dst;                 /* underlying sink (file/conn) */
    rdb_codec_ctx *cctx;
    rdb_codec_t codec;
    size_t blk_target, blk_limit; /* limit can be = 2*target */
    sds rawbuf, cmpbuf;
    uint64_t blocks;          /* emitted count */
    int checksum;             /* crc64|none */
    /* embed a rio vtable so this object *is* a rio */
    rio rio_itf;
} rio_compress;

int rioInitCompress(rio_compress *rc, rio *dst, const rdb_frame_opts *opts);
int rioCompressFlush(rio_compress *rc, int last);
void rioCompressCleanup(rio_compress *rc);

#endif /* VALKEY_RIO_COMPRESS_H */
