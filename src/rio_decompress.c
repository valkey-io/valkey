/*
 * RIO decompression filter that turns framed RDB blocks into raw bytes.
 *
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rio_decompress.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crc64.h"
#include "endianconv.h"
#include "rdb_codec.h"
#include "rdb_frame.h"
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

static inline rio_decompress *rioDecompressFromRio(rio *r) {
    return (rio_decompress *)((unsigned char *)r - offsetof(rio_decompress, rio_itf));
}

static size_t rioDecompressWrite(rio *r, const void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    errno = EINVAL;
    return 0;
}

static int rioDecompressReadFromSrc(rio_decompress *rd, void *buf, size_t len) {
    if (len == 0) return C_OK;
    if (rd->src == NULL) return C_ERR;

    rio *src = rd->src;
    void (*saved_cksum)(rio *, const void *, size_t) = src->update_cksum;
    src->update_cksum = NULL;
    int ok = rioRead(src, buf, len) != 0 ? C_OK : C_ERR;
    src->update_cksum = saved_cksum;
    return ok;
}

static int rioDecompressFrameCodecToCodec(uint8_t frame_codec, rdb_codec_t *codec) {
    switch (frame_codec) {
    case RDB_FR_CODEC_RAW:
        *codec = RDBC_RAW;
        return C_OK;
    case RDB_FR_CODEC_LZ4:
        *codec = RDBC_LZ4;
        return C_OK;
    case RDB_FR_CODEC_LZF:
        *codec = RDBC_LZF;
        return C_OK;
    default:
        break;
    }
    return C_ERR;
}

static int rioDecompressLoadBlock(rio_decompress *rd) {
    if (rd->src == NULL) return C_ERR;

    unsigned char hdrbuf[sizeof(RdbFrameBlockHdr)];
    if (rioDecompressReadFromSrc(rd, hdrbuf, sizeof(hdrbuf)) == C_ERR) return C_ERR;

    RdbFrameBlockHdr hdr;
    memcpy(&hdr, hdrbuf, sizeof(hdr));

    if (hdr.magic[0] != RDB_FR_MAGIC0 || hdr.magic[1] != RDB_FR_MAGIC1 || hdr.magic[2] != RDB_FR_MAGIC2 ||
        hdr.magic[3] != RDB_FR_MAGIC3) {
        errno = EINVAL;
        return C_ERR;
    }
    if ((hdr.flags & (uint8_t)~RDB_FR_FLAG_LAST) != 0) {
        errno = EINVAL;
        return C_ERR;
    }

    uint32_t raw_len32 = hdr.raw_len_le;
    uint32_t cmp_len32 = hdr.cmp_len_le;
    memrev32ifbe(&raw_len32);
    memrev32ifbe(&cmp_len32);
    size_t raw_len = raw_len32;
    size_t cmp_len = cmp_len32;

    uint64_t stored_crc = hdr.crc64_le;
    memrev64ifbe(&stored_crc);

    rdb_codec_t codec;
    if (rioDecompressFrameCodecToCodec(hdr.codec, &codec) == C_ERR) {
        errno = EINVAL;
        return C_ERR;
    }

    rd->eof = (hdr.flags & RDB_FR_FLAG_LAST) != 0;
    rd->pos = 0;
    sdsclear(rd->rawbuf);

    if (codec == RDBC_RAW) {
        if (cmp_len != raw_len) {
            errno = EINVAL;
            return C_ERR;
        }
        if (raw_len > 0) {
            sds buf = sdsMakeRoomFor(rd->rawbuf, raw_len);
            if (buf == NULL) {
                errno = ENOMEM;
                return C_ERR;
            }
            rd->rawbuf = buf;
            if (rioDecompressReadFromSrc(rd, rd->rawbuf, raw_len) == C_ERR) return C_ERR;
            sdsIncrLen(rd->rawbuf, raw_len);
        }

        if (stored_crc != 0) {
            uint64_t crc = crc64(0, hdrbuf, offsetof(RdbFrameBlockHdr, crc64_le));
            if (raw_len > 0) crc = crc64(crc, (unsigned char *)rd->rawbuf, raw_len);
            if (crc != stored_crc) {
                errno = EIO;
                return C_ERR;
            }
        }
        return C_OK;
    }

    unsigned char *payload = NULL;
    if (cmp_len > 0) {
        payload = zmalloc(cmp_len);
        if (payload == NULL) {
            errno = ENOMEM;
            return C_ERR;
        }
        if (rioDecompressReadFromSrc(rd, payload, cmp_len) == C_ERR) {
            zfree(payload);
            return C_ERR;
        }
    }

    if (stored_crc != 0) {
        uint64_t crc = crc64(0, hdrbuf, offsetof(RdbFrameBlockHdr, crc64_le));
        if (cmp_len > 0) crc = crc64(crc, payload, cmp_len);
        if (crc != stored_crc) {
            if (payload) zfree(payload);
            errno = EIO;
            return C_ERR;
        }
    }

    if (rdbCodecDecompress(codec, payload, cmp_len, &rd->rawbuf) == C_ERR) {
        if (payload) zfree(payload);
        errno = EINVAL;
        return C_ERR;
    }
    if (payload) zfree(payload);

    if (sdslen(rd->rawbuf) != raw_len) {
        errno = EINVAL;
        return C_ERR;
    }
    return C_OK;
}

static size_t rioDecompressRead(rio *r, void *buf, size_t len) {
    if (len == 0) return 1;

    rio_decompress *rd = rioDecompressFromRio(r);
    unsigned char *out = buf;
    size_t remaining = len;

    while (remaining > 0) {
        size_t raw_len = sdslen(rd->rawbuf);
        size_t avail = raw_len > rd->pos ? raw_len - rd->pos : 0;
        if (avail == 0) {
            if (rd->eof) {
                r->flags |= RIO_FLAG_READ_ERROR;
                errno = EIO;
                return 0;
            }
            if (rioDecompressLoadBlock(rd) == C_ERR) {
                r->flags |= RIO_FLAG_READ_ERROR;
                return 0;
            }
            continue;
        }

        size_t chunk = avail < remaining ? avail : remaining;
        memcpy(out, rd->rawbuf + rd->pos, chunk);
        rd->pos += chunk;
        out += chunk;
        remaining -= chunk;
    }

    return 1;
}

static off_t rioDecompressTell(rio *r) {
    return (off_t)r->processed_bytes;
}

static int rioDecompressFlush(rio *r) {
    UNUSED(r);
    return 1;
}

int rioInitDecompress(rio_decompress *rd, rio *src) {
    if (rd == NULL || src == NULL) return C_ERR;

    memset(rd, 0, sizeof(*rd));
    rd->src = src;
    rd->rawbuf = sdsempty();
    if (rd->rawbuf == NULL) return C_ERR;

    rd->rio_itf.read = rioDecompressRead;
    rd->rio_itf.write = rioDecompressWrite;
    rd->rio_itf.tell = rioDecompressTell;
    rd->rio_itf.flush = rioDecompressFlush;
    rd->rio_itf.update_cksum = NULL;
    rd->rio_itf.max_processing_chunk = src->max_processing_chunk;
    rd->rio_itf.flags = src->flags & RIO_FLAG_SKIP_RDB_CHECKSUM;

    return C_OK;
}
