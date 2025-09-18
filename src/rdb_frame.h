/*
 * RDB frame format definitions.
 *
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RDB_FRAME_H
#define RDB_FRAME_H

#include <stdint.h>
#include <stddef.h>

#define RDB_FR_MAGIC0 'R'
#define RDB_FR_MAGIC1 'B'
#define RDB_FR_MAGIC2 'C'
#define RDB_FR_MAGIC3 1 /* version */

/* Supported frame codecs. */
#define RDB_FR_CODEC_RAW 0
#define RDB_FR_CODEC_LZ4 1
#define RDB_FR_CODEC_LZF 2

/* Frame flags. */
#define RDB_FR_FLAG_LAST 1

typedef struct __attribute__((packed)) RdbFrameBlockHdr {
    uint8_t magic[4];   /* RBC\1 */
    uint8_t codec;      /* 0=RAW,1=LZ4,2=LZF */
    uint8_t flags;      /* bit0: last-block */
    uint32_t raw_len_le; /* uncompressed bytes */
    uint32_t cmp_len_le; /* compressed bytes */
    uint64_t crc64_le;   /* over header (except crc) + payload */
} RdbFrameBlockHdr;

static inline int rdbFrameHasMagicPrefix(const unsigned char *buf, size_t len) {
    return len >= 4 && buf[0] == RDB_FR_MAGIC0 && buf[1] == RDB_FR_MAGIC1 && buf[2] == RDB_FR_MAGIC2 &&
           buf[3] == RDB_FR_MAGIC3;
}

#endif /* RDB_FRAME_H */
