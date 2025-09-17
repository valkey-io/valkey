/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <stdint.h>

#include "../../src/unit/test_help.h"
#include "../../src/rdb_codec.h"

#ifndef C_OK
#define C_OK 0
#endif
#ifndef C_ERR
#define C_ERR -1
#endif

#define PATTERN_COUNT 10

static sds createRepeatingPattern(const char *seed, size_t repeat) {
    size_t seed_len = strlen(seed);
    size_t total = seed_len * repeat;
    sds s = sdsnewlen(SDS_NOINIT, total);
    char *ptr = s;
    for (size_t i = 0; i < repeat; i++) {
        memcpy(ptr + (i * seed_len), seed, seed_len);
    }
    ptr[total] = '\0';
    return s;
}

static void buildTestPatterns(sds patterns[PATTERN_COUNT]) {
    patterns[0] = sdsempty();
    patterns[1] = sdsnew("short string for codec tests");

    patterns[2] = sdsnewlen(SDS_NOINIT, 256);
    memset(patterns[2], 'a', sdslen(patterns[2]));
    patterns[2][sdslen(patterns[2])] = '\0';

    patterns[3] = createRepeatingPattern("0123456789abcdef", 64);

    patterns[4] = sdsnewlen(SDS_NOINIT, 512);
    for (size_t i = 0; i < sdslen(patterns[4]); i++) {
        patterns[4][i] = (char)('A' + (i % 26));
    }
    patterns[4][sdslen(patterns[4])] = '\0';

    patterns[5] = createRepeatingPattern("The quick brown fox jumps over the lazy dog. ", 40);

    patterns[6] = sdsnewlen(SDS_NOINIT, 2048);
    memset(patterns[6], 0, sdslen(patterns[6]));
    patterns[6][sdslen(patterns[6])] = '\0';

    patterns[7] = sdsnewlen(SDS_NOINIT, 1536);
    for (size_t i = 0; i < sdslen(patterns[7]); i++) {
        patterns[7][i] = (char)((i * 7) & 0xff);
    }
    patterns[7][sdslen(patterns[7])] = '\0';

    patterns[8] = createRepeatingPattern("VALKEY", 300);

    patterns[9] = sdsnewlen(SDS_NOINIT, 1024);
    for (size_t i = 0; i < sdslen(patterns[9]); i++) {
        patterns[9][i] = (char)((i * 17 + 3) & 0xff);
    }
    patterns[9][sdslen(patterns[9])] = '\0';
}

int test_rdb_codec_roundtrip(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    sds patterns[PATTERN_COUNT];
    buildTestPatterns(patterns);

    rdb_codec_ctx *raw = rdbCodecCreate(RDBC_RAW);
    rdb_codec_ctx *lz4 = rdbCodecCreate(RDBC_LZ4);
    rdb_codec_ctx *lzf = rdbCodecCreate(RDBC_LZF);
    TEST_ASSERT(raw && lz4 && lzf);

    rdb_codec_ctx *contexts[] = {raw, lz4, lzf};
    sds compressed = NULL;
    sds plain = NULL;

    for (size_t pattern_index = 0; pattern_index < PATTERN_COUNT; pattern_index++) {
        sds pattern = patterns[pattern_index];
        size_t pattern_len = sdslen(pattern);
        for (size_t codec_index = 0; codec_index < sizeof(contexts) / sizeof(contexts[0]); codec_index++) {
            rdb_codec_ctx *ctx = contexts[codec_index];
            rdb_codec_t actual = RDBC_RAW;
            TEST_ASSERT(rdbCodecCompress(ctx, pattern, pattern_len, &compressed, &actual) == C_OK);
            TEST_ASSERT(compressed != NULL);

            TEST_ASSERT(rdbCodecDecompress(actual, compressed, sdslen(compressed), &plain) == C_OK);
            TEST_ASSERT(sdslen(plain) == pattern_len);
            if (pattern_len > 0) {
                TEST_ASSERT(memcmp(plain, pattern, pattern_len) == 0);
            }
        }
    }

    if (compressed) sdsfree(compressed);
    if (plain) sdsfree(plain);
    rdbCodecFree(raw);
    rdbCodecFree(lz4);
    rdbCodecFree(lzf);
    for (size_t i = 0; i < PATTERN_COUNT; i++) {
        sdsfree(patterns[i]);
    }
    return 0;
}

int test_rdb_codec_raw_fallback(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    rdb_codec_ctx *lz4 = rdbCodecCreate(RDBC_LZ4);
    rdb_codec_ctx *lzf = rdbCodecCreate(RDBC_LZF);
    TEST_ASSERT(lz4 && lzf);

    sds compressed = NULL;
    sds plain = NULL;
    const size_t len = 1024;
    sds pattern = sdsnewlen(SDS_NOINIT, len);
    rdb_codec_ctx *contexts[] = {lz4, lzf};

    for (size_t codec_index = 0; codec_index < sizeof(contexts) / sizeof(contexts[0]); codec_index++) {
        rdb_codec_ctx *ctx = contexts[codec_index];
        int fallback_triggered = 0;
        uint64_t seed = 0x123456789abcdef0ULL + codec_index * 0x9e3779b97f4a7c15ULL;
        for (size_t attempt = 0; attempt < 32; attempt++) {
            uint64_t state = seed + attempt * 0xd1342543de82ef95ULL;
            for (size_t i = 0; i < len; i++) {
                state = state * 6364136223846793005ULL + 1;
                pattern[i] = (char)(state >> 32);
            }
            pattern[len] = '\0';

            rdb_codec_t actual = RDBC_RAW;
            TEST_ASSERT(rdbCodecCompress(ctx, pattern, len, &compressed, &actual) == C_OK);
            if (actual == RDBC_RAW) {
                TEST_ASSERT(rdbCodecDecompress(actual, compressed, sdslen(compressed), &plain) == C_OK);
                TEST_ASSERT(sdslen(plain) == len);
                TEST_ASSERT(memcmp(plain, pattern, len) == 0);
                fallback_triggered = 1;
                break;
            }
        }
        TEST_ASSERT(fallback_triggered);
    }

    if (compressed) sdsfree(compressed);
    if (plain) sdsfree(plain);
    sdsfree(pattern);
    rdbCodecFree(lz4);
    rdbCodecFree(lzf);
    return 0;
}
