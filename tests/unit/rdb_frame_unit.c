/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>
#include <string.h>

#include "../../src/rdb_frame.h"
#include "../../src/rdb_codec.h"
#include "../../src/unit/test_help.h"

int test_rdb_frame_codec_helpers(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    TEST_ASSERT(strcmp(rdbFrameCodecToString(RDB_FR_CODEC_RAW), "raw") == 0);
    TEST_ASSERT(strcmp(rdbFrameCodecToString(RDB_FR_CODEC_LZ4), "lz4") == 0);
    TEST_ASSERT(strcmp(rdbFrameCodecToString(RDB_FR_CODEC_LZF), "lzf") == 0);
    TEST_ASSERT(rdbFrameCodecToString(-1) == NULL);

    TEST_ASSERT(rdbFrameCodecFromString("raw") == RDB_FR_CODEC_RAW);
    TEST_ASSERT(rdbFrameCodecFromString("LZ4") == RDB_FR_CODEC_LZ4);
    TEST_ASSERT(rdbFrameCodecFromString("lzf") == RDB_FR_CODEC_LZF);
    TEST_ASSERT(rdbFrameCodecFromString("unknown") == -1);
    TEST_ASSERT(rdbFrameCodecFromString(NULL) == -1);
    return 0;
}

int test_rdb_frame_checksum_helpers(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    TEST_ASSERT(strcmp(rdbFrameChecksumToString(RDB_FR_CHECKSUM_CRC64), "crc64") == 0);
    TEST_ASSERT(strcmp(rdbFrameChecksumToString(RDB_FR_CHECKSUM_NONE), "none") == 0);
    TEST_ASSERT(rdbFrameChecksumToString(-1) == NULL);

    TEST_ASSERT(rdbFrameChecksumFromString("crc64") == RDB_FR_CHECKSUM_CRC64);
    TEST_ASSERT(rdbFrameChecksumFromString("NONE") == RDB_FR_CHECKSUM_NONE);
    TEST_ASSERT(rdbFrameChecksumFromString("invalid") == -1);
    TEST_ASSERT(rdbFrameChecksumFromString(NULL) == -1);
    return 0;
}

int test_rdb_frame_parse_triplet_success(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char line[] = "codec=lz4 blk=131072 checksum=crc64";
    const char *codec = NULL;
    const char *blk = NULL;
    const char *checksum = NULL;
    TEST_ASSERT(rdbFrameParseConfigTriplet(line, &codec, &blk, &checksum) == RDB_FRAME_PARSE_OK);
    TEST_ASSERT(codec && blk && checksum);
    TEST_ASSERT(strcmp(codec, "lz4") == 0);
    TEST_ASSERT(strcmp(checksum, "crc64") == 0);
    TEST_ASSERT(rdbFrameCodecFromString(codec) == RDB_FR_CODEC_LZ4);
    TEST_ASSERT(rdbFrameChecksumFromString(checksum) == RDB_FR_CHECKSUM_CRC64);
    TEST_ASSERT(strtoull(blk, NULL, 10) == 131072ULL);
    return 0;
}

int test_rdb_frame_parse_triplet_errors(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *codec = NULL;
    const char *blk = NULL;
    const char *checksum = NULL;

    char missing[] = "codec=lz4 blk=65536";
    TEST_ASSERT(rdbFrameParseConfigTriplet(missing, &codec, &blk, &checksum) == RDB_FRAME_PARSE_INVALID_FORMAT);

    char duplicate[] = "codec=lz4 codec=lzf checksum=crc64";
    TEST_ASSERT(rdbFrameParseConfigTriplet(duplicate, &codec, &blk, &checksum) == RDB_FRAME_PARSE_INVALID_FORMAT);

    char unknown[] = "codec=lz4 foo=bar checksum=crc64";
    TEST_ASSERT(rdbFrameParseConfigTriplet(unknown, &codec, &blk, &checksum) == RDB_FRAME_PARSE_UNKNOWN_FIELD);
    return 0;
}

int test_rdb_frame_format_line(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char buf[64];
    ssize_t written = rdbFrameFormatConfigLine(buf, sizeof(buf), RDB_FR_CODEC_LZF, 65536, RDB_FR_CHECKSUM_NONE);
    TEST_ASSERT(written > 0);
    TEST_ASSERT((size_t)written == strlen(buf));
    TEST_ASSERT(strcmp(buf, "codec=lzf blk=65536 checksum=none") == 0);

    TEST_ASSERT(rdbFrameFormatConfigLine(NULL, sizeof(buf), RDB_FR_CODEC_RAW, 1, RDB_FR_CHECKSUM_CRC64) < 0);
    TEST_ASSERT(rdbFrameFormatConfigLine(buf, sizeof(buf), -1, 1, RDB_FR_CHECKSUM_NONE) < 0);
    TEST_ASSERT(rdbFrameFormatConfigLine(buf, 0, RDB_FR_CODEC_RAW, 1, RDB_FR_CHECKSUM_CRC64) < 0);
    TEST_ASSERT(rdbFrameFormatConfigLine(buf, 4, RDB_FR_CODEC_RAW, 1, RDB_FR_CHECKSUM_CRC64) < 0);
    return 0;
}

int test_rdb_frame_codec_cross_conversion(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    TEST_ASSERT(rdbFrameCodecFromRdbCodec(RDBC_RAW) == RDB_FR_CODEC_RAW);
    TEST_ASSERT(rdbFrameCodecFromRdbCodec(RDBC_LZ4) == RDB_FR_CODEC_LZ4);
    TEST_ASSERT(rdbFrameCodecFromRdbCodec(RDBC_LZF) == RDB_FR_CODEC_LZF);
    TEST_ASSERT(rdbFrameCodecFromRdbCodec(-1) == -1);

    TEST_ASSERT(rdbFrameCodecToRdbCodec(RDB_FR_CODEC_RAW) == RDBC_RAW);
    TEST_ASSERT(rdbFrameCodecToRdbCodec(RDB_FR_CODEC_LZ4) == RDBC_LZ4);
    TEST_ASSERT(rdbFrameCodecToRdbCodec(RDB_FR_CODEC_LZF) == RDBC_LZF);
    TEST_ASSERT(rdbFrameCodecToRdbCodec(-1) == -1);
    return 0;
}

int test_rdb_frame_has_magic_prefix(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char good[] = {RDB_FR_MAGIC0, RDB_FR_MAGIC1, RDB_FR_MAGIC2, RDB_FR_MAGIC3};
    unsigned char bad_version[] = {RDB_FR_MAGIC0, RDB_FR_MAGIC1, RDB_FR_MAGIC2, (unsigned char)(RDB_FR_MAGIC3 + 1)};
    unsigned char short_buf[] = {RDB_FR_MAGIC0, RDB_FR_MAGIC1, RDB_FR_MAGIC2};

    TEST_ASSERT(rdbFrameHasMagicPrefix(good, sizeof(good)));
    TEST_ASSERT(!rdbFrameHasMagicPrefix(bad_version, sizeof(bad_version)));
    TEST_ASSERT(!rdbFrameHasMagicPrefix(short_buf, sizeof(short_buf)));
    TEST_ASSERT(!rdbFrameHasMagicPrefix(NULL, 0));
    return 0;
}

int test_rdb_frame_parse_triplet_whitespace(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char line[] = " checksum=none  blk=65536 codec=lzf ";
    const char *codec = NULL;
    const char *blk = NULL;
    const char *checksum = NULL;
    TEST_ASSERT(rdbFrameParseConfigTriplet(line, &codec, &blk, &checksum) == RDB_FRAME_PARSE_OK);
    TEST_ASSERT(codec && blk && checksum);
    TEST_ASSERT(strcmp(codec, "lzf") == 0);
    TEST_ASSERT(strcmp(blk, "65536") == 0);
    TEST_ASSERT(strcmp(checksum, "none") == 0);
    return 0;
}

int test_rdb_frame_parse_triplet_empty_values(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *codec = NULL;
    const char *blk = NULL;
    const char *checksum = NULL;

    char empty_codec[] = "codec= blk=1 checksum=crc64";
    TEST_ASSERT(rdbFrameParseConfigTriplet(empty_codec, &codec, &blk, &checksum) == RDB_FRAME_PARSE_INVALID_FORMAT);

    codec = blk = checksum = NULL;
    char empty_blk[] = "codec=raw blk= checksum=crc64";
    TEST_ASSERT(rdbFrameParseConfigTriplet(empty_blk, &codec, &blk, &checksum) == RDB_FRAME_PARSE_INVALID_FORMAT);

    codec = blk = checksum = NULL;
    char empty_checksum[] = "codec=lz4 blk=65536 checksum=";
    TEST_ASSERT(rdbFrameParseConfigTriplet(empty_checksum, &codec, &blk, &checksum) == RDB_FRAME_PARSE_INVALID_FORMAT);
    return 0;
}

int test_rdb_frame_parse_triplet_null_args(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *codec = NULL;
    const char *blk = NULL;
    const char *checksum = NULL;

    TEST_ASSERT(rdbFrameParseConfigTriplet(NULL, &codec, &blk, &checksum) == RDB_FRAME_PARSE_INVALID_FORMAT);

    char base_codec[] = "codec=raw blk=65536 checksum=crc64";
    TEST_ASSERT(rdbFrameParseConfigTriplet(base_codec, NULL, &blk, &checksum) == RDB_FRAME_PARSE_INVALID_FORMAT);

    codec = blk = checksum = NULL;
    char base_blk[] = "codec=raw blk=65536 checksum=crc64";
    TEST_ASSERT(rdbFrameParseConfigTriplet(base_blk, &codec, NULL, &checksum) == RDB_FRAME_PARSE_INVALID_FORMAT);

    codec = blk = checksum = NULL;
    char base_checksum[] = "codec=raw blk=65536 checksum=crc64";
    TEST_ASSERT(rdbFrameParseConfigTriplet(base_checksum, &codec, &blk, NULL) == RDB_FRAME_PARSE_INVALID_FORMAT);
    return 0;
}

int test_rdb_frame_codec_defaults(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    TEST_ASSERT(rdbFrameCodecFromRdbCodecOrDefault(RDBC_RAW, RDB_FR_CODEC_LZ4) == RDB_FR_CODEC_RAW);
    TEST_ASSERT(rdbFrameCodecFromRdbCodecOrDefault(RDBC_LZ4, RDB_FR_CODEC_RAW) == RDB_FR_CODEC_LZ4);
    TEST_ASSERT(rdbFrameCodecFromRdbCodecOrDefault(RDBC_LZF, RDB_FR_CODEC_RAW) == RDB_FR_CODEC_LZF);
    TEST_ASSERT(rdbFrameCodecFromRdbCodecOrDefault(-1, RDB_FR_CODEC_LZF) == RDB_FR_CODEC_LZF);
    TEST_ASSERT(rdbFrameCodecFromRdbCodecOrDefault(-1, -1) == RDB_FR_CODEC_RAW);
    return 0;
}

int test_rdb_frame_checksum_defaults(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    TEST_ASSERT(rdbFrameChecksumOrDefault(RDB_FR_CHECKSUM_CRC64, RDB_FR_CHECKSUM_NONE) == RDB_FR_CHECKSUM_CRC64);
    TEST_ASSERT(rdbFrameChecksumOrDefault(RDB_FR_CHECKSUM_NONE, RDB_FR_CHECKSUM_CRC64) == RDB_FR_CHECKSUM_NONE);
    TEST_ASSERT(rdbFrameChecksumOrDefault(-1, RDB_FR_CHECKSUM_NONE) == RDB_FR_CHECKSUM_NONE);
    TEST_ASSERT(rdbFrameChecksumOrDefault(-1, -1) == RDB_FR_CHECKSUM_CRC64);
    return 0;
}

int test_rdb_frame_block_defaults(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    TEST_ASSERT(rdbFrameBlockSizeOrDefault(131072, 262144, 65536) == 131072);
    TEST_ASSERT(rdbFrameBlockSizeOrDefault(0, 262144, 65536) == 262144);
    TEST_ASSERT(rdbFrameBlockSizeOrDefault(32768, 0, 65536) == 65536);
    TEST_ASSERT(rdbFrameBlockSizeOrDefault(0, 0, 65536) == 65536);
    TEST_ASSERT(rdbFrameBlockSizeOrDefault(0, 131072, 0) == 131072);
    return 0;
}
