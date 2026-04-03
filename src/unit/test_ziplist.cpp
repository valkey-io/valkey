/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cassert>

extern "C" {
#include "adlist.h"
#include "fmacros.h"
#include "ziplist.h"
#include "zmalloc.h"

extern bool accurate;
/* External declarations for internal ziplist.c types and symbols needed for testing */
typedef struct zlentry {
    unsigned int prevrawlensize;
    unsigned int prevrawlen;
    unsigned int lensize;
    unsigned int len;
    unsigned int headersize;
    unsigned char encoding;
    unsigned char *p;
} zlentry;

unsigned int zipStorePrevEntryLength(unsigned char *p, unsigned int len);
unsigned int zipStoreEntryEncoding(unsigned char *p, unsigned char encoding, unsigned int rawlen);
void testOnlyZipEntry(unsigned char *p, zlentry *e);
}

/* Macros from ziplist.c needed for testing */
#define ZIP_BIG_PREVLEN 254
#define ZIPLIST_HEADER_SIZE (sizeof(uint32_t) * 2 + sizeof(uint16_t))
#define ZIPLIST_ENTRY_HEAD(zl) ((zl) + ZIPLIST_HEADER_SIZE)
#define ZIPLIST_ENTRY_ZERO(zle)                              \
    {                                                        \
        (zle)->prevrawlensize = (zle)->prevrawlen = 0;       \
        (zle)->lensize = (zle)->len = (zle)->headersize = 0; \
        (zle)->encoding = 0;                                 \
        (zle)->p = NULL;                                     \
    }

#include <sys/time.h>

static unsigned char *createList(void) {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char *)("foo"), 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)("quux"), 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)("hello"), 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char *)("1024"), 4, ZIPLIST_TAIL);
    return zl;
}

static unsigned char *createIntList(void) {
    unsigned char *zl = ziplistNew();
    char buf[32];

    snprintf(buf, sizeof(buf), "100");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_TAIL);
    snprintf(buf, sizeof(buf), "128000");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_TAIL);
    snprintf(buf, sizeof(buf), "-100");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_HEAD);
    snprintf(buf, sizeof(buf), "4294967296");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_HEAD);
    snprintf(buf, sizeof(buf), "non integer");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_TAIL);
    snprintf(buf, sizeof(buf), "much much longer non integer");
    zl = ziplistPush(zl, (unsigned char *)buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return ((long long)tv.tv_sec * 1000000) + tv.tv_usec;
}

static void stress(int pos, int num, int maxsize, int dnum) {
    int i, j, k;
    unsigned char *zl;
    for (i = 0; i < maxsize; i += dnum) {
        zl = ziplistNew();
        for (j = 0; j < i; j++) {
            zl = ziplistPush(zl, (unsigned char *)("quux"), 4, ZIPLIST_TAIL);
        }

        /* Do num times a push+pop from pos */
        for (k = 0; k < num; k++) {
            zl = ziplistPush(zl, (unsigned char *)("quux"), 4, pos);
            zl = ziplistDeleteRange(zl, 0, 1);
        }
        zfree(zl);
    }
}

static unsigned char *pop(unsigned char *zl, int where) {
    unsigned char *p, *vstr;
    unsigned int vlen;
    long long vlong = 0;

    p = ziplistIndex(zl, where == ZIPLIST_HEAD ? 0 : -1);
    if (ziplistGet(p, &vstr, &vlen, &vlong)) {
        return ziplistDelete(zl, &p);
    } else {
        exit(1);
    }
}

static int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min + rand() % (max - min + 1);
    int minval = 0, maxval = 0;
    switch (rand() % 3) {
    case 0:
        minval = 0;
        maxval = 255;
        break;
    case 1:
        minval = 48;
        maxval = 122;
        break;
    case 2:
        minval = 48;
        maxval = 52;
        break;
    default: return 0;
    }

    while (p < len) target[p++] = minval + rand() % (maxval - minval + 1);
    return len;
}

static void verify(unsigned char *zl, zlentry *e) {
    int len = ziplistLen(zl);
    zlentry _e;

    ZIPLIST_ENTRY_ZERO(&_e);

    for (int i = 0; i < len; i++) {
        memset(&e[i], 0, sizeof(zlentry));
        testOnlyZipEntry(ziplistIndex(zl, i), &e[i]);

        memset(&_e, 0, sizeof(zlentry));
        testOnlyZipEntry(ziplistIndex(zl, -len + i), &_e);

        ASSERT_EQ(memcmp(&e[i], &_e, sizeof(zlentry)), 0);
    }
}

static unsigned char *insertHelper(unsigned char *zl, char ch, size_t len, unsigned char *pos) {
    assert(len <= (size_t)ZIP_BIG_PREVLEN);
    unsigned char data[ZIP_BIG_PREVLEN] = {0};
    memset(data, ch, len);
    return ziplistInsert(zl, pos, data, len);
}

static int compareHelper(unsigned char *zl, char ch, size_t len, int index) {
    assert(len <= (size_t)ZIP_BIG_PREVLEN);
    unsigned char data[ZIP_BIG_PREVLEN] = {0};
    memset(data, ch, len);
    unsigned char *p = ziplistIndex(zl, index);
    assert(p != nullptr);
    return ziplistCompare(p, data, len);
}

static size_t strEntryBytesSmall(size_t slen) {
    return slen + zipStorePrevEntryLength(nullptr, 0) + zipStoreEntryEncoding(nullptr, 0, slen);
}

static size_t strEntryBytesLarge(size_t slen) {
    return slen + zipStorePrevEntryLength(nullptr, ZIP_BIG_PREVLEN) + zipStoreEntryEncoding(nullptr, 0, slen);
}

class ZiplistTest : public ::testing::Test {
  protected:
    void SetUp() override {
        srand(0);
    }
};

TEST_F(ZiplistTest, ziplistCreateIntList) {
    unsigned char *zl, *p;

    zl = createIntList();
    /* "4294967296", "-100", "100", "128000", "non integer", "much much longer non integer" */

    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("4294967296"), 10));

    p = ziplistIndex(zl, 1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("-100"), 4));

    p = ziplistIndex(zl, 2);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("100"), 3));

    p = ziplistIndex(zl, 3);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("128000"), 6));

    p = ziplistIndex(zl, 4);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("non integer"), 11));

    p = ziplistIndex(zl, 5);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("much much longer non integer"), 28));

    zfree(zl);
}

TEST_F(ZiplistTest, ziplistPop) {
    unsigned char *zl, *p;

    zl = createList(); /* "hello", "foo", "quux", "1024" */

    p = ziplistIndex(zl, -1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("1024"), 4));

    zl = pop(zl, ZIPLIST_TAIL); /* "hello", "foo", "quux" */

    p = ziplistIndex(zl, -1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("quux"), 4));

    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("hello"), 5));

    zl = pop(zl, ZIPLIST_HEAD); /* "foo", "quux" */

    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("foo"), 3));

    zl = pop(zl, ZIPLIST_TAIL); /* "foo" */

    p = ziplistIndex(zl, -1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("foo"), 3));

    zl = pop(zl, ZIPLIST_TAIL);

    ASSERT_EQ(ziplistLen(zl), 0u);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistGetElementAtIndex3) {
    unsigned char *zl, *p;
    zl = createList(); /* "hello", "foo", "quux", "1024" */
    p = ziplistIndex(zl, 3);
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("1024"), 4));
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistGetElementOutOfRange) {
    unsigned char *zl, *p;
    zl = createList();
    p = ziplistIndex(zl, 4);
    ASSERT_EQ(p, nullptr);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistGetLastElement) {
    unsigned char *zl, *p;
    zl = createList(); /* "hello", "foo", "quux", "1024" */
    p = ziplistIndex(zl, -1);
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("1024"), 4));
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistGetFirstElement) {
    unsigned char *zl, *p;
    zl = createList(); /* "hello", "foo", "quux", "1024" */
    p = ziplistIndex(zl, -4);
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("hello"), 5));
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistGetElementOutOfRangeReverse) {
    unsigned char *zl, *p;
    zl = createList(); /* "hello", "foo", "quux", "1024" */
    p = ziplistIndex(zl, -5);
    ASSERT_EQ(p, nullptr);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistIterateThroughFullList) {
    unsigned char *zl, *p, *entry;
    unsigned int elen;
    long long value;

    zl = createList();
    p = ziplistIndex(zl, 0);
    while (ziplistGet(p, &entry, &elen, &value)) {
        ASSERT_NE(p, nullptr);
        p = ziplistNext(zl, p);
    }
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistIterateThroughListFrom1ToEnd) {
    unsigned char *zl, *p, *entry;
    unsigned int elen;
    long long value;

    zl = createList();
    p = ziplistIndex(zl, 1);
    while (ziplistGet(p, &entry, &elen, &value)) {
        ASSERT_NE(p, nullptr);
        p = ziplistNext(zl, p);
    }
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistIterateThroughListFrom2ToEnd) {
    unsigned char *zl, *p, *entry;
    unsigned int elen;
    long long value;

    zl = createList();
    p = ziplistIndex(zl, 2);
    while (ziplistGet(p, &entry, &elen, &value)) {
        ASSERT_NE(p, nullptr);
        p = ziplistNext(zl, p);
    }
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistIterateThroughStartOutOfRange) {
    unsigned char *zl, *p;
    zl = createList();
    p = ziplistIndex(zl, 4);
    ASSERT_EQ(p, nullptr);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistIterateBackToFront) {
    unsigned char *zl, *p, *entry;
    unsigned int elen;
    long long value;

    zl = createList();
    p = ziplistIndex(zl, -1);
    while (ziplistGet(p, &entry, &elen, &value)) {
        ASSERT_NE(p, nullptr);
        p = ziplistPrev(zl, p);
    }
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistIterateBackToFrontDeletingAllItems) {
    unsigned char *zl, *p, *entry;
    unsigned int elen;
    long long value;

    zl = createList();
    p = ziplistIndex(zl, -1);
    while (ziplistGet(p, &entry, &elen, &value)) {
        ASSERT_NE(p, nullptr);
        zl = ziplistDelete(zl, &p);
        p = ziplistPrev(zl, p);
    }
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistDeleteInclusiveRange0To0) {
    unsigned char *zl, *p;
    zl = createList(); /* "hello", "foo", "quux", "1024" */

    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("hello"), 5));
    int orig_len = ziplistLen(zl);

    zl = ziplistDeleteRange(zl, 0, 1);
    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("foo"), 3));
    int new_len = ziplistLen(zl);
    ASSERT_EQ(orig_len - 1, new_len);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistDeleteInclusiveRange0To1) {
    unsigned char *zl, *p;
    zl = createList(); /* "hello", "foo", "quux", "1024" */

    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("hello"), 5));
    p = ziplistIndex(zl, 1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("foo"), 3));
    int orig_len = ziplistLen(zl);

    zl = ziplistDeleteRange(zl, 0, 2); /* "quux", "1024" */

    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("quux"), 4));
    p = ziplistIndex(zl, 1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("1024"), 4));
    int new_len = ziplistLen(zl);
    ASSERT_EQ(orig_len - 2, new_len);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistDeleteInclusiveRange1To2) {
    unsigned char *zl, *p;
    zl = createList(); /* "hello", "foo", "quux", "1024" */

    p = ziplistIndex(zl, 1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("foo"), 3));
    p = ziplistIndex(zl, 2);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("quux"), 4));
    int orig_len = ziplistLen(zl);

    zl = ziplistDeleteRange(zl, 1, 2); /* "hello", "1024" */

    p = ziplistIndex(zl, 1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("1024"), 4));
    int new_len = ziplistLen(zl);
    ASSERT_EQ(orig_len - 2, new_len);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistDeleteWithStartIndexOutOfRange) {
    unsigned char *zl;
    zl = createList();
    int orig_len = ziplistLen(zl);
    zl = ziplistDeleteRange(zl, 5, 1);
    int new_len = ziplistLen(zl);
    ASSERT_EQ(orig_len, new_len);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistDeleteWithNumOverflow) {
    unsigned char *zl;
    zl = createList(); /* "hello", "foo", "quux", "1024" */

    int orig_len = ziplistLen(zl);
    zl = ziplistDeleteRange(zl, 1, 5);
    int new_len = ziplistLen(zl);
    ASSERT_EQ(orig_len - 3, new_len);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistDeleteFooWhileIterating) {
    unsigned char *zl, *p, *entry;
    unsigned int elen;
    long long value;

    zl = createList(); /* "hello", "foo", "quux", "1024" */
    p = ziplistIndex(zl, 0);
    while (ziplistGet(p, &entry, &elen, &value)) {
        ASSERT_NE(p, nullptr);
        if (entry && strncmp("foo", (char *)entry, elen) == 0) {
            zl = ziplistDelete(zl, &p);
        } else {
            p = ziplistNext(zl, p);
        }
    }
    p = ziplistIndex(zl, 1);
    ziplistGet(p, &entry, &elen, &value);
    ASSERT_FALSE(ziplistCompare(p, (unsigned char *)("foo"), 3));
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("quux"), 4));
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistReplaceWithSameSize) {
    unsigned char *zl, *p;
    zl = createList(); /* "hello", "foo", "quux", "1024" */
    unsigned char *orig_zl = zl;

    p = ziplistIndex(zl, 0);
    zl = ziplistReplace(zl, p, (unsigned char *)("zoink"), 5);
    p = ziplistIndex(zl, 3);
    zl = ziplistReplace(zl, p, (unsigned char *)("yy"), 2);
    p = ziplistIndex(zl, 1);
    zl = ziplistReplace(zl, p, (unsigned char *)("65536"), 5);

    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("zoink"), 5));
    p = ziplistIndex(zl, 3);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("yy"), 2));
    p = ziplistIndex(zl, 1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("65536"), 5));

    ASSERT_EQ(zl, orig_zl); /* no reallocations have happened */
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistReplaceWithDifferentSize) {
    unsigned char *zl, *p;
    zl = createList(); /* "hello", "foo", "quux", "1024" */

    p = ziplistIndex(zl, 1);
    zl = ziplistReplace(zl, p, (unsigned char *)("squirrel"), 8);

    p = ziplistIndex(zl, 1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("squirrel"), 8));
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistRegressionTestForOver255ByteStrings) {
    unsigned char *zl, *p, *vstr;
    unsigned int vlen;
    long long vlong;

    char v1[257] = {0}, v2[257] = {0};
    memset(v1, 'x', 256);
    memset(v2, 'y', 256);
    zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char *)v1, strlen(v1), ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)v2, strlen(v2), ZIPLIST_TAIL);

    /* Pop values again and compare their value. */
    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistGet(p, &vstr, &vlen, &vlong));
    ASSERT_EQ(strncmp(v1, (char *)vstr, vlen), 0);
    p = ziplistIndex(zl, 1);
    ASSERT_TRUE(ziplistGet(p, &vstr, &vlen, &vlong));
    ASSERT_EQ(strncmp(v2, (char *)vstr, vlen), 0);
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistRegressionTestDeleteNextToLastEntries) {
    unsigned char *zl;
    char v[3][257] = {{0}};
    zlentry e[3] = {{0, 0, 0, 0, 0, 0, nullptr}};
    size_t i;

    for (i = 0; i < (sizeof(v) / sizeof(v[0])); i++) {
        memset(v[i], 'a' + i, sizeof(v[0]));
    }

    v[0][256] = '\0';
    v[1][1] = '\0';
    v[2][256] = '\0';

    zl = ziplistNew();
    for (i = 0; i < (sizeof(v) / sizeof(v[0])); i++) {
        zl = ziplistPush(zl, (unsigned char *)v[i], strlen(v[i]), ZIPLIST_TAIL);
    }

    verify(zl, e);

    ASSERT_EQ(e[0].prevrawlensize, 1u);
    ASSERT_EQ(e[1].prevrawlensize, 5u);
    ASSERT_EQ(e[2].prevrawlensize, 1u);

    /* Deleting entry 1 will increase `prevrawlensize` for entry 2 */
    unsigned char *p = e[1].p;
    zl = ziplistDelete(zl, &p);

    verify(zl, e);

    ASSERT_EQ(e[0].prevrawlensize, 1u);
    ASSERT_EQ(e[1].prevrawlensize, 5u);

    zfree(zl);
}

TEST_F(ZiplistTest, ziplistCreateLongListAndCheckIndices) {
    unsigned char *zl, *p, *vstr;
    unsigned int vlen;
    long long vlong;

    zl = ziplistNew();
    char buf[32];
    int i, len;
    for (i = 0; i < 1000; i++) {
        len = snprintf(buf, sizeof(buf), "%d", i);
        zl = ziplistPush(zl, (unsigned char *)buf, len, ZIPLIST_TAIL);
    }
    for (i = 0; i < 1000; i++) {
        p = ziplistIndex(zl, i);
        ASSERT_TRUE(ziplistGet(p, &vstr, &vlen, &vlong));

        p = ziplistIndex(zl, -i - 1);
        ASSERT_TRUE(ziplistGet(p, &vstr, &vlen, &vlong));
    }
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistCompareStringWithZiplistEntries) {
    unsigned char *zl, *p;

    zl = createList();
    p = ziplistIndex(zl, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("hello"), 5));
    ASSERT_FALSE(ziplistCompare(p, (unsigned char *)("hella"), 5));

    p = ziplistIndex(zl, 3);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("1024"), 4));
    zfree(zl);
}

TEST_F(ZiplistTest, ziplistMergeTest) {
    unsigned char *zl, *p;
    /* create list gives us: [hello, foo, quux, 1024] */
    zl = createList();
    unsigned char *zl2 = createList();

    unsigned char *zl3 = ziplistNew();
    unsigned char *zl4 = ziplistNew();

    ASSERT_FALSE(ziplistMerge(&zl4, &zl4));

    /* Merge two empty ziplists, get empty result back. */
    zl4 = ziplistMerge(&zl3, &zl4);
    ASSERT_EQ(ziplistLen(zl4), 0u);
    zfree(zl4);

    zl2 = ziplistMerge(&zl, &zl2);
    /* merge gives us: [hello, foo, quux, 1024, hello, foo, quux, 1024] */

    ASSERT_EQ(ziplistLen(zl2), 8u);

    p = ziplistIndex(zl2, 0);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("hello"), 5));
    ASSERT_FALSE(ziplistCompare(p, (unsigned char *)("hella"), 5));

    p = ziplistIndex(zl2, 3);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("1024"), 4));
    ASSERT_FALSE(ziplistCompare(p, (unsigned char *)("1025"), 4));

    p = ziplistIndex(zl2, 4);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("hello"), 5));
    ASSERT_FALSE(ziplistCompare(p, (unsigned char *)("hella"), 5));

    p = ziplistIndex(zl2, 7);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("1024"), 4));
    ASSERT_FALSE(ziplistCompare(p, (unsigned char *)("1025"), 4));

    zfree(zl);
}

/* This is a stress test with random payloads of different encoding.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ZiplistTest.DISABLED_ziplistStressWithRandomPayloadsOfDifferentEncoding --gtest_also_run_disabled_tests
 */
TEST_F(ZiplistTest, DISABLED_ziplistStressWithRandomPayloadsOfDifferentEncoding) {
    int i, j, len, where;
    unsigned char *p;
    char buf[1024];
    int buflen = 0;
    list *ref;
    listNode *refnode;

    /* Hold temp vars from ziplist */
    unsigned char *sstr;
    unsigned int slen;
    long long sval = 0;

    int iteration = accurate ? 20000 : 20;
    for (i = 0; i < iteration; i++) {
        unsigned char *zl = ziplistNew();
        ref = listCreate();
        listSetFreeMethod(ref, sdsfreeVoid);
        len = rand() % 256;

        /* Create lists */
        for (j = 0; j < len; j++) {
            where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
            if (rand() % 2) {
                buflen = randstring(buf, 1, sizeof(buf) - 1);
            } else {
                switch (rand() % 3) {
                case 0: buflen = snprintf(buf, sizeof(buf), "%lld", (0LL + rand()) >> 20); break;
                case 1: buflen = snprintf(buf, sizeof(buf), "%lld", (0LL + rand())); break;
                case 2: buflen = snprintf(buf, sizeof(buf), "%lld", (0LL + rand()) << 20); break;
                default: ASSERT_TRUE(false);
                }
            }

            /* Add to ziplist */
            zl = ziplistPush(zl, (unsigned char *)buf, buflen, where);

            /* Add to reference list */
            if (where == ZIPLIST_HEAD) {
                listAddNodeHead(ref, sdsnewlen(buf, buflen));
            } else if (where == ZIPLIST_TAIL) {
                listAddNodeTail(ref, sdsnewlen(buf, buflen));
            } else {
                ASSERT_TRUE(false);
            }
        }

        ASSERT_EQ(listLength(ref), ziplistLen(zl));
        for (j = 0; j < len; j++) {
            /* Naive way to get elements, but similar to the stresser
             * executed from the Tcl test suite. */
            p = ziplistIndex(zl, j);
            refnode = listIndex(ref, j);

            ASSERT_TRUE(ziplistGet(p, &sstr, &slen, &sval));
            if (sstr == nullptr) {
                buflen = snprintf(buf, sizeof(buf), "%lld", sval);
            } else {
                buflen = slen;
                memcpy(buf, sstr, buflen);
                buf[buflen] = '\0';
            }
            ASSERT_EQ(memcmp(buf, listNodeValue(refnode), buflen), 0);
        }
        zfree(zl);
        listRelease(ref);
    }
}

TEST_F(ZiplistTest, ziplistCascadeUpdateEdgeCases) {
    unsigned char *zl;
    /* Inserting a entry with data length greater than ZIP_BIG_PREVLEN-4
     * will leads to cascade update. */
    size_t s1 = ZIP_BIG_PREVLEN - 4, s2 = ZIP_BIG_PREVLEN - 3;
    zl = ziplistNew();

    zlentry e[4] = {{0, 0, 0, 0, 0, 0, nullptr}};

    zl = insertHelper(zl, 'a', s1, ZIPLIST_ENTRY_HEAD(zl));
    verify(zl, e);

    ASSERT_EQ(e[0].prevrawlensize, 1u);
    ASSERT_EQ(e[0].prevrawlen, 0u);
    ASSERT_TRUE(compareHelper(zl, 'a', s1, 0));

    /* No expand. */
    zl = insertHelper(zl, 'b', s1, ZIPLIST_ENTRY_HEAD(zl));
    verify(zl, e);

    ASSERT_EQ(e[0].prevrawlensize, 1u);
    ASSERT_EQ(e[0].prevrawlen, 0u);
    ASSERT_TRUE(compareHelper(zl, 'b', s1, 0));

    ASSERT_EQ(e[1].prevrawlensize, 1u);
    ASSERT_EQ(e[1].prevrawlen, strEntryBytesSmall(s1));
    ASSERT_TRUE(compareHelper(zl, 'a', s1, 1));

    /* Expand(tail included). */
    zl = insertHelper(zl, 'c', s2, ZIPLIST_ENTRY_HEAD(zl));
    verify(zl, e);

    ASSERT_EQ(e[0].prevrawlensize, 1u);
    ASSERT_EQ(e[0].prevrawlen, 0u);
    ASSERT_TRUE(compareHelper(zl, 'c', s2, 0));

    ASSERT_EQ(e[1].prevrawlensize, 5u);
    ASSERT_EQ(e[1].prevrawlen, strEntryBytesSmall(s2));
    ASSERT_TRUE(compareHelper(zl, 'b', s1, 1));

    ASSERT_EQ(e[2].prevrawlensize, 5u);
    ASSERT_EQ(e[2].prevrawlen, strEntryBytesLarge(s1));
    ASSERT_TRUE(compareHelper(zl, 'a', s1, 2));

    /* Expand(only previous head entry). */
    zl = insertHelper(zl, 'd', s2, ZIPLIST_ENTRY_HEAD(zl));
    verify(zl, e);

    ASSERT_EQ(e[0].prevrawlensize, 1u);
    ASSERT_EQ(e[0].prevrawlen, 0u);
    ASSERT_TRUE(compareHelper(zl, 'd', s2, 0));

    ASSERT_EQ(e[1].prevrawlensize, 5u);
    ASSERT_EQ(e[1].prevrawlen, strEntryBytesSmall(s2));
    ASSERT_TRUE(compareHelper(zl, 'c', s2, 1));

    ASSERT_EQ(e[2].prevrawlensize, 5u);
    ASSERT_EQ(e[2].prevrawlen, strEntryBytesLarge(s2));
    ASSERT_TRUE(compareHelper(zl, 'b', s1, 2));

    ASSERT_EQ(e[3].prevrawlensize, 5u);
    ASSERT_EQ(e[3].prevrawlen, strEntryBytesLarge(s1));
    ASSERT_TRUE(compareHelper(zl, 'a', s1, 3));

    /* Delete from mid. */
    unsigned char *p = ziplistIndex(zl, 2);
    zl = ziplistDelete(zl, &p);
    verify(zl, e);

    ASSERT_EQ(e[0].prevrawlensize, 1u);
    ASSERT_EQ(e[0].prevrawlen, 0u);
    ASSERT_TRUE(compareHelper(zl, 'd', s2, 0));

    ASSERT_EQ(e[1].prevrawlensize, 5u);
    ASSERT_EQ(e[1].prevrawlen, strEntryBytesSmall(s2));
    ASSERT_TRUE(compareHelper(zl, 'c', s2, 1));

    ASSERT_EQ(e[2].prevrawlensize, 5u);
    ASSERT_EQ(e[2].prevrawlen, strEntryBytesLarge(s2));
    ASSERT_TRUE(compareHelper(zl, 'a', s1, 2));

    zfree(zl);
}

TEST_F(ZiplistTest, ziplistInsertEdgeCase) {
    unsigned char *zl, *p;
    // From issue #7170
    zl = ziplistNew();

    /* We set some values to almost reach the critical point - 254 */
    char A_252[253] = {0}, A_250[251] = {0};
    memset(A_252, 'A', 252);
    memset(A_250, 'A', 250);

    /* After the rpush, the list look like: [one two A_252 A_250 three 10] */
    zl = ziplistPush(zl, (unsigned char *)("one"), 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)("two"), 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)A_252, strlen(A_252), ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)A_250, strlen(A_250), ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)("three"), 5, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *)("10"), 2, ZIPLIST_TAIL);

    p = ziplistIndex(zl, 2);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)A_252, strlen(A_252)));

    /* When we remove A_252, the list became: [one two A_250 three 10]
     * A_250's prev node became node two, because node two quite small
     * So A_250's prevlenSize shrink to 1, A_250's total size became 253(1+2+250)
     * The prev node of node three is still node A_250.
     * We will not shrink the node three's prevlenSize, keep it at 5 bytes */
    zl = ziplistDelete(zl, &p);

    p = ziplistIndex(zl, 3);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("three"), 5));

    /* We want to insert a node after A_250, the list became: [one two A_250 10 three 10]
     * Because the new node is quite small, node three prevlenSize will shrink to 1 */
    zl = ziplistInsert(zl, p, (unsigned char *)("10"), 2);

    /* Last element should equal 10 */
    p = ziplistIndex(zl, -1);
    ASSERT_TRUE(ziplistCompare(p, (unsigned char *)("10"), 2));

    zfree(zl);
}

/* This is a stress test with variable size.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ZiplistTest.DISABLED_ziplistStressWithVariableSize --gtest_also_run_disabled_tests
 */
TEST_F(ZiplistTest, DISABLED_ziplistStressWithVariableSize) {
    unsigned long long start = usec();
    int maxsize = accurate ? 16384 : 16;
    stress(ZIPLIST_HEAD, 100000, maxsize, 256);
    printf("Stress with variable size HEAD: %lld usec\n", usec() - start);

    start = usec();
    stress(ZIPLIST_TAIL, 100000, maxsize, 256);
    printf("Stress with variable size TAIL: %lld usec\n", usec() - start);
}

/* This is a special unit test useful for benchmarking ziplistFind performance.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ZiplistTest.DISABLED_ziplistBenchmarkziplistFind --gtest_also_run_disabled_tests
 */
TEST_F(ZiplistTest, DISABLED_ziplistBenchmarkziplistFind) {
    unsigned char *zl = ziplistNew();
    int iteration = accurate ? 100000 : 100;
    for (int i = 0; i < iteration; i++) {
        char buf[4096] = "asdf";
        zl = ziplistPush(zl, (unsigned char *)buf, 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 40, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 400, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 4000, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1"), 1, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10"), 2, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100"), 3, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1000"), 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10000"), 5, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100000"), 6, ZIPLIST_TAIL);
    }

    long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *p = ziplistIndex(zl, 0);
        unsigned char *fptr = ziplistFind(zl, p, (unsigned char *)("nothing"), 7, 1);
        ASSERT_EQ(fptr, nullptr);
    }
    printf("ziplistFind: %lld usec\n", usec() - start);
    zfree(zl);
}

/* This is a benchmark test for ziplistIndex.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ZiplistTest.DISABLED_ziplistBenchmarkziplistIndex --gtest_also_run_disabled_tests
 */
TEST_F(ZiplistTest, DISABLED_ziplistBenchmarkziplistIndex) {
    unsigned char *zl = ziplistNew();
    int iteration = accurate ? 100000 : 100;
    for (int i = 0; i < iteration; i++) {
        char buf[4096] = "asdf";
        zl = ziplistPush(zl, (unsigned char *)buf, 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 40, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 400, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 4000, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1"), 1, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10"), 2, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100"), 3, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1000"), 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10000"), 5, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100000"), 6, ZIPLIST_TAIL);
    }

    long long start = usec();
    for (int i = 0; i < 2000; i++) {
        ziplistIndex(zl, 99999);
    }
    printf("ziplistIndex: %lld usec\n", usec() - start);

    zfree(zl);
}

/* This is a benchmark test for ziplistValidateIntegrity.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ZiplistTest.DISABLED_ziplistBenchmarkziplistValidateIntegrity --gtest_also_run_disabled_tests
 */
TEST_F(ZiplistTest, DISABLED_ziplistBenchmarkziplistValidateIntegrity) {
    unsigned char *zl = ziplistNew();
    int iteration = accurate ? 100000 : 100;
    for (int i = 0; i < iteration; i++) {
        char buf[4096] = "asdf";
        zl = ziplistPush(zl, (unsigned char *)buf, 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 40, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 400, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 4000, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1"), 1, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10"), 2, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100"), 3, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1000"), 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10000"), 5, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100000"), 6, ZIPLIST_TAIL);
    }
    long long start = usec();
    for (int i = 0; i < 2000; i++) {
        ziplistValidateIntegrity(zl, ziplistBlobLen(zl), 1, nullptr, nullptr);
    }
    printf("ziplistValidateIntegrity: %lld usec\n", usec() - start);

    zfree(zl);
}

/* This is a benchmark test for ziplistCompare with string.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ZiplistTest.DISABLED_ziplistBenchmarkziplistCompareWithString --gtest_also_run_disabled_tests
 */
TEST_F(ZiplistTest, DISABLED_ziplistBenchmarkziplistCompareWithString) {
    unsigned char *zl = ziplistNew();
    int iteration = accurate ? 100000 : 100;
    for (int i = 0; i < iteration; i++) {
        char buf[4096] = "asdf";
        zl = ziplistPush(zl, (unsigned char *)buf, 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 40, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 400, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 4000, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1"), 1, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10"), 2, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100"), 3, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1000"), 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10000"), 5, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100000"), 6, ZIPLIST_TAIL);
    }
    long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *eptr = ziplistIndex(zl, 0);
        while (eptr != nullptr) {
            ziplistCompare(eptr, (unsigned char *)("nothing"), 7);
            eptr = ziplistNext(zl, eptr);
        }
    }
    printf("ziplistCompare with string: %lld usec\n", usec() - start);

    zfree(zl);
}

/* This is a benchmark test for ziplistCompare with number.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ZiplistTest.DISABLED_ziplistBenchmarkziplistCompareWithNumber --gtest_also_run_disabled_tests
 */
TEST_F(ZiplistTest, DISABLED_ziplistBenchmarkziplistCompareWithNumber) {
    unsigned char *zl = ziplistNew();
    int iteration = accurate ? 100000 : 100;
    for (int i = 0; i < iteration; i++) {
        char buf[4096] = "asdf";
        zl = ziplistPush(zl, (unsigned char *)buf, 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 40, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 400, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)buf, 4000, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1"), 1, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10"), 2, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100"), 3, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("1000"), 4, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("10000"), 5, ZIPLIST_TAIL);
        zl = ziplistPush(zl, (unsigned char *)("100000"), 6, ZIPLIST_TAIL);
    }
    long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *eptr = ziplistIndex(zl, 0);
        while (eptr != nullptr) {
            ziplistCompare(eptr, (unsigned char *)("99999"), 5);
            eptr = ziplistNext(zl, eptr);
        }
    }
    printf("ziplistCompare with number: %lld usec\n", usec() - start);

    zfree(zl);
}

/* This is a stress test for __ziplistCascadeUpdate.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ZiplistTest.DISABLED_ziplistStress__ziplistCascadeUpdate --gtest_also_run_disabled_tests
 */
TEST_F(ZiplistTest, DISABLED_ziplistStress__ziplistCascadeUpdate) {
    char data[ZIP_BIG_PREVLEN];
    unsigned char *zl = ziplistNew();
    int iteration = accurate ? 100000 : 100;
    for (int i = 0; i < iteration; i++) {
        zl = ziplistPush(zl, (unsigned char *)data, ZIP_BIG_PREVLEN - 4, ZIPLIST_TAIL);
    }
    long long start = usec();
    zl = ziplistPush(zl, (unsigned char *)data, ZIP_BIG_PREVLEN - 3, ZIPLIST_HEAD);
    printf("Stress __ziplistCascadeUpdate: %lld usec\n", usec() - start);

    zfree(zl);
}
