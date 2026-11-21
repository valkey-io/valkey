/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdlib>
#include <cstring>

extern "C" {
#include "server.h"
}

/* The tests compare each bitmap with a reference: a plain buffer holding the
 * bytes of the equivalent string, where bit i is bit (7 - i % 8) of byte
 * i / 8, as for SETBIT. */

static int refGetBit(const unsigned char *ref, size_t len, uint64_t i) {
    if (i / 8 >= len) return 0;
    return (ref[i / 8] >> (7 - i % 8)) & 1;
}

static void refSetBit(unsigned char *ref, uint64_t i, int on) {
    if (on)
        ref[i / 8] |= (unsigned char)(1 << (7 - i % 8));
    else
        ref[i / 8] &= (unsigned char)~(1 << (7 - i % 8));
}

static uint64_t refCount(const unsigned char *ref, size_t len, uint64_t first, uint64_t last) {
    uint64_t n = 0;
    for (uint64_t i = first; i <= last && i / 8 < len; i++) n += refGetBit(ref, len, i);
    return n;
}

static long long refFirst(const unsigned char *ref, size_t len, int bit, uint64_t first, uint64_t last) {
    for (uint64_t i = first; i <= last && i / 8 < len; i++) {
        if (refGetBit(ref, len, i) == bit) return (long long)i;
    }
    return -1;
}

/* Fills 'buf' with random bytes of about 'density' set bits, with runs of
 * ones when 'runs' is set, so that the bitmaps get array, bitset and run
 * containers. */
static void fillRandom(unsigned char *buf, size_t len, double density, int runs) {
    memset(buf, 0, len);
    for (uint64_t i = 0; i < (uint64_t)len * 8; i++) {
        if (runs && random() % 1000 == 0) {
            uint64_t end = i + (uint64_t)(random() % 100000);
            for (; i < end && i < (uint64_t)len * 8; i++) refSetBit(buf, i, 1);
        } else if ((double)random() / RAND_MAX < density) {
            refSetBit(buf, i, 1);
        }
    }
}

static void expectSameBytes(robj *bitmap, const unsigned char *ref, size_t len) {
    ASSERT_EQ(bitmapRoaringLen(bitmap), len);
    unsigned char *bytes = (unsigned char *)zmalloc(len + 1);
    bitmapRoaringReadBytes(bitmap, 0, len, bytes);
    EXPECT_EQ(memcmp(bytes, ref, len), 0);
    zfree(bytes);
}

class BitmapRoaringTest : public ::testing::Test {
  protected:
    void SetUp() override {
        bitmapRoaringInit();
        srandom(1234);
    }
};

/* Converting a string to a bitmap and back gives the same bytes, for lengths
 * around the 8 KB chunks of Roaring bitmaps and every kind of container. */
TEST_F(BitmapRoaringTest, StringRoundTrip) {
    size_t lengths[] = {0, 1, 7, 8191, 8192, 8193, 3 * 8192 + 5, 100000};
    double densities[] = {0.0, 0.001, 0.05, 0.5, 1.0};
    for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
        for (size_t d = 0; d < sizeof(densities) / sizeof(densities[0]); d++) {
            for (int runs = 0; runs <= 1; runs++) {
                size_t len = lengths[l];
                unsigned char *ref = (unsigned char *)zmalloc(len + 1);
                fillRandom(ref, len, densities[d], runs);
                robj *s = createStringObject((char *)ref, len);
                robj *b = bitmapRoaringObjectFromString(s);
                ASSERT_NE(b, nullptr);
                expectSameBytes(b, ref, len);
                EXPECT_EQ(bitmapRoaringCardinality(b), refCount(ref, len, 0, (uint64_t)len * 8));
                robj *back = bitmapRoaringToStringObject(b);
                ASSERT_EQ(stringObjectLen(back), len);
                EXPECT_EQ(memcmp(objectGetVal(back), ref, len), 0);
                decrRefCount(back);
                decrRefCount(b);
                decrRefCount(s);
                zfree(ref);
            }
        }
    }
}

/* An integer-encoded string converts as its decimal digits. */
TEST_F(BitmapRoaringTest, IntegerEncodedString) {
    robj *s = createStringObjectFromLongLong(12345);
    robj *b = bitmapRoaringObjectFromString(s);
    ASSERT_NE(b, nullptr);
    expectSameBytes(b, (const unsigned char *)"12345", 5);
    decrRefCount(b);
    decrRefCount(s);
}

/* SETBIT, GETBIT, BITCOUNT and BITPOS over random offsets and ranges agree
 * with the reference. */
TEST_F(BitmapRoaringTest, BitAccessMatchesBytes) {
    size_t len = 300000;
    unsigned char *ref = (unsigned char *)zcalloc(len);
    robj *b = createBitmapRoaringObject(0);
    size_t cur = 0;
    for (int j = 0; j < 20000; j++) {
        uint64_t i = (uint64_t)(random() % (len * 8));
        /* Clustered writes make full chunks, which become bitsets. */
        if (j % 4 == 0) i = 65536 * 3 + (uint64_t)(random() % 70000);
        if (i / 8 >= cur) {
            cur = i / 8 + 1;
            bitmapRoaringGrow(b, cur);
        }
        int on = random() % 4 != 0;
        int old = refGetBit(ref, cur, i);
        EXPECT_EQ(bitmapRoaringSetBit(b, i, on), old);
        refSetBit(ref, i, on);
        EXPECT_EQ(bitmapRoaringGetBit(b, i), on);
    }
    expectSameBytes(b, ref, cur);
    for (int j = 0; j < 200; j++) {
        uint64_t first = (uint64_t)(random() % (cur * 8));
        uint64_t last = first + (uint64_t)(random() % 200000);
        EXPECT_EQ(bitmapRoaringCount(b, first, last), refCount(ref, cur, first, last));
        for (int bit = 0; bit <= 1; bit++) {
            uint64_t end = last < cur * 8 ? last : cur * 8 - 1;
            EXPECT_EQ(bitmapRoaringFirst(b, bit, first, end), refFirst(ref, cur, bit, first, end));
        }
    }
    decrRefCount(b);
    zfree(ref);
}

/* BITFIELD writes up to 9 bytes at a time with bitmapRoaringWriteBytes. */
TEST_F(BitmapRoaringTest, WriteBytesMatchesBytes) {
    size_t len = 200000;
    unsigned char *ref = (unsigned char *)zmalloc(len);
    fillRandom(ref, len, 0.01, 1);
    robj *s = createStringObject((char *)ref, len);
    robj *b = bitmapRoaringObjectFromString(s);
    decrRefCount(s);
    for (int j = 0; j < 5000; j++) {
        size_t count = (size_t)(random() % 10);
        size_t start = (size_t)(random() % (len - count));
        unsigned char src[9];
        for (size_t k = 0; k < count; k++) src[k] = (unsigned char)(random() % 3 == 0 ? random() : 0);
        bitmapRoaringWriteBytes(b, start, count, src);
        memcpy(ref + start, src, count);
    }
    expectSameBytes(b, ref, len);
    decrRefCount(b);
    zfree(ref);
}

/* The RDB payload keeps the bits; loading rejects a length too short for
 * them, truncated payloads and trailing bytes. */
TEST_F(BitmapRoaringTest, SerializeRoundTripAndRejects) {
    size_t len = 50000;
    unsigned char *ref = (unsigned char *)zmalloc(len);
    fillRandom(ref, len, 0.02, 1);
    ref[len - 1] = 0; /* the length extends past the last set bit */
    robj *s = createStringObject((char *)ref, len);
    robj *b = bitmapRoaringObjectFromString(s);
    decrRefCount(s);
    sds payload = bitmapRoaringSerialize(b);
    robj *c = bitmapRoaringDeserialize(payload, sdslen(payload), len);
    ASSERT_NE(c, nullptr);
    expectSameBytes(c, ref, len);
    decrRefCount(c);
    EXPECT_EQ(bitmapRoaringDeserialize(payload, sdslen(payload), 10), nullptr);
    EXPECT_EQ(bitmapRoaringDeserialize(payload, sdslen(payload) - 1, len), nullptr);
    sds longer = sdscatlen(sdsdup(payload), "x", 1);
    EXPECT_EQ(bitmapRoaringDeserialize(longer, sdslen(longer), len), nullptr);
    EXPECT_EQ(bitmapRoaringDeserialize(payload, sdslen(payload), BITMAP_ROARING_MAX_BYTES + 1), nullptr);
    sdsfree(longer);
    sdsfree(payload);
    decrRefCount(b);
    zfree(ref);
}

/* Random corruptions of a valid payload are either rejected or give a valid
 * bitmap; they must never crash (run under ASAN for full effect). */
TEST_F(BitmapRoaringTest, ImportCorruptedPayloads) {
    size_t len = 30000;
    unsigned char *ref = (unsigned char *)zmalloc(len);
    fillRandom(ref, len, 0.05, 1);
    robj *s = createStringObject((char *)ref, len);
    sds good = bitmapRoaringExport(s);
    decrRefCount(s);
    ASSERT_NE(good, nullptr);
    robj *b = bitmapRoaringImport(good, sdslen(good));
    ASSERT_NE(b, nullptr);
    decrRefCount(b);
    EXPECT_EQ(bitmapRoaringImport(good, 0), nullptr);
    EXPECT_EQ(bitmapRoaringImport("garbage", 7), nullptr);
    for (int j = 0; j < 2000; j++) {
        sds bad = sdsdup(good);
        int flips = 1 + random() % 4;
        for (int k = 0; k < flips; k++) bad[random() % sdslen(bad)] ^= (char)(1 << (random() % 8));
        if (random() % 4 == 0) sdsrange(bad, 0, (ssize_t)(random() % sdslen(bad)) - 1);
        robj *o = bitmapRoaringImport(bad, sdslen(bad));
        if (o != nullptr && bitmapRoaringLen(o) > (1 << 20)) {
            /* A corrupted key can move bits far away: check the counts only. */
            EXPECT_EQ(bitmapRoaringCount(o, 0, (uint64_t)bitmapRoaringLen(o) * 8 - 1), bitmapRoaringCardinality(o));
            decrRefCount(o);
        } else if (o != nullptr) {
            /* Accepted: it must behave like the bytes it converts to. */
            size_t n = bitmapRoaringLen(o);
            unsigned char *bytes = (unsigned char *)zmalloc(n + 1);
            bitmapRoaringReadBytes(o, 0, n, bytes);
            EXPECT_EQ(bitmapRoaringCardinality(o), refCount(bytes, n, 0, (uint64_t)n * 8));
            zfree(bytes);
            decrRefCount(o);
        }
        sdsfree(bad);
    }
    sdsfree(good);
    zfree(ref);
}

/* BITOP on bitmaps, strings and missing inputs agrees with the byte loop. */
TEST_F(BitmapRoaringTest, BitopMatchesBytes) {
    size_t lens[3] = {70000, 140000, 9000};
    unsigned char *refs[3];
    robj *strings[3];
    robj *bitmaps[3];
    for (int k = 0; k < 3; k++) {
        refs[k] = (unsigned char *)zmalloc(lens[k]);
        fillRandom(refs[k], lens[k], k == 1 ? 0.6 : 0.01, k == 2);
        strings[k] = createStringObject((char *)refs[k], lens[k]);
        bitmaps[k] = bitmapRoaringObjectFromString(strings[k]);
    }
    size_t maxlen = 140000;
    unsigned char *expect = (unsigned char *)zmalloc(maxlen);
    for (int op = BITOP_AND; op <= BITOP_NOT; op++) {
        /* AND reads three inputs; with the missing key it would be empty. */
        int n = op == BITOP_NOT ? 1 : op == BITOP_AND ? 3
                                                      : 4;
        size_t outlen = op == BITOP_NOT ? lens[0] : maxlen;
        for (size_t i = 0; i < outlen; i++) {
            unsigned char a = i < lens[0] ? refs[0][i] : 0;
            unsigned char b = i < lens[1] ? refs[1][i] : 0;
            unsigned char c = i < lens[2] ? refs[2][i] : 0;
            switch (op) {
            case BITOP_AND: expect[i] = a & b & c; break;
            case BITOP_OR: expect[i] = a | b | c; break;
            case BITOP_XOR: expect[i] = a ^ b ^ c; break;
            default: expect[i] = (unsigned char)~a; break;
            }
        }
        /* Mixed inputs: a bitmap, a string, a bitmap and a missing key. */
        robj *inputs[4] = {bitmaps[0], strings[1], bitmaps[2], NULL};
        robj *res = bitmapRoaringBitop(op, inputs, (unsigned long)n, outlen);
        expectSameBytes(res, expect, outlen);
        decrRefCount(res);
    }
    /* AND with a missing key: zero bytes of the longest length. */
    robj *with_missing[2] = {bitmaps[1], NULL};
    robj *res = bitmapRoaringBitop(BITOP_AND, with_missing, 2, lens[1]);
    EXPECT_EQ(bitmapRoaringCardinality(res), 0u);
    EXPECT_EQ(bitmapRoaringLen(res), lens[1]);
    decrRefCount(res);
    for (int k = 0; k < 3; k++) {
        decrRefCount(strings[k]);
        decrRefCount(bitmaps[k]);
        zfree(refs[k]);
    }
    zfree(expect);
}

/* DEBUG DIGEST depends on the bits and the length, not on the containers. */
TEST_F(BitmapRoaringTest, DigestIgnoresContainers) {
    size_t len = 20000;
    unsigned char *ref = (unsigned char *)zmalloc(len);
    fillRandom(ref, len, 0.0, 1); /* runs only: run containers after conversion */
    robj *s = createStringObject((char *)ref, len);
    robj *a = bitmapRoaringObjectFromString(s);
    decrRefCount(s);
    robj *b = createBitmapRoaringObject(len);
    for (uint64_t i = 0; i < (uint64_t)len * 8; i++) {
        if (refGetBit(ref, len, i)) bitmapRoaringSetBit(b, i, 1);
    }
    unsigned char da[20], db[20];
    memset(da, 0, sizeof(da));
    memset(db, 0, sizeof(db));
    bitmapRoaringDigest(da, a);
    bitmapRoaringDigest(db, b);
    EXPECT_EQ(memcmp(da, db, sizeof(da)), 0);
    bitmapRoaringGrow(b, len + 1);
    memset(db, 0, sizeof(db));
    bitmapRoaringDigest(db, b);
    EXPECT_NE(memcmp(da, db, sizeof(da)), 0);
    decrRefCount(a);
    decrRefCount(b);
    zfree(ref);
}
