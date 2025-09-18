#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../crc64.h"
#include "../endianconv.h"
#include "../rdb_codec.h"
#include "../rdb_frame.h"
#include "../rio.h"
#include "../rio_compress.h"
#include "../server.h"
#include "../zmalloc.h"

#include "test_help.h"

int test_rio_compress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    crc64_init();
    srand(1);

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL);

    rio dst;
    rioInitWithFile(&dst, fp);
    dst.update_cksum = rioGenericUpdateChecksum;

    rdb_frame_opts opts = {0};
    opts.codec = RDB_FR_CODEC_LZ4;
    opts.block_bytes = 64 * 1024;
    opts.checksum = RDB_FR_CHECKSUM_CRC64;

    rio_compress rc;
    TEST_ASSERT(rioInitCompress(&rc, &dst, &opts) == C_OK);
    rio *wr = &rc.rio_itf;

    size_t random_len = 1024 * 1024;
    unsigned char *random = zmalloc(random_len);
    TEST_ASSERT(random != NULL);
    for (size_t i = 0; i < random_len; i++) random[i] = (unsigned char)(rand() & 0xFF);

    sds expected = sdsnewlen(random, random_len);
    TEST_ASSERT(expected != NULL);
    TEST_ASSERT(rioWrite(wr, random, random_len));

    const char *extra1 = "valkey compression block test data 1";
    const char *extra2 = "another block of textual data to cross boundaries and flush";
    TEST_ASSERT(rioWrite(wr, extra1, strlen(extra1)));
    expected = sdscatlen(expected, extra1, strlen(extra1));
    TEST_ASSERT(expected != NULL);
    TEST_ASSERT(rioWrite(wr, extra2, strlen(extra2)));
    expected = sdscatlen(expected, extra2, strlen(extra2));
    TEST_ASSERT(expected != NULL);

    TEST_ASSERT(rioCompressFlush(&rc, 1) == C_OK);
    TEST_ASSERT(rioFlush(rc.dst));
    TEST_ASSERT(rc.blocks > 0);

    off_t written = ftello(fp);
    TEST_ASSERT(written > 0);
    TEST_ASSERT(fseeko(fp, 0, SEEK_SET) == 0);

    sds chunk = sdsempty();
    TEST_ASSERT(chunk != NULL);

    size_t total_raw = 0;
    int seen_last = 0;
    int block_index = 0;

    while ((off_t)ftello(fp) < written) {
        unsigned char hdrbuf[sizeof(RdbFrameBlockHdr)];
        size_t read = fread(hdrbuf, sizeof(hdrbuf), 1, fp);
        TEST_ASSERT(read == 1);

        RdbFrameBlockHdr hdr;
        memcpy(&hdr, hdrbuf, sizeof(hdr));
        TEST_ASSERT(hdr.magic[0] == RDB_FR_MAGIC0);
        TEST_ASSERT(hdr.magic[1] == RDB_FR_MAGIC1);
        TEST_ASSERT(hdr.magic[2] == RDB_FR_MAGIC2);
        TEST_ASSERT(hdr.magic[3] == RDB_FR_MAGIC3);

        uint32_t raw_len = hdr.raw_len_le;
        uint32_t cmp_len = hdr.cmp_len_le;
        memrev32ifbe(&raw_len);
        memrev32ifbe(&cmp_len);

        uint64_t stored_crc = hdr.crc64_le;
        memrev64ifbe(&stored_crc);

        unsigned char *payload = NULL;
        if (cmp_len > 0) {
            payload = zmalloc(cmp_len);
            TEST_ASSERT(payload != NULL);
            TEST_ASSERT(fread(payload, cmp_len, 1, fp) == 1);
        }

        if (opts.checksum == RDB_FR_CHECKSUM_CRC64) {
            uint64_t crc = crc64(0, hdrbuf, offsetof(RdbFrameBlockHdr, crc64_le));
            if (cmp_len > 0) crc = crc64(crc, payload, cmp_len);
            TEST_ASSERT(crc == stored_crc);
        } else {
            TEST_ASSERT(stored_crc == 0);
        }

        rdb_codec_t block_codec;
        switch (hdr.codec) {
        case RDB_FR_CODEC_RAW:
            block_codec = RDBC_RAW;
            break;
        case RDB_FR_CODEC_LZ4:
            block_codec = RDBC_LZ4;
            break;
        case RDB_FR_CODEC_LZF:
            block_codec = RDBC_LZF;
            break;
        default:
            TEST_ASSERT_MESSAGE("Unsupported codec in frame header", 0);
            block_codec = RDBC_RAW;
            break;
        }

        const unsigned char *payload_bytes = payload ? payload : (unsigned char *)"";
        if (block_codec == RDBC_RAW) {
            TEST_ASSERT(raw_len == cmp_len);
            TEST_ASSERT(memcmp(expected + total_raw, payload_bytes, raw_len) == 0);
        } else {
            TEST_ASSERT(rdbCodecDecompress(block_codec, payload, cmp_len, &chunk) == C_OK);
            TEST_ASSERT((size_t)raw_len == sdslen(chunk));
            TEST_ASSERT(memcmp(expected + total_raw, chunk, raw_len) == 0);
        }

        total_raw += raw_len;
        block_index++;

        if (hdr.flags & RDB_FR_FLAG_LAST) {
            TEST_ASSERT(!seen_last);
            seen_last = 1;
            TEST_ASSERT(ftello(fp) == written);
        } else {
            TEST_ASSERT(!seen_last);
        }

        if (payload) zfree(payload);
    }

    TEST_ASSERT(seen_last);
    TEST_ASSERT(total_raw == sdslen(expected));
    TEST_ASSERT(block_index == (int)rc.blocks);

    sdsfree(chunk);
    sdsfree(expected);
    zfree(random);
    rdbCodecFree(rc.cctx);
    sdsfree(rc.rawbuf);
    sdsfree(rc.cmpbuf);
    fclose(fp);

    return 0;
}
