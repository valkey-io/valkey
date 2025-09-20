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
#include <sys/types.h>

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

/* Frame checksums. */
#ifndef RDB_FR_CHECKSUM_CRC64
#define RDB_FR_CHECKSUM_CRC64 0
#define RDB_FR_CHECKSUM_NONE 1
#endif

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

typedef enum {
    RDB_FRAME_PARSE_OK = 0,
    RDB_FRAME_PARSE_INVALID_FORMAT,
    RDB_FRAME_PARSE_UNKNOWN_FIELD
} rdbFrameParseResult;

const char *rdbFrameCodecToString(int codec);
int rdbFrameCodecFromString(const char *token);
int rdbFrameCodecFromRdbCodec(int codec);
int rdbFrameCodecToRdbCodec(int frame_codec);
const char *rdbFrameChecksumToString(int checksum);
int rdbFrameChecksumFromString(const char *token);
ssize_t rdbFrameFormatConfigLine(char *buf, size_t buf_len, int codec, size_t block_bytes, int checksum);
rdbFrameParseResult rdbFrameParseConfigTriplet(char *buf,
                                               const char **codec_token,
                                               const char **blk_token,
                                               const char **checksum_token);

int rdbFrameCodecFromRdbCodecOrDefault(int codec, int default_frame_codec);
int rdbFrameChecksumOrDefault(int checksum, int default_checksum);
size_t rdbFrameBlockSizeOrDefault(size_t requested_block,
                                  size_t default_block,
                                  size_t min_block);

#endif /* RDB_FRAME_H */
