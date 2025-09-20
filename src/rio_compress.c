/*
 * RIO compression filter that turns raw RDB bytes into framed blocks.
 *
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rio_compress.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crc64.h"
#include "endianconv.h"
#include "rdb_frame.h"
#include "server.h"

static inline rio_compress *rioCompressFromRio(rio *r) {
    return (rio_compress *)((unsigned char *)r - offsetof(rio_compress, rio_itf));
}

static int rioCompressWriteToDst(rio_compress *rc, const void *buf, size_t len) {
    if (len == 0) return C_OK;
    if (rc->dst == NULL) return C_ERR;

    rio *dst = rc->dst;
    void (*saved_cksum)(rio *, const void *, size_t) = dst->update_cksum;
    dst->update_cksum = NULL;
    int ok = rioWrite(dst, buf, len) != 0 ? C_OK : C_ERR;
    dst->update_cksum = saved_cksum;
    return ok;
}

static size_t rioCompressWrite(rio *r, const void *buf, size_t len) {
    if (len == 0) return 1;

    rio_compress *rc = rioCompressFromRio(r);
    const unsigned char *p = buf;
    size_t remaining = len;

    while (remaining > 0) {
        size_t raw_len = sdslen(rc->rawbuf);
        if (raw_len >= rc->blk_limit) {
            if (rioCompressFlush(rc, 0) == C_ERR) return 0;
            raw_len = sdslen(rc->rawbuf);
        }

        size_t space = rc->blk_limit > raw_len ? rc->blk_limit - raw_len : 0;
        if (space == 0) {
            if (rioCompressFlush(rc, 0) == C_ERR) return 0;
            continue;
        }

        size_t chunk = remaining < space ? remaining : space;
        sds newbuf = sdscatlen(rc->rawbuf, p, chunk);
        if (newbuf == NULL) {
            r->flags |= RIO_FLAG_WRITE_ERROR;
            return 0;
        }
        rc->rawbuf = newbuf;
        p += chunk;
        remaining -= chunk;

        if (sdslen(rc->rawbuf) >= rc->blk_target) {
            if (rioCompressFlush(rc, 0) == C_ERR) return 0;
        }
    }

    return 1;
}

static off_t rioCompressTell(rio *r) {
    rio_compress *rc = rioCompressFromRio(r);
    if (rc->dst && rc->dst->tell) return rc->dst->tell(rc->dst);
    return -1;
}

static int rioCompressFlushFn(rio *r) {
    rio_compress *rc = rioCompressFromRio(r);
    if (rioCompressFlush(rc, 0) == C_ERR) return 0;
    return rc->dst && rc->dst->flush ? rc->dst->flush(rc->dst) : 1;
}

static void rioCompressUpdateChecksum(rio *r, const void *buf, size_t len) {
    rio_compress *rc = rioCompressFromRio(r);
    if ((r->flags & RIO_FLAG_SKIP_RDB_CHECKSUM) != 0) return;

    r->cksum = crc64(r->cksum, buf, len);
    if (rc->dst && rc->dst->update_cksum && (rc->dst->flags & RIO_FLAG_SKIP_RDB_CHECKSUM) == 0) {
        rc->dst->update_cksum(rc->dst, buf, len);
    }
}

static int rioCompressInitBuffers(rio_compress *rc) {
    rc->rawbuf = sdsempty();
    rc->cmpbuf = sdsempty();
    if (rc->rawbuf == NULL || rc->cmpbuf == NULL) {
        sdsfree(rc->rawbuf);
        sdsfree(rc->cmpbuf);
        rc->rawbuf = rc->cmpbuf = NULL;
        return C_ERR;
    }
    return C_OK;
}

static rdb_codec_t rioCompressSelectCodec(int frame_codec) {
    switch (frame_codec) {
    case RDB_FR_CODEC_RAW:
        return RDBC_RAW;
    case RDB_FR_CODEC_LZ4:
        return RDBC_LZ4;
    case RDB_FR_CODEC_LZF:
        return RDBC_LZF;
    default:
        break;
    }
    return RDBC_RAW;
}

int rioInitCompress(rio_compress *rc, rio *dst, const rdb_frame_opts *opts) {
    if (rc == NULL || dst == NULL || opts == NULL) return C_ERR;

    memset(rc, 0, sizeof(*rc));
    rc->dst = dst;

    rc->codec = rioCompressSelectCodec(opts->codec);
    rc->cctx = rdbCodecCreate(rc->codec);
    if (rc->cctx == NULL) return C_ERR;

    rc->blk_target = opts->block_bytes ? opts->block_bytes : 262144;
    if (rc->blk_target == 0) rc->blk_target = 262144;
    if (rc->blk_target > SIZE_MAX / 2) rc->blk_limit = rc->blk_target;
    else rc->blk_limit = rc->blk_target * 2;
    if (rc->blk_limit < rc->blk_target) rc->blk_limit = rc->blk_target;

    rc->checksum = opts->checksum;

    if (rioCompressInitBuffers(rc) == C_ERR) {
        rdbCodecFree(rc->cctx);
        rc->cctx = NULL;
        return C_ERR;
    }

    memset(&rc->rio_itf, 0, sizeof(rc->rio_itf));
    rc->rio_itf.write = rioCompressWrite;
    rc->rio_itf.tell = rioCompressTell;
    rc->rio_itf.flush = rioCompressFlushFn;
    rc->rio_itf.update_cksum = rioCompressUpdateChecksum;
    rc->rio_itf.max_processing_chunk = dst->max_processing_chunk;

    return C_OK;
}

int rioCompressFlush(rio_compress *rc, int last) {
    if (rc == NULL) return C_ERR;

    size_t raw_len = sdslen(rc->rawbuf);
    if (raw_len == 0 && !last) return C_OK;

    rdb_codec_t actual_codec = rc->codec;
    const void *payload = rc->rawbuf;
    size_t payload_len = raw_len;

    if (raw_len > 0 && rc->codec != RDBC_RAW) {
        if (rdbCodecCompress(rc->cctx, rc->rawbuf, raw_len, &rc->cmpbuf, &actual_codec) == C_ERR) return C_ERR;
        if (actual_codec == RDBC_RAW) {
            payload = rc->rawbuf;
            payload_len = raw_len;
        } else if (actual_codec == RDBC_LZ4) {
            payload = rc->cmpbuf;
            payload_len = sdslen(rc->cmpbuf);
        } else if (actual_codec == RDBC_LZF) {
            payload = rc->cmpbuf;
            payload_len = sdslen(rc->cmpbuf);
        } else {
            return C_ERR;
        }
    } else {
        actual_codec = RDBC_RAW;
        payload = rc->rawbuf;
        payload_len = raw_len;
    }

    if (raw_len > UINT32_MAX || payload_len > UINT32_MAX) return C_ERR;

    RdbFrameBlockHdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0] = RDB_FR_MAGIC0;
    hdr.magic[1] = RDB_FR_MAGIC1;
    hdr.magic[2] = RDB_FR_MAGIC2;
    hdr.magic[3] = RDB_FR_MAGIC3;
    int frame_codec = rdbFrameCodecFromRdbCodecOrDefault(actual_codec, RDB_FR_CODEC_RAW);
    hdr.codec = (uint8_t)frame_codec;
    hdr.flags = last ? RDB_FR_FLAG_LAST : 0;
    hdr.raw_len_le = (uint32_t)raw_len;
    hdr.cmp_len_le = (uint32_t)payload_len;
    memrev32ifbe(&hdr.raw_len_le);
    memrev32ifbe(&hdr.cmp_len_le);

    uint64_t crc = 0;
    if (rc->checksum == RDB_FR_CHECKSUM_CRC64) {
        crc = crc64(0, (unsigned char *)&hdr, offsetof(RdbFrameBlockHdr, crc64_le));
        if (payload_len > 0) crc = crc64(crc, payload, payload_len);
    }
    hdr.crc64_le = crc;
    memrev64ifbe(&hdr.crc64_le);

    if (rioCompressWriteToDst(rc, &hdr, sizeof(hdr)) == C_ERR) return C_ERR;
    if (payload_len > 0 && rioCompressWriteToDst(rc, payload, payload_len) == C_ERR) return C_ERR;

    rc->blocks++;
    sdsclear(rc->rawbuf);
    sdsclear(rc->cmpbuf);
    return C_OK;
}

void rioCompressCleanup(rio_compress *rc) {
    if (rc == NULL) return;
    if (rc->cctx) {
        rdbCodecFree(rc->cctx);
        rc->cctx = NULL;
    }
    if (rc->rawbuf) {
        sdsfree(rc->rawbuf);
        rc->rawbuf = NULL;
    }
    if (rc->cmpbuf) {
        sdsfree(rc->cmpbuf);
        rc->cmpbuf = NULL;
    }
    rc->dst = NULL;
    rc->blocks = 0;
}
