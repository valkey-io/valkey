/*
 * RDB frame utilities.
 *
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rdb_frame.h"

#include "rdb_codec.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifndef C_OK
#define C_OK 0
#endif
#ifndef C_ERR
#define C_ERR -1
#endif

const char *rdbFrameCodecToString(int codec) {
    switch (codec) {
    case RDB_FR_CODEC_RAW:
        return "raw";
    case RDB_FR_CODEC_LZ4:
        return "lz4";
    case RDB_FR_CODEC_LZF:
        return "lzf";
    default:
        break;
    }
    return NULL;
}

int rdbFrameCodecFromString(const char *token) {
    if (token == NULL) return -1;
    if (!strcasecmp(token, "raw")) return RDB_FR_CODEC_RAW;
    if (!strcasecmp(token, "lz4")) return RDB_FR_CODEC_LZ4;
    if (!strcasecmp(token, "lzf")) return RDB_FR_CODEC_LZF;
    return -1;
}

int rdbFrameCodecFromRdbCodec(int codec) {
    switch (codec) {
    case RDBC_RAW:
        return RDB_FR_CODEC_RAW;
    case RDBC_LZ4:
        return RDB_FR_CODEC_LZ4;
    case RDBC_LZF:
        return RDB_FR_CODEC_LZF;
    default:
        break;
    }
    return -1;
}

int rdbFrameCodecToRdbCodec(int frame_codec) {
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
    return -1;
}

const char *rdbFrameChecksumToString(int checksum) {
    switch (checksum) {
    case RDB_FR_CHECKSUM_CRC64:
        return "crc64";
    case RDB_FR_CHECKSUM_NONE:
        return "none";
    default:
        break;
    }
    return NULL;
}

int rdbFrameChecksumFromString(const char *token) {
    if (token == NULL) return -1;
    if (!strcasecmp(token, "crc64")) return RDB_FR_CHECKSUM_CRC64;
    if (!strcasecmp(token, "none")) return RDB_FR_CHECKSUM_NONE;
    return -1;
}

ssize_t rdbFrameFormatConfigLine(char *buf, size_t buf_len, int codec, size_t block_bytes, int checksum) {
    if (buf == NULL || buf_len == 0) return -1;

    const char *codec_str = rdbFrameCodecToString(codec);
    const char *checksum_str = rdbFrameChecksumToString(checksum);
    if (codec_str == NULL || checksum_str == NULL) return -1;

    int len = snprintf(buf, buf_len, "codec=%s blk=%zu checksum=%s", codec_str, block_bytes, checksum_str);
    if (len < 0) return -1;
    if ((size_t)len >= buf_len) return -1;
    return len;
}

int rdbFrameCodecFromRdbCodecOrDefault(int codec, int default_frame_codec) {
    int frame_codec = rdbFrameCodecFromRdbCodec(codec);
    if (frame_codec >= 0) return frame_codec;
    if (rdbFrameCodecToString(default_frame_codec) == NULL) return RDB_FR_CODEC_RAW;
    return default_frame_codec;
}

int rdbFrameChecksumOrDefault(int checksum, int default_checksum) {
    if (rdbFrameChecksumToString(checksum) != NULL) return checksum;
    if (rdbFrameChecksumToString(default_checksum) == NULL) return RDB_FR_CHECKSUM_CRC64;
    return default_checksum;
}

size_t rdbFrameBlockSizeOrDefault(size_t requested_block, size_t default_block, size_t min_block) {
    size_t block = requested_block ? requested_block : default_block;
    if (block == 0) block = min_block ? min_block : default_block;
    if (min_block && block < min_block) block = min_block;
    return block;
}

rdbFrameParseResult rdbFrameParseConfigTriplet(char *buf,
                                               const char **codec_token,
                                               const char **blk_token,
                                               const char **checksum_token) {
    if (buf == NULL || codec_token == NULL || blk_token == NULL || checksum_token == NULL) {
        return RDB_FRAME_PARSE_INVALID_FORMAT;
    }

    *codec_token = NULL;
    *blk_token = NULL;
    *checksum_token = NULL;

    char *saveptr = NULL;
    int field_count = 0;
    for (char *field = strtok_r(buf, " ", &saveptr); field != NULL; field = strtok_r(NULL, " ", &saveptr)) {
        field_count++;
        if (!strncmp(field, "codec=", 6)) {
            if (field[6] == '\0') return RDB_FRAME_PARSE_INVALID_FORMAT;
            if (*codec_token != NULL) return RDB_FRAME_PARSE_INVALID_FORMAT;
            *codec_token = field + 6;
        } else if (!strncmp(field, "blk=", 4)) {
            if (field[4] == '\0') return RDB_FRAME_PARSE_INVALID_FORMAT;
            if (*blk_token != NULL) return RDB_FRAME_PARSE_INVALID_FORMAT;
            *blk_token = field + 4;
        } else if (!strncmp(field, "checksum=", 9)) {
            if (field[9] == '\0') return RDB_FRAME_PARSE_INVALID_FORMAT;
            if (*checksum_token != NULL) return RDB_FRAME_PARSE_INVALID_FORMAT;
            *checksum_token = field + 9;
        } else {
            return RDB_FRAME_PARSE_UNKNOWN_FIELD;
        }
    }

    if (field_count != 3 || *codec_token == NULL || *blk_token == NULL || *checksum_token == NULL) {
        return RDB_FRAME_PARSE_INVALID_FORMAT;
    }

    return RDB_FRAME_PARSE_OK;
}
