/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cassert>
#include <cstdint>
#include <sys/time.h>

extern "C" {
#include "adlist.h"
#include "fmacros.h"
#include "listpack.h"
#include "sds.h"
#include "zmalloc.h"

extern bool accurate;
/* Function declarations from listpack.c */
unsigned char *lpSkip(unsigned char *p);
}

#define LP_INTBUF_SIZE 21 /* 20 digits of -2^63 + 1 null term. */

/* Macros from listpack.c needed for testing */
#define LP_HDR_SIZE 6u /* 32 bit total len + 16 bit number of elements. */
#define LP_EOF 0xFF
#define LP_HDR_NUMELE_UNKNOWN (uint32_t) UINT16_MAX
#define LP_HDR_NUMELE_UNKNOWN_UL (unsigned long)UINT16_MAX
#define LP_ENCODING_7BIT_UINT_MASK 0x80
#define LP_ENCODING_IS_7BIT_UINT(byte) (((byte) & LP_ENCODING_7BIT_UINT_MASK) == 0)
#define LP_ENCODING_6BIT_STR 0x80
#define LP_ENCODING_6BIT_STR_MASK 0xC0
#define LP_ENCODING_IS_6BIT_STR(byte) (((byte) & LP_ENCODING_6BIT_STR_MASK) == LP_ENCODING_6BIT_STR)
#define LP_ENCODING_13BIT_INT 0xC0
#define LP_ENCODING_13BIT_INT_MASK 0xE0
#define LP_ENCODING_IS_13BIT_INT(byte) (((byte) & LP_ENCODING_13BIT_INT_MASK) == LP_ENCODING_13BIT_INT)
#define LP_ENCODING_12BIT_STR 0xE0
#define LP_ENCODING_12BIT_STR_MASK 0xF0
#define LP_ENCODING_IS_12BIT_STR(byte) (((byte) & LP_ENCODING_12BIT_STR_MASK) == LP_ENCODING_12BIT_STR)
#define LP_ENCODING_16BIT_INT 0xF1
#define LP_ENCODING_16BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_16BIT_INT(byte) (((byte) & LP_ENCODING_16BIT_INT_MASK) == LP_ENCODING_16BIT_INT)
#define LP_ENCODING_24BIT_INT 0xF2
#define LP_ENCODING_24BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_24BIT_INT(byte) (((byte) & LP_ENCODING_24BIT_INT_MASK) == LP_ENCODING_24BIT_INT)
#define LP_ENCODING_32BIT_INT 0xF3
#define LP_ENCODING_32BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_32BIT_INT(byte) (((byte) & LP_ENCODING_32BIT_INT_MASK) == LP_ENCODING_32BIT_INT)
#define LP_ENCODING_64BIT_INT 0xF4
#define LP_ENCODING_64BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_64BIT_INT(byte) (((byte) & LP_ENCODING_64BIT_INT_MASK) == LP_ENCODING_64BIT_INT)
#define LP_ENCODING_32BIT_STR 0xF0
#define LP_ENCODING_32BIT_STR_MASK 0xFF
#define LP_ENCODING_IS_32BIT_STR(byte) (((byte) & LP_ENCODING_32BIT_STR_MASK) == LP_ENCODING_32BIT_STR)
#define lpGetNumElements(p) (((uint32_t)((p)[4]) << 0) | ((uint32_t)((p)[5]) << 8))

static const char *mixlist[] = {"hello", "foo", "quux", "1024"};
static const char *intlist[] = {"4294967296", "-100", "100", "128000",
                                "non integer", "much much longer non integer"};

static unsigned char *createList(void) {
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char *)mixlist[1], strlen(mixlist[1]));
    lp = lpAppend(lp, (unsigned char *)mixlist[2], strlen(mixlist[2]));
    lp = lpPrepend(lp, (unsigned char *)mixlist[0], strlen(mixlist[0]));
    lp = lpAppend(lp, (unsigned char *)mixlist[3], strlen(mixlist[3]));
    return lp;
}

static unsigned char *createIntList(void) {
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char *)intlist[2], strlen(intlist[2]));
    lp = lpAppend(lp, (unsigned char *)intlist[3], strlen(intlist[3]));
    lp = lpPrepend(lp, (unsigned char *)intlist[1], strlen(intlist[1]));
    lp = lpPrepend(lp, (unsigned char *)intlist[0], strlen(intlist[0]));
    lp = lpAppend(lp, (unsigned char *)intlist[4], strlen(intlist[4]));
    lp = lpAppend(lp, (unsigned char *)intlist[5], strlen(intlist[5]));
    return lp;
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return ((long long)tv.tv_sec * 1000000) + tv.tv_usec;
}

static void stress(int pos, int num, int maxsize, int dnum) {
    int i, j, k;
    unsigned char *lp;
    char posstr[2][5] = {"HEAD", "TAIL"};
    long long start;
    for (i = 0; i < maxsize; i += dnum) {
        lp = lpNew(0);
        for (j = 0; j < i; j++) {
            lp = lpAppend(lp, (unsigned char *)("quux"), 4);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++) {
            if (pos == 0) {
                lp = lpPrepend(lp, (unsigned char *)("quux"), 4);
            } else {
                lp = lpAppend(lp, (unsigned char *)("quux"), 4);
            }
            lp = lpDelete(lp, lpFirst(lp), nullptr);
        }
        printf("List size: %8d, bytes: %8zu, %dx push+pop (%s): %6lld usec\n", i, lpBytes(lp), num, posstr[pos],
               usec() - start);
        lpFree(lp);
    }
}

static unsigned char *pop(unsigned char *lp, int where, void *expected) {
    unsigned char *p, *vstr;
    int64_t vlen;

    p = lpSeek(lp, where == 0 ? 0 : -1);
    vstr = lpGet(p, &vlen, nullptr);

    if (vstr) {
        size_t expected_len = strlen((const char *)expected);
        assert(vlen == (int64_t)expected_len);
        assert(memcmp(vstr, expected, expected_len) == 0);
    } else {
        assert(vlen == *(int64_t *)expected);
    }

    return lpDelete(lp, p, &p);
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
    default:
        ADD_FAILURE() << "Unexpected random value";
        return 0;
    }

    while (p < len) target[p++] = minval + rand() % (maxval - minval + 1);
    return len;
}

static int verifyEntry(unsigned char *p, unsigned char *s, size_t slen) {
    assert(lpCompare(p, s, slen));
    return 0;
}

static int lpValidation(unsigned char *p, unsigned int head_count, void *userdata) {
    UNUSED(p);
    UNUSED(head_count);

    int ret;
    long *count = (long *)userdata;
    ret = lpCompare(p, (unsigned char *)mixlist[*count], strlen(mixlist[*count]));
    (*count)++;
    return ret;
}

class ListpackTest : public ::testing::Test {
  protected:
    void SetUp() override {
        /* Seed random for tests that need it */
        srand(0);
    }
};

TEST_F(ListpackTest, listpackCreateIntList) {
    /* Create int list */
    unsigned char *lp;

    lp = createIntList();
    ASSERT_EQ(lpLength(lp), 6u);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackCreateList) {
    /* Create list */
    unsigned char *lp;

    lp = createList();
    ASSERT_EQ(lpLength(lp), 4u);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackLpPrepend) {
    /* Test lpPrepend */
    unsigned char *lp;

    lp = lpNew(0);
    lp = lpPrepend(lp, (unsigned char *)("abc"), 3);
    lp = lpPrepend(lp, (unsigned char *)("1024"), 4);
    verifyEntry(lpSeek(lp, 0), (unsigned char *)("1024"), 4);
    verifyEntry(lpSeek(lp, 1), (unsigned char *)("abc"), 3);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackLpPrependInteger) {
    /* Test lpPrependInteger */
    unsigned char *lp;

    lp = lpNew(0);
    lp = lpPrependInteger(lp, 127);
    lp = lpPrependInteger(lp, 4095);
    lp = lpPrependInteger(lp, 32767);
    lp = lpPrependInteger(lp, 8388607);
    lp = lpPrependInteger(lp, 2147483647);
    lp = lpPrependInteger(lp, 9223372036854775807LL);
    verifyEntry(lpSeek(lp, 0), (unsigned char *)("9223372036854775807"), 19);
    verifyEntry(lpSeek(lp, -1), (unsigned char *)("127"), 3);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackGetElementAtIndex) {
    /* Get element at index */
    unsigned char *lp;

    lp = createList();
    verifyEntry(lpSeek(lp, 0), (unsigned char *)("hello"), 5);
    verifyEntry(lpSeek(lp, 3), (unsigned char *)("1024"), 4);
    verifyEntry(lpSeek(lp, -1), (unsigned char *)("1024"), 4);
    verifyEntry(lpSeek(lp, -4), (unsigned char *)("hello"), 5);
    ASSERT_EQ(lpSeek(lp, 4), nullptr);
    ASSERT_EQ(lpSeek(lp, -5), nullptr);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackPop) {
    /* Pop list */
    unsigned char *lp;
    int64_t expected;

    lp = createList();
    expected = 1024;
    lp = pop(lp, 1, &expected);
    lp = pop(lp, 0, (void *)"hello");
    lp = pop(lp, 1, (void *)"quux");
    lp = pop(lp, 1, (void *)"foo");
    lpFree(lp);
}

TEST_F(ListpackTest, listpackGetElementAtIndex2) {
    /* Get element at index */
    unsigned char *lp;

    lp = createList();
    verifyEntry(lpSeek(lp, 0), (unsigned char *)("hello"), 5);
    verifyEntry(lpSeek(lp, 3), (unsigned char *)("1024"), 4);
    verifyEntry(lpSeek(lp, -1), (unsigned char *)("1024"), 4);
    verifyEntry(lpSeek(lp, -4), (unsigned char *)("hello"), 5);
    ASSERT_EQ(lpSeek(lp, 4), nullptr);
    ASSERT_EQ(lpSeek(lp, -5), nullptr);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackIterate0toEnd) {
    /* Iterate list from 0 to end */
    int i;
    unsigned char *lp, *p;

    lp = createList();
    p = lpFirst(lp);
    i = 0;
    while (p) {
        verifyEntry(p, (unsigned char *)mixlist[i], strlen(mixlist[i]));
        p = lpNext(lp, p);
        i++;
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackIterate1toEnd) {
    /* Iterate list from 1 to end */
    int i;
    unsigned char *lp, *p;

    lp = createList();
    i = 1;
    p = lpSeek(lp, i);
    while (p) {
        verifyEntry(p, (unsigned char *)mixlist[i], strlen(mixlist[i]));
        p = lpNext(lp, p);
        i++;
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackIterate2toEnd) {
    /* Iterate list from 2 to end */
    int i;
    unsigned char *lp, *p;

    lp = createList();
    i = 2;
    p = lpSeek(lp, i);
    while (p) {
        verifyEntry(p, (unsigned char *)mixlist[i], strlen(mixlist[i]));
        p = lpNext(lp, p);
        i++;
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackIterateBackToFront) {
    /* Iterate from back to front */
    int i;
    unsigned char *lp, *p;

    lp = createList();
    p = lpLast(lp);
    i = 3;
    while (p) {
        verifyEntry(p, (unsigned char *)mixlist[i], strlen(mixlist[i]));
        p = lpPrev(lp, p);
        i--;
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackIterateBackToFrontWithDelete) {
    /* Iterate from back to front, deleting all items */
    int i;
    unsigned char *lp, *p;

    lp = createList();
    p = lpLast(lp);
    i = 3;
    while ((p = lpLast(lp))) {
        verifyEntry(p, (unsigned char *)mixlist[i], strlen(mixlist[i]));
        lp = lpDelete(lp, p, &p);
        ASSERT_EQ(p, nullptr);
        i--;
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackDeleteWhenNumIsMinusOne) {
    /* Delete whole listpack when num == -1 */
    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 0, -1);
    ASSERT_EQ(lpLength(lp), 0u);
    ASSERT_EQ(lp[LP_HDR_SIZE], LP_EOF);
    ASSERT_EQ(lpBytes(lp), (size_t)(LP_HDR_SIZE + 1));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpFirst(lp);
    lp = lpDeleteRangeWithEntry(lp, &ptr, -1);
    ASSERT_EQ(lpLength(lp), 0u);
    ASSERT_EQ(lp[LP_HDR_SIZE], LP_EOF);
    ASSERT_EQ(lpBytes(lp), (size_t)(LP_HDR_SIZE + 1));
    zfree(lp);
}

TEST_F(ListpackTest, listpackDeleteWithNegativeIndex) {
    /* Delete whole listpack with negative index */
    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, -4, 4);
    ASSERT_EQ(lpLength(lp), 0u);
    ASSERT_EQ(lp[LP_HDR_SIZE], LP_EOF);
    ASSERT_EQ(lpBytes(lp), (size_t)(LP_HDR_SIZE + 1));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpSeek(lp, -4);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 4);
    ASSERT_EQ(lpLength(lp), 0u);
    ASSERT_EQ(lp[LP_HDR_SIZE], LP_EOF);
    ASSERT_EQ(lpBytes(lp), (size_t)(LP_HDR_SIZE + 1));
    zfree(lp);
}

TEST_F(ListpackTest, listpackDeleteInclusiveRange0_0) {
    /* Delete inclusive range 0,0 */
    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 0, 1);
    ASSERT_EQ(lpLength(lp), 3u);
    ASSERT_EQ(lpSkip(lpLast(lp))[0], LP_EOF); /* check set LP_EOF correctly */
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpFirst(lp);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 1);
    ASSERT_EQ(lpLength(lp), 3u);
    ASSERT_EQ(lpSkip(lpLast(lp))[0], LP_EOF); /* check set LP_EOF correctly */
    zfree(lp);
}

TEST_F(ListpackTest, listpackDeleteInclusiveRange0_1) {
    /* Delete inclusive range 0,1 */
    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 0, 2);
    ASSERT_EQ(lpLength(lp), 2u);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[2], strlen(mixlist[2]));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpFirst(lp);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 2);
    ASSERT_EQ(lpLength(lp), 2u);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[2], strlen(mixlist[2]));
    zfree(lp);
}

TEST_F(ListpackTest, listpackDeleteInclusiveRange1_2) {
    /* Delete inclusive range 1,2 */
    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 1, 2);
    ASSERT_EQ(lpLength(lp), 2u);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[0], strlen(mixlist[0]));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpSeek(lp, 1);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 2);
    ASSERT_EQ(lpLength(lp), 2u);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[0], strlen(mixlist[0]));
    zfree(lp);
}

TEST_F(ListpackTest, listpackDeleteWitStartIndexOutOfRange) {
    /* Delete with start index out of range */
    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 5, 1);
    ASSERT_EQ(lpLength(lp), 4u);
    zfree(lp);
}

TEST_F(ListpackTest, listpackDeleteWitNumOverflow) {
    /* Delete with num overflow */
    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 1, 5);
    ASSERT_EQ(lpLength(lp), 1u);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[0], strlen(mixlist[0]));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpSeek(lp, 1);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 5);
    ASSERT_EQ(lpLength(lp), 1u);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[0], strlen(mixlist[0]));
    zfree(lp);
}

TEST_F(ListpackTest, listpackBatchDelete) {
    /* Batch delete */
    unsigned char *lp = createList(); /* char *mixlist[] = {"hello", "foo", "quux", "1024"} */
    ASSERT_EQ(lpLength(lp), 4u);      /* Pre-condition */
    unsigned char *p0 = lpFirst(lp), *p1 = lpNext(lp, p0), *p2 = lpNext(lp, p1), *p3 = lpNext(lp, p2);
    unsigned char *ps[] = {p0, p1, p3};
    lp = lpBatchDelete(lp, ps, 3);
    ASSERT_EQ(lpLength(lp), 1u);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[2], strlen(mixlist[2]));
    ASSERT_EQ(lpValidateIntegrity(lp, lpBytes(lp), 1, nullptr, nullptr), 1);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackDeleteFooWhileIterating) {
    /* Delete foo while iterating */
    unsigned char *lp, *p;

    lp = createList();
    p = lpFirst(lp);
    while (p) {
        if (lpCompare(p, (unsigned char *)("foo"), 3)) {
            lp = lpDelete(lp, p, &p);
        } else {
            p = lpNext(lp, p);
        }
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackReplaceWithSameSize) {
    /* Replace with same size */
    unsigned char *lp, *p, *orig_lp;

    orig_lp = lp = createList(); /* "hello", "foo", "quux", "1024" */
    p = lpSeek(lp, 0);
    lp = lpReplace(lp, &p, (unsigned char *)("zoink"), 5);
    p = lpSeek(lp, 3);
    lp = lpReplace(lp, &p, (unsigned char *)("y"), 1);
    p = lpSeek(lp, 1);
    lp = lpReplace(lp, &p, (unsigned char *)("65536"), 5);
    p = lpSeek(lp, 0);
    ASSERT_EQ(memcmp((char *)p,
                     "\x85zoink\x06"
                     "\xf2\x00\x00\x01\x04" /* 65536 as int24 */
                     "\x84quux\05"
                     "\x81y\x02"
                     "\xff",
                     22),
              0);
    ASSERT_EQ(lp, orig_lp); /* no reallocations have happened */
    lpFree(lp);
}

TEST_F(ListpackTest, listpackReplaceWithDifferentSize) {
    /* Replace with different size */
    unsigned char *lp, *p;

    lp = createList(); /* "hello", "foo", "quux", "1024" */
    p = lpSeek(lp, 1);
    lp = lpReplace(lp, &p, (unsigned char *)("squirrel"), 8);
    p = lpSeek(lp, 0);
    ASSERT_EQ(strncmp((char *)p,
                      "\x85hello\x06"
                      "\x88squirrel\x09"
                      "\x84quux\x05"
                      "\xc4\x00\x02"
                      "\xff",
                      27),
              0);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackRegressionGt255Bytes) {
    /* Regression test for >255 byte strings */
    unsigned char *lp, *p, *vstr;
    int64_t vlen;

    char v1[257] = {0}, v2[257] = {0};
    memset(v1, 'x', 256);
    memset(v2, 'y', 256);
    lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char *)v1, strlen(v1));
    lp = lpAppend(lp, (unsigned char *)v2, strlen(v2));

    /* Pop values again and compare their value. */
    p = lpFirst(lp);
    vstr = lpGet(p, &vlen, nullptr);
    ASSERT_EQ(strncmp(v1, (char *)vstr, vlen), 0);
    p = lpSeek(lp, 1);
    vstr = lpGet(p, &vlen, nullptr);
    ASSERT_EQ(strncmp(v2, (char *)vstr, vlen), 0);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackCreateLongListAndCheckIndices) {
    /* Create long list and check indices */
    unsigned char *lp, *p;
    int64_t vlen;

    lp = lpNew(0);
    char buf[32];
    int i, len;
    for (i = 0; i < 1000; i++) {
        len = snprintf(buf, sizeof(buf), "%d", i);
        lp = lpAppend(lp, (unsigned char *)buf, len);
    }
    for (i = 0; i < 1000; i++) {
        p = lpSeek(lp, i);
        lpGet(p, &vlen, nullptr);
        ASSERT_EQ(i, vlen);

        p = lpSeek(lp, -i - 1);
        lpGet(p, &vlen, nullptr);
        ASSERT_EQ(999 - i, vlen);
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackCompareStrsWithLpEntries) {
    /* Compare strings with listpack entries */
    unsigned char *lp, *p;

    lp = createList();
    p = lpSeek(lp, 0);
    ASSERT_TRUE(lpCompare(p, (unsigned char *)("hello"), 5));
    ASSERT_FALSE(lpCompare(p, (unsigned char *)("hella"), 5));

    p = lpSeek(lp, 3);
    ASSERT_TRUE(lpCompare(p, (unsigned char *)("1024"), 4));
    lpFree(lp);
}

TEST_F(ListpackTest, listpackLpMergeEmptyLps) {
    /* lpMerge two empty listpacks */
    unsigned char *lp1 = lpNew(0);
    unsigned char *lp2 = lpNew(0);

    /* Merge two empty listpacks, get empty result back. */
    lp1 = lpMerge(&lp1, &lp2);
    ASSERT_EQ(lpLength(lp1), 0u);
    zfree(lp1);
}

TEST_F(ListpackTest, listpackLpMergeLp1Larger) {
    /* lpMerge two listpacks - first larger than second */
    unsigned char *lp1 = createIntList();
    unsigned char *lp2 = createList();

    size_t lp1_bytes = lpBytes(lp1);
    size_t lp2_bytes = lpBytes(lp2);
    unsigned long lp1_len = lpLength(lp1);
    unsigned long lp2_len = lpLength(lp2);

    unsigned char *lp3 = lpMerge(&lp1, &lp2);
    ASSERT_EQ(lp3, lp1);
    ASSERT_EQ(lp2, nullptr);
    ASSERT_EQ(lpLength(lp3), lp1_len + lp2_len);
    ASSERT_EQ(lpBytes(lp3), lp1_bytes + lp2_bytes - LP_HDR_SIZE - 1);
    verifyEntry(lpSeek(lp3, 0), (unsigned char *)("4294967296"), 10);
    verifyEntry(lpSeek(lp3, 5), (unsigned char *)("much much longer non integer"), 28);
    verifyEntry(lpSeek(lp3, 6), (unsigned char *)("hello"), 5);
    verifyEntry(lpSeek(lp3, -1), (unsigned char *)("1024"), 4);
    zfree(lp3);
}

TEST_F(ListpackTest, listpackLpMergeLp2Larger) {
    /* lpMerge two listpacks - second larger than first */
    unsigned char *lp1 = createList();
    unsigned char *lp2 = createIntList();

    size_t lp1_bytes = lpBytes(lp1);
    size_t lp2_bytes = lpBytes(lp2);
    unsigned long lp1_len = lpLength(lp1);
    unsigned long lp2_len = lpLength(lp2);

    unsigned char *lp3 = lpMerge(&lp1, &lp2);
    ASSERT_EQ(lp3, lp2);
    ASSERT_EQ(lp1, nullptr);
    ASSERT_EQ(lpLength(lp3), lp1_len + lp2_len);
    ASSERT_EQ(lpBytes(lp3), lp1_bytes + lp2_bytes - LP_HDR_SIZE - 1);
    verifyEntry(lpSeek(lp3, 0), (unsigned char *)("hello"), 5);
    verifyEntry(lpSeek(lp3, 3), (unsigned char *)("1024"), 4);
    verifyEntry(lpSeek(lp3, 4), (unsigned char *)("4294967296"), 10);
    verifyEntry(lpSeek(lp3, -1), (unsigned char *)("much much longer non integer"), 28);
    zfree(lp3);
}

TEST_F(ListpackTest, listpackLpNextRandom) {
    /* lpNextRandom normal usage */
    /* Create some data */
    unsigned char *lp = lpNew(0);
    unsigned char buf[100] = "asdf";
    unsigned int size = 100;
    for (size_t i = 0; i < size; i++) {
        lp = lpAppend(lp, buf, i);
    }
    ASSERT_EQ(lpLength(lp), size);

    /* Pick a subset of the elements of every possible subset size */
    for (unsigned int count = 0; count <= size; count++) {
        unsigned int remaining = count;
        unsigned char *p = lpFirst(lp);
        unsigned char *prev = nullptr;
        unsigned index = 0;
        while (remaining > 0) {
            ASSERT_NE(p, nullptr);
            p = lpNextRandom(lp, p, &index, remaining--, 0);
            ASSERT_NE(p, nullptr);
            ASSERT_NE(p, prev);
            prev = p;
            p = lpNext(lp, p);
            index++;
        }
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackLpNextRandomCC) {
    /* lpNextRandom corner cases */
    unsigned char *lp = lpNew(0);
    unsigned i = 0;

    /* Pick from empty listpack returns NULL. */
    ASSERT_EQ(lpNextRandom(lp, nullptr, &i, 2, 0), nullptr);

    /* Add some elements and find their pointers within the listpack. */
    lp = lpAppend(lp, (unsigned char *)("abc"), 3);
    lp = lpAppend(lp, (unsigned char *)("def"), 3);
    lp = lpAppend(lp, (unsigned char *)("ghi"), 3);
    ASSERT_EQ(lpLength(lp), 3u);
    unsigned char *p0 = lpFirst(lp);
    unsigned char *p1 = lpNext(lp, p0);
    unsigned char *p2 = lpNext(lp, p1);
    ASSERT_EQ(lpNext(lp, p2), nullptr);

    /* Pick zero elements returns NULL. */
    i = 0;
    ASSERT_EQ(lpNextRandom(lp, lpFirst(lp), &i, 0, 0), nullptr);

    /* Pick all returns all. */
    i = 0;
    ASSERT_TRUE(lpNextRandom(lp, p0, &i, 3, 0) == p0 && i == 0);
    i = 1;
    ASSERT_TRUE(lpNextRandom(lp, p1, &i, 2, 0) == p1 && i == 1);
    i = 2;
    ASSERT_TRUE(lpNextRandom(lp, p2, &i, 1, 0) == p2 && i == 2);

    /* Pick more than one when there's only one left returns the last one. */
    i = 2;
    ASSERT_TRUE(lpNextRandom(lp, p2, &i, 42, 0) == p2 && i == 2);

    /* Pick all even elements returns p0 and p2. */
    i = 0;
    ASSERT_TRUE(lpNextRandom(lp, p0, &i, 10, 1) == p0 && i == 0);
    i = 1;
    ASSERT_TRUE(lpNextRandom(lp, p1, &i, 10, 1) == p2 && i == 2);

    /* Don't crash even for bad index. */
    for (int j = 0; j < 100; j++) {
        unsigned char *p = nullptr;
        switch (j % 4) {
        case 0: p = p0; break;
        case 1: p = p1; break;
        case 2: p = p2; break;
        case 3: p = nullptr; break;
        }
        i = j % 7;
        unsigned int remaining = j % 5;
        p = lpNextRandom(lp, p, &i, remaining, 0);
        ASSERT_TRUE(p == p0 || p == p1 || p == p2 || p == nullptr);
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackRandomPairWithOneElement) {
    /* Random pair with one element */
    listpackEntry key, val;
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char *)("abc"), 3);
    lp = lpAppend(lp, (unsigned char *)("123"), 3);
    lpRandomPair(lp, 1, &key, &val);
    ASSERT_EQ(memcmp(key.sval, "abc", key.slen), 0);
    ASSERT_EQ(val.lval, 123);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackRandomPairWithManyElements) {
    /* Random pair with many elements */
    listpackEntry key, val;
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char *)("abc"), 3);
    lp = lpAppend(lp, (unsigned char *)("123"), 3);
    lp = lpAppend(lp, (unsigned char *)("456"), 3);
    lp = lpAppend(lp, (unsigned char *)("def"), 3);
    lpRandomPair(lp, 2, &key, &val);
    if (key.sval) {
        ASSERT_EQ(memcmp(key.sval, "abc", key.slen), 0);
        ASSERT_EQ(key.slen, 3u);
        ASSERT_EQ(val.lval, 123);
    }
    if (!key.sval) {
        ASSERT_EQ(key.lval, 456);
        ASSERT_EQ(memcmp(val.sval, "def", val.slen), 0);
    }
    lpFree(lp);
}

TEST_F(ListpackTest, listpackRandomPairsWithOneElement) {
    /* Random pairs with one element */
    int count = 5;
    unsigned char *lp = lpNew(0);
    listpackEntry *keys = (listpackEntry *)(zmalloc(sizeof(listpackEntry) * count));
    listpackEntry *vals = (listpackEntry *)(zmalloc(sizeof(listpackEntry) * count));

    lp = lpAppend(lp, (unsigned char *)("abc"), 3);
    lp = lpAppend(lp, (unsigned char *)("123"), 3);
    lpRandomPairs(lp, count, keys, vals);
    ASSERT_EQ(memcmp(keys[4].sval, "abc", keys[4].slen), 0);
    ASSERT_EQ(vals[4].lval, 123);
    zfree(keys);
    zfree(vals);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackRandomPairsWithManyElements) {
    /* Random pairs with many elements */
    int count = 5;
    unsigned char *lp = lpNew(0);
    listpackEntry *keys = (listpackEntry *)(zmalloc(sizeof(listpackEntry) * count));
    listpackEntry *vals = (listpackEntry *)(zmalloc(sizeof(listpackEntry) * count));

    lp = lpAppend(lp, (unsigned char *)("abc"), 3);
    lp = lpAppend(lp, (unsigned char *)("123"), 3);
    lp = lpAppend(lp, (unsigned char *)("456"), 3);
    lp = lpAppend(lp, (unsigned char *)("def"), 3);
    lpRandomPairs(lp, count, keys, vals);
    for (int i = 0; i < count; i++) {
        if (keys[i].sval) {
            ASSERT_EQ(memcmp(keys[i].sval, "abc", keys[i].slen), 0);
            ASSERT_EQ(keys[i].slen, 3u);
            ASSERT_EQ(vals[i].lval, 123);
        }
        if (!keys[i].sval) {
            ASSERT_EQ(keys[i].lval, 456);
            ASSERT_EQ(memcmp(vals[i].sval, "def", vals[i].slen), 0);
        }
    }
    zfree(keys);
    zfree(vals);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackRandomPairsUniqueWithOneElement) {
    /* Random pairs unique with one element */
    unsigned picked;
    int count = 5;
    unsigned char *lp = lpNew(0);
    listpackEntry *keys = (listpackEntry *)(zmalloc(sizeof(listpackEntry) * count));
    listpackEntry *vals = (listpackEntry *)(zmalloc(sizeof(listpackEntry) * count));

    lp = lpAppend(lp, (unsigned char *)("abc"), 3);
    lp = lpAppend(lp, (unsigned char *)("123"), 3);
    picked = lpRandomPairsUnique(lp, count, keys, vals);
    ASSERT_EQ(picked, 1u);
    ASSERT_EQ(memcmp(keys[0].sval, "abc", keys[0].slen), 0);
    ASSERT_EQ(vals[0].lval, 123);
    zfree(keys);
    zfree(vals);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackRandomPairsUniqueWithManyElements) {
    /* Random pairs unique with many elements */
    unsigned picked;
    int count = 5;
    unsigned char *lp = lpNew(0);
    listpackEntry *keys = (listpackEntry *)(zmalloc(sizeof(listpackEntry) * count));
    listpackEntry *vals = (listpackEntry *)(zmalloc(sizeof(listpackEntry) * count));

    lp = lpAppend(lp, (unsigned char *)("abc"), 3);
    lp = lpAppend(lp, (unsigned char *)("123"), 3);
    lp = lpAppend(lp, (unsigned char *)("456"), 3);
    lp = lpAppend(lp, (unsigned char *)("def"), 3);
    picked = lpRandomPairsUnique(lp, count, keys, vals);
    ASSERT_EQ(picked, 2u);
    for (unsigned i = 0; i < 2; i++) {
        if (keys[i].sval) {
            ASSERT_EQ(memcmp(keys[i].sval, "abc", keys[i].slen), 0);
            ASSERT_EQ(keys[i].slen, 3u);
            ASSERT_EQ(vals[i].lval, 123);
        }
        if (!keys[i].sval) {
            ASSERT_EQ(keys[i].lval, 456);
            ASSERT_EQ(memcmp(vals[i].sval, "def", vals[i].slen), 0);
        }
    }
    zfree(keys);
    zfree(vals);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackPushVariousEncodings) {
    /* push various encodings */
    unsigned char *lp;

    lp = lpNew(0);

    /* Push integer encode element using lpAppend */
    lp = lpAppend(lp, (unsigned char *)("127"), 3);
    ASSERT_TRUE(LP_ENCODING_IS_7BIT_UINT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)("4095"), 4);
    ASSERT_TRUE(LP_ENCODING_IS_13BIT_INT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)("32767"), 5);
    ASSERT_TRUE(LP_ENCODING_IS_16BIT_INT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)("8388607"), 7);
    ASSERT_TRUE(LP_ENCODING_IS_24BIT_INT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)("2147483647"), 10);
    ASSERT_TRUE(LP_ENCODING_IS_32BIT_INT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)("9223372036854775807"), 19);
    ASSERT_TRUE(LP_ENCODING_IS_64BIT_INT(lpLast(lp)[0]));

    /* Push integer encode element using lpAppendInteger */
    lp = lpAppendInteger(lp, 127);
    ASSERT_TRUE(LP_ENCODING_IS_7BIT_UINT(lpLast(lp)[0]));
    verifyEntry(lpLast(lp), (unsigned char *)("127"), 3);
    lp = lpAppendInteger(lp, 4095);
    verifyEntry(lpLast(lp), (unsigned char *)("4095"), 4);
    ASSERT_TRUE(LP_ENCODING_IS_13BIT_INT(lpLast(lp)[0]));
    lp = lpAppendInteger(lp, 32767);
    verifyEntry(lpLast(lp), (unsigned char *)("32767"), 5);
    ASSERT_TRUE(LP_ENCODING_IS_16BIT_INT(lpLast(lp)[0]));
    lp = lpAppendInteger(lp, 8388607);
    verifyEntry(lpLast(lp), (unsigned char *)("8388607"), 7);
    ASSERT_TRUE(LP_ENCODING_IS_24BIT_INT(lpLast(lp)[0]));
    lp = lpAppendInteger(lp, 2147483647);
    verifyEntry(lpLast(lp), (unsigned char *)("2147483647"), 10);
    ASSERT_TRUE(LP_ENCODING_IS_32BIT_INT(lpLast(lp)[0]));
    lp = lpAppendInteger(lp, 9223372036854775807);
    verifyEntry(lpLast(lp), (unsigned char *)("9223372036854775807"), 19);
    ASSERT_TRUE(LP_ENCODING_IS_64BIT_INT(lpLast(lp)[0]));

    /* string encode */
    unsigned char *str = (unsigned char *)(zmalloc(65535));
    memset(str, 0, 65535);
    lp = lpAppend(lp, str, 63);
    ASSERT_TRUE(LP_ENCODING_IS_6BIT_STR(lpLast(lp)[0]));
    lp = lpAppend(lp, str, 4095);
    ASSERT_TRUE(LP_ENCODING_IS_12BIT_STR(lpLast(lp)[0]));
    lp = lpAppend(lp, str, 65535);
    ASSERT_TRUE(LP_ENCODING_IS_32BIT_STR(lpLast(lp)[0]));
    zfree(str);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackLpFind) {
    /* Test lpFind */
    unsigned char *lp;

    lp = createList();
    ASSERT_EQ(lpFind(lp, lpFirst(lp), (unsigned char *)("abc"), 3, 0), nullptr);
    verifyEntry(lpFind(lp, lpFirst(lp), (unsigned char *)("hello"), 5, 0),
                (unsigned char *)("hello"), 5);
    verifyEntry(lpFind(lp, lpFirst(lp), (unsigned char *)("1024"), 4, 0),
                (unsigned char *)("1024"), 4);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackLpValidateIntegrity) {
    /* Test lpValidateIntegrity */
    unsigned char *lp;

    lp = createList();
    long count = 0;
    ASSERT_EQ(lpValidateIntegrity(lp, lpBytes(lp), 1, lpValidation, &count), 1);
    lpFree(lp);
}

TEST_F(ListpackTest, listpackNumberOfElementsExceedsLP_HDR_NUMELE_UNKNOWN) {
    /* Test number of elements exceeds LP_HDR_NUMELE_UNKNOWN */
    unsigned char *lp;

    lp = lpNew(0);
    for (uint32_t i = 0; i < LP_HDR_NUMELE_UNKNOWN + 1; i++)
        lp = lpAppend(lp, (unsigned char *)("1"), 1);

    ASSERT_EQ(lpGetNumElements(lp), LP_HDR_NUMELE_UNKNOWN);
    ASSERT_EQ(lpLength(lp), LP_HDR_NUMELE_UNKNOWN_UL + 1);

    lp = lpDeleteRange(lp, -2, 2);
    ASSERT_EQ(lpGetNumElements(lp), LP_HDR_NUMELE_UNKNOWN);
    ASSERT_EQ(lpLength(lp), LP_HDR_NUMELE_UNKNOWN_UL - 1);
    ASSERT_EQ(lpGetNumElements(lp), LP_HDR_NUMELE_UNKNOWN - 1); /* update length after lpLength */
    lpFree(lp);
}

/* This is a stress test with random payloads of different encoding.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ListpackTest.DISABLED_listpackStressWithRandom --gtest_also_run_disabled_tests
 */
TEST_F(ListpackTest, DISABLED_listpackStressWithRandom) {
    /* Stress with random payloads of different encoding */
    unsigned char *lp, *vstr;
    int64_t vlen;
    unsigned char intbuf[LP_INTBUF_SIZE];

    unsigned long long start = usec();
    int i, j, len, where;
    unsigned char *p;
    char buf[1024];
    int buflen;
    list *ref;
    listNode *refnode;

    int iteration = accurate ? 20000 : 20;
    for (i = 0; i < iteration; i++) {
        lp = lpNew(0);
        ref = listCreate();
        listSetFreeMethod(ref, sdsfreeVoid);
        len = rand() % 256;

        /* Create lists */
        for (j = 0; j < len; j++) {
            where = (rand() & 1) ? 0 : 1;
            if (rand() % 2) {
                buflen = randstring(buf, 1, sizeof(buf) - 1);
            } else {
                switch (rand() % 3) {
                case 0: buflen = snprintf(buf, sizeof(buf), "%lld", (0LL + rand()) >> 20); break;
                case 1: buflen = snprintf(buf, sizeof(buf), "%lld", (0LL + rand())); break;
                case 2: buflen = snprintf(buf, sizeof(buf), "%lld", (0LL + rand()) << 20); break;
                default: ADD_FAILURE() << "Unexpected random value"; buflen = 0;
                }
            }

            /* Add to listpack */
            if (where == 0) {
                lp = lpPrepend(lp, (unsigned char *)buf, buflen);
            } else {
                lp = lpAppend(lp, (unsigned char *)buf, buflen);
            }

            /* Add to reference list */
            if (where == 0) {
                listAddNodeHead(ref, sdsnewlen(buf, buflen));
            } else if (where == 1) {
                listAddNodeTail(ref, sdsnewlen(buf, buflen));
            } else {
                ADD_FAILURE() << "Invalid where value";
            }
        }

        ASSERT_EQ(listLength(ref), lpLength(lp));
        for (j = 0; j < len; j++) {
            /* Naive way to get elements, but similar to the stresser
             * executed from the Tcl test suite. */
            p = lpSeek(lp, j);
            refnode = listIndex(ref, j);

            vstr = lpGet(p, &vlen, intbuf);
            ASSERT_EQ(memcmp(vstr, listNodeValue(refnode), vlen), 0);
        }
        lpFree(lp);
        listRelease(ref);
    }
    printf("Done. usec=%lld\n\n", usec() - start);
}

/* This is a stress test with variable listpack size.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ListpackTest.DISABLED_listpackSTressWithVariableSize --gtest_also_run_disabled_tests
 */
TEST_F(ListpackTest, DISABLED_listpackSTressWithVariableSize) {
    /* Stress with variable listpack size */
    unsigned long long start = usec();
    int maxsize = accurate ? 16384 : 16;
    stress(0, 100000, maxsize, 256);
    stress(1, 100000, maxsize, 256);
    printf("Done. usec=%lld\n\n", usec() - start);
}

/* Benchmark tests - these are DISABLED by default.
 * To run benchmark tests explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=ListpackTest.DISABLED_listpackBenchmark* --gtest_also_run_disabled_tests
 */

class ListpackBenchmark : public ::testing::Test {
  protected:
    static unsigned char *lp;
    static int iteration;

    static void SetUpTestSuite() {
        iteration = accurate ? 100000 : 100;
        lp = lpNew(0);
    }

    static void TearDownTestSuite() {
        if (lp) {
            lpFree(lp);
            lp = nullptr;
        }
    }
};

unsigned char *ListpackBenchmark::lp = nullptr;
int ListpackBenchmark::iteration = 0;

TEST_F(ListpackBenchmark, DISABLED_listpackBenchmarkLpAppend) {
    /* Benchmark lpAppend */
    unsigned long long start = usec();
    for (int i = 0; i < iteration; i++) {
        char buf[4096] = "asdf";
        lp = lpAppend(lp, (unsigned char *)buf, 4);
        lp = lpAppend(lp, (unsigned char *)buf, 40);
        lp = lpAppend(lp, (unsigned char *)buf, 400);
        lp = lpAppend(lp, (unsigned char *)buf, 4000);
        lp = lpAppend(lp, (unsigned char *)("1"), 1);
        lp = lpAppend(lp, (unsigned char *)("10"), 2);
        lp = lpAppend(lp, (unsigned char *)("100"), 3);
        lp = lpAppend(lp, (unsigned char *)("1000"), 4);
        lp = lpAppend(lp, (unsigned char *)("10000"), 5);
        lp = lpAppend(lp, (unsigned char *)("100000"), 6);
    }
    printf("Done. usec=%lld\n", usec() - start);
}

TEST_F(ListpackBenchmark, DISABLED_listpackBenchmarkLpFindString) {
    /* Benchmark lpFind string */
    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *fptr = lpFirst(lp);
        fptr = lpFind(lp, fptr, (unsigned char *)("nothing"), 7, 1);
    }
    printf("Done. usec=%lld\n", usec() - start);
}

TEST_F(ListpackBenchmark, DISABLED_listpackBenchmarkLpFindNumber) {
    /* Benchmark lpFind number */
    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *fptr = lpFirst(lp);
        fptr = lpFind(lp, fptr, (unsigned char *)("99999"), 5, 1);
    }
    printf("Done. usec=%lld\n", usec() - start);
}

TEST_F(ListpackBenchmark, DISABLED_listpackBenchmarkLpSeek) {
    /* Benchmark lpSeek */
    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        lpSeek(lp, 99999);
    }
    printf("Done. usec=%lld\n", usec() - start);
}

TEST_F(ListpackBenchmark, DISABLED_listpackBenchmarkLpValidateIntegrity) {
    /* Benchmark lpValidateIntegrity */
    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        lpValidateIntegrity(lp, lpBytes(lp), 1, nullptr, nullptr);
    }
    printf("Done. usec=%lld\n", usec() - start);
}

TEST_F(ListpackBenchmark, DISABLED_listpackBenchmarkLpCompareWithString) {
    /* Benchmark lpCompare with string */
    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *eptr = lpSeek(lp, 0);
        while (eptr != nullptr) {
            lpCompare(eptr, (unsigned char *)("nothing"), 7);
            eptr = lpNext(lp, eptr);
        }
    }
    printf("Done. usec=%lld\n", usec() - start);
}

TEST_F(ListpackBenchmark, DISABLED_listpackBenchmarkLpCompareWithNumber) {
    /* Benchmark lpCompare with number */
    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *eptr = lpSeek(lp, 0);
        while (eptr != nullptr) {
            lpCompare(eptr, (unsigned char *)("99999"), 5);
            eptr = lpNext(lp, eptr);
        }
    }
    printf("Done. usec=%lld\n", usec() - start);
}
