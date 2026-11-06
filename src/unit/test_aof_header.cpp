/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "crc64.h"
#include "server.h"
}

static int parseHeader(const char *line, aofHeader *hdr) {
    const char *err = NULL;
    return aofParseHeader(line, strlen(line), hdr, &err);
}

TEST(AofHeaderTest, ParsesHeader) {
    aofHeader hdr;
    ASSERT_EQ(parseHeader("#HDR:v1;len:45;checksum:1234;\r\n", &hdr), C_OK);
    ASSERT_EQ(hdr.len, 45);
    ASSERT_EQ(hdr.checksum, 1234u);
    ASSERT_TRUE(hdr.has_checksum);
    ASSERT_EQ(hdr.checksum_start, strlen("#HDR:v1;len:45;checksum:"));
    ASSERT_EQ(hdr.checksum_end, strlen("#HDR:v1;len:45;checksum:1234"));
}

TEST(AofHeaderTest, ParsesFullRangeChecksum) {
    aofHeader hdr;
    ASSERT_EQ(parseHeader("#HDR:v1;len:0;checksum:18446744073709551615;\r\n", &hdr), C_OK);
    ASSERT_EQ(hdr.len, 0);
    ASSERT_EQ(hdr.checksum, UINT64_MAX);
}

TEST(AofHeaderTest, IgnoresUnknownFields) {
    aofHeader hdr;
    ASSERT_EQ(parseHeader("#HDR:v1;len:45;replid:abc;reploff:7;maxlen:5;checksum:9;\r\n", &hdr), C_OK);
    ASSERT_EQ(hdr.len, 45);
    ASSERT_EQ(hdr.checksum, 9u);
}

TEST(AofHeaderTest, ParsesFieldsInAnyOrder) {
    aofHeader hdr;
    ASSERT_EQ(parseHeader("#HDR:v1;foo:1;checksum:9;bar:2;len:45;\r\n", &hdr), C_OK);
    ASSERT_EQ(hdr.len, 45);
    ASSERT_EQ(hdr.checksum, 9u);
}

TEST(AofHeaderTest, ChecksumsHeaderWithoutItsValue) {
    crc64_init();
    const char *line = "#HDR:v1;len:45;checksum:9;foo:1;\r\n";
    const char *without_value = "#HDR:v1;len:45;checksum:;foo:1;\r\n";
    aofHeader hdr;
    ASSERT_EQ(parseHeader(line, &hdr), C_OK);
    ASSERT_EQ(aofChecksumHeader(7, line, strlen(line), &hdr),
              crc64(7, (const unsigned char *)without_value, strlen(without_value)));
}

TEST(AofHeaderTest, ChecksumIsOptional) {
    aofHeader hdr;
    ASSERT_EQ(parseHeader("#HDR:v1;len:45;reploff:7;\r\n", &hdr), C_OK);
    ASSERT_EQ(hdr.len, 45);
    ASSERT_FALSE(hdr.has_checksum);
}

TEST(AofHeaderTest, RejectsMalformedHeaders) {
    const char *invalid[] = {
        "#HDR:v1;len:45;checksum:1;",                        /* No CRLF. */
        "#HDR:v1;len:45;checksum:1;\n",                      /* No CR. */
        "#HDR:v2;len:45;checksum:1;\r\n",                    /* Unsupported version. */
        "#HDR:v1;checksum:1;\r\n",                           /* No len. */
        "#HDR:v1;len:45;len:46;checksum:1;\r\n",             /* Duplicate len. */
        "#HDR:v1;len:45;checksum:1;checksum:2;\r\n",         /* Duplicate checksum. */
        "#HDR:v1;len:abc;checksum:1;\r\n",                   /* Not a number. */
        "#HDR:v1;len:-1;checksum:1;\r\n",                    /* Negative. */
        "#HDR:v1;len:;checksum:1;\r\n",                      /* Empty. */
        "#HDR:v1;len:99999999999999999999;checksum:1;\r\n",  /* Overflow. */
        "#HDR:v1;len:45;foo;checksum:1;\r\n",                /* Field without a value. */
        "#HDR:v1;len:45;:1;checksum:1;\r\n",                 /* Field without a key. */
        "#HDR:v1;len:45;checksum:1\r\n",                     /* Unterminated field. */
        "#HDR:v1;len:45;checksum:5abc;\r\n",                 /* Checksum with trailing garbage. */
        "#HDR:v1;len:45;checksum:+5;\r\n",                   /* Checksum with a sign. */
        "#HDR:v1;len:45;checksum:;\r\n",                     /* Empty checksum. */
        "#HDR:v1;len:45;checksum:18446744073709551616;\r\n", /* Checksum overflow. */
        "#HDR:v1;\r\n",                                      /* No fields. */
        "\r\n",                                              /* Only CRLF. */
        "#HDR:v1;len:45;fo o:1;checksum:1;\r\n",             /* Space in a key. */
        "#HDR:v1;len:45;foo:a\tb;checksum:1;\r\n",           /* Control character in a value. */
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        aofHeader hdr;
        ASSERT_EQ(parseHeader(invalid[i], &hdr), C_ERR) << invalid[i];
    }
}

TEST(AofHeaderTest, ParsesUint64) {
    uint64_t v;
    ASSERT_EQ(aofParseUint64("0", 1, &v), 1);
    ASSERT_EQ(v, 0u);
    ASSERT_EQ(aofParseUint64("18446744073709551615", 20, &v), 1);
    ASSERT_EQ(v, UINT64_MAX);
    /* Only the given length is parsed. */
    ASSERT_EQ(aofParseUint64("12;", 2, &v), 1);
    ASSERT_EQ(v, 12u);
    ASSERT_EQ(aofParseUint64("", 0, &v), 0);
    ASSERT_EQ(aofParseUint64("18446744073709551616", 20, &v), 0);
    ASSERT_EQ(aofParseUint64("-1", 2, &v), 0);
    ASSERT_EQ(aofParseUint64(" 1", 2, &v), 0);
}

/* Builds a block the way flushAppendOnlyFile() does, then parses and verifies it. */
TEST(AofHeaderTest, VerifiesABlockBuiltLikeTheWriter) {
    crc64_init();
    const char *payload = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
    uint64_t seed = 12345;
    char prefix[64], line[128];
    snprintf(prefix, sizeof(prefix), "#HDR:v1;len:%zu;checksum:", strlen(payload));
    uint64_t checksum = crc64(seed, (unsigned char *)prefix, strlen(prefix));
    checksum = crc64(checksum, (unsigned char *)";\r\n", 3);
    checksum = crc64(checksum, (unsigned char *)payload, strlen(payload));
    snprintf(line, sizeof(line), "%s%llu;\r\n", prefix, (unsigned long long)checksum);

    aofHeader hdr;
    ASSERT_EQ(parseHeader(line, &hdr), C_OK);
    ASSERT_EQ(hdr.len, (long long)strlen(payload));
    ASSERT_EQ(hdr.checksum, checksum);

    FILE *fp = tmpfile();
    ASSERT_NE(fp, nullptr);
    fputs(payload, fp);
    rewind(fp);
    uint64_t computed = aofChecksumHeader(seed, line, strlen(line), &hdr);
    ASSERT_EQ(aofChecksumBlock(fp, hdr.len, NULL, &computed), C_OK);
    ASSERT_EQ(computed, hdr.checksum);
    fclose(fp);
}

TEST(AofHeaderTest, ChecksumBlock) {
    crc64_init();
    FILE *fp = tmpfile();
    ASSERT_NE(fp, nullptr);
    fputs("abcdef", fp);

    /* An empty block leaves the checksum unchanged. */
    rewind(fp);
    uint64_t crc = 7;
    ASSERT_EQ(aofChecksumBlock(fp, 0, NULL, &crc), C_OK);
    ASSERT_EQ(crc, 7u);

    /* Reading in chunks and into a buffer give the same checksum. */
    char buf[16];
    uint64_t crc_chunks = 0, crc_buf = 0;
    rewind(fp);
    ASSERT_EQ(aofChecksumBlock(fp, 6, NULL, &crc_chunks), C_OK);
    rewind(fp);
    ASSERT_EQ(aofChecksumBlock(fp, 6, buf, &crc_buf), C_OK);
    ASSERT_EQ(crc_chunks, crc_buf);
    ASSERT_EQ(crc_chunks, crc64(0, (unsigned char *)"abcdef", 6));
    ASSERT_EQ(memcmp(buf, "abcdef", 6), 0);

    /* A block longer than the rest of the file is a short read. */
    rewind(fp);
    crc = 0;
    ASSERT_EQ(aofChecksumBlock(fp, 7, NULL, &crc), C_ERR);
    rewind(fp);
    ASSERT_EQ(aofChecksumBlock(fp, 7, buf, &crc), C_ERR);
    fclose(fp);
}
