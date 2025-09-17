#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../crc64.h"
#include "../endianconv.h"
#include "../rdb_codec.h"
#include "../rdb_frame.h"
#include "../rio.h"
#include "../rio_compress.h"
#include "../rio_decompress.h"
#include "../server.h"
#include "../zmalloc.h"

#include "test_help.h"

int test_rio_decompress(int argc, char **argv, int flags) {
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

    const char *extra1 = "valkey decompression block test data";
    const char *extra2 = "the quick brown fox jumps over the lazy dog";
    TEST_ASSERT(rioWrite(wr, extra1, strlen(extra1)));
    expected = sdscatlen(expected, extra1, strlen(extra1));
    TEST_ASSERT(expected != NULL);
    TEST_ASSERT(rioWrite(wr, extra2, strlen(extra2)));
    expected = sdscatlen(expected, extra2, strlen(extra2));
    TEST_ASSERT(expected != NULL);

    TEST_ASSERT(rioCompressFlush(&rc, 1) == C_OK);
    TEST_ASSERT(rioFlush(rc.dst));

    size_t expected_len = sdslen(expected);
    TEST_ASSERT(expected_len > 0);

    TEST_ASSERT(fseeko(fp, 0, SEEK_SET) == 0);

    rio src;
    rioInitWithFile(&src, fp);

    rio_decompress rd;
    TEST_ASSERT(rioInitDecompress(&rd, &src) == C_OK);
    rio *reader = &rd.rio_itf;

    unsigned char *decoded = zmalloc(expected_len);
    TEST_ASSERT(decoded != NULL);

    size_t chunk_sizes[] = {1, 7, 4096, 65536, 3, 128};
    size_t consumed = 0;
    size_t idx = 0;
    while (consumed < expected_len) {
        size_t chunk = chunk_sizes[idx % (sizeof(chunk_sizes) / sizeof(chunk_sizes[0]))];
        if (chunk > expected_len - consumed) chunk = expected_len - consumed;
        TEST_ASSERT(rioRead(reader, decoded + consumed, chunk));
        consumed += chunk;
        idx++;
    }

    TEST_ASSERT(memcmp(decoded, expected, expected_len) == 0);
    TEST_ASSERT(rioTell(reader) == (off_t)expected_len);
    TEST_ASSERT(rd.eof);
    TEST_ASSERT(rd.pos == sdslen(rd.rawbuf));

    unsigned char extra;
    TEST_ASSERT(rioRead(reader, &extra, 1) == 0);
    TEST_ASSERT(rioGetReadError(reader));

    zfree(decoded);
    sdsfree(expected);
    zfree(random);
    rdbCodecFree(rc.cctx);
    sdsfree(rc.rawbuf);
    sdsfree(rc.cmpbuf);
    sdsfree(rd.rawbuf);
    fclose(fp);

    return 0;
}
