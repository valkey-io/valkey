#include <stdint.h>
#include <sys/time.h>

#include "../listpack.c"

#include "../adlist.h"
#include "../sds.h"
#include "../zmalloc.h"
#include "test_help.h"

char *mixlist[] = {"hello", "foo", "quux", "1024"};
char *intlist[] = {"4294967296", "-100", "100", "128000", "non integer", "much much longer non integer"};

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
    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000000) + tv.tv_usec;
}

static void stress(int pos, int num, int maxsize, int dnum) {
    int i, j, k;
    unsigned char *lp;
    char posstr[2][5] = {"HEAD", "TAIL"};
    long long start;
    for (i = 0; i < maxsize; i += dnum) {
        lp = lpNew(0);
        for (j = 0; j < i; j++) {
            lp = lpAppend(lp, (unsigned char *)"quux", 4);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++) {
            if (pos == 0) {
                lp = lpPrepend(lp, (unsigned char *)"quux", 4);
            } else {
                lp = lpAppend(lp, (unsigned char *)"quux", 4);
            }
            lp = lpDelete(lp, lpFirst(lp), NULL);
        }
        printf("List size: %8d, bytes: %8zu, %dx push+pop (%s): %6lld usec\n", i, lpBytes(lp), num, posstr[pos],
               usec() - start);
        lpFree(lp);
    }
}

/* simple wrapper to used in functions whose return type is not int */
static inline int _test_assert(int c) {
    TEST_ASSERT(c);
    return 0;
}

static unsigned char *pop(unsigned char *lp, int where, void *expected) {
    unsigned char *p, *vstr;
    int64_t vlen;

    p = lpSeek(lp, where == 0 ? 0 : -1);
    vstr = lpGet(p, &vlen, NULL);

    if (vstr) {
        _test_assert(strcmp((const char *)vstr, (const char *)expected));
    } else {
        _test_assert(vlen == *(int64_t *)expected);
    }

    return lpDelete(lp, p, &p);
}

static int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min + rand() % (max - min + 1);
    int minval, maxval;
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
    default: TEST_ASSERT(NULL);
    }

    while (p < len) target[p++] = minval + rand() % (maxval - minval + 1);
    return len;
}

static int verifyEntry(unsigned char *p, unsigned char *s, size_t slen) {
    TEST_ASSERT(lpCompare(p, s, slen));
    return 0;
}

static int lpValidation(unsigned char *p, unsigned int head_count, void *userdata) {
    UNUSED(p);
    UNUSED(head_count);

    int ret;
    long *count = userdata;
    ret = lpCompare(p, (unsigned char *)mixlist[*count], strlen(mixlist[*count]));
    (*count)++;
    return ret;
}

int test_listpackCreateIntList(int argc, char **argv, int flags) {
    /* Create int list */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createIntList();
    TEST_ASSERT(lpLength(lp) == 6);
    lpFree(lp);

    return 0;
}

int test_listpackCreateList(int argc, char **argv, int flags) {
    /* Create list */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    TEST_ASSERT(lpLength(lp) == 4);
    lpFree(lp);

    return 0;
}

int test_listpackLpPrepend(int argc, char **argv, int flags) {
    /* Test lpPrepend */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = lpNew(0);
    lp = lpPrepend(lp, (unsigned char *)"abc", 3);
    lp = lpPrepend(lp, (unsigned char *)"1024", 4);
    verifyEntry(lpSeek(lp, 0), (unsigned char *)"1024", 4);
    verifyEntry(lpSeek(lp, 1), (unsigned char *)"abc", 3);
    lpFree(lp);

    return 0;
}

int test_listpackLpPrependInteger(int argc, char **argv, int flags) {
    /* Test lpPrependInteger */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = lpNew(0);
    lp = lpPrependInteger(lp, 127);
    lp = lpPrependInteger(lp, 4095);
    lp = lpPrependInteger(lp, 32767);
    lp = lpPrependInteger(lp, 8388607);
    lp = lpPrependInteger(lp, 2147483647);
    lp = lpPrependInteger(lp, 9223372036854775807);
    verifyEntry(lpSeek(lp, 0), (unsigned char *)"9223372036854775807", 19);
    verifyEntry(lpSeek(lp, -1), (unsigned char *)"127", 3);
    lpFree(lp);

    return 0;
}

int test_listpackGetELementAtIndex(int argc, char **argv, int flags) {
    /* Get element at index */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    verifyEntry(lpSeek(lp, 0), (unsigned char *)"hello", 5);
    verifyEntry(lpSeek(lp, 3), (unsigned char *)"1024", 4);
    verifyEntry(lpSeek(lp, -1), (unsigned char *)"1024", 4);
    verifyEntry(lpSeek(lp, -4), (unsigned char *)"hello", 5);
    TEST_ASSERT(lpSeek(lp, 4) == NULL);
    TEST_ASSERT(lpSeek(lp, -5) == NULL);
    lpFree(lp);

    return 0;
}

int test_listpackPop(int argc, char **argv, int flags) {
    /* Pop list */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;
    int64_t expected;

    lp = createList();
    expected = 1024;
    lp = pop(lp, 1, &expected);
    lp = pop(lp, 0, "hello");
    lp = pop(lp, 1, "quux");
    lp = pop(lp, 1, "foo");
    lpFree(lp);

    return 0;
}

int test_listpackGetELementAtIndex2(int argc, char **argv, int flags) {
    /* Get element at index */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    verifyEntry(lpSeek(lp, 0), (unsigned char *)"hello", 5);
    verifyEntry(lpSeek(lp, 3), (unsigned char *)"1024", 4);
    verifyEntry(lpSeek(lp, -1), (unsigned char *)"1024", 4);
    verifyEntry(lpSeek(lp, -4), (unsigned char *)"hello", 5);
    TEST_ASSERT(lpSeek(lp, 4) == NULL);
    TEST_ASSERT(lpSeek(lp, -5) == NULL);
    lpFree(lp);

    return 0;
}

int test_listpackIterate0toEnd(int argc, char **argv, int flags) {
    /* Iterate list from 0 to end */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

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

    return 0;
}

int test_listpackIterate1toEnd(int argc, char **argv, int flags) {
    /* Iterate list from 1 to end */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

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

    return 0;
}

int test_listpackIterate2toEnd(int argc, char **argv, int flags) {
    /* Iterate list from 2 to end */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

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

    return 0;
}

int test_listpackIterateBackToFront(int argc, char **argv, int flags) {
    /* Iterate from back to front */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

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

    return 0;
}

int test_listpackIterateBackToFrontWithDelete(int argc, char **argv, int flags) {
    /* Iterate from back to front, deleting all items */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int i;
    unsigned char *lp, *p;

    lp = createList();
    p = lpLast(lp);
    i = 3;
    while ((p = lpLast(lp))) {
        verifyEntry(p, (unsigned char *)mixlist[i], strlen(mixlist[i]));
        lp = lpDelete(lp, p, &p);
        TEST_ASSERT(p == NULL);
        i--;
    }
    lpFree(lp);

    return 0;
}

int test_listpackDeleteWhenNumIsMinusOne(int argc, char **argv, int flags) {
    /* Delete whole listpack when num == -1 */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 0, -1);
    TEST_ASSERT(lpLength(lp) == 0);
    TEST_ASSERT(lp[LP_HDR_SIZE] == LP_EOF);
    TEST_ASSERT(lpBytes(lp) == (LP_HDR_SIZE + 1));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpFirst(lp);
    lp = lpDeleteRangeWithEntry(lp, &ptr, -1);
    TEST_ASSERT(lpLength(lp) == 0);
    TEST_ASSERT(lp[LP_HDR_SIZE] == LP_EOF);
    TEST_ASSERT(lpBytes(lp) == (LP_HDR_SIZE + 1));
    zfree(lp);

    return 0;
}

int test_listpackDeleteWithNegativeIndex(int argc, char **argv, int flags) {
    /* Delete whole listpack with negative index */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, -4, 4);
    TEST_ASSERT(lpLength(lp) == 0);
    TEST_ASSERT(lp[LP_HDR_SIZE] == LP_EOF);
    TEST_ASSERT(lpBytes(lp) == (LP_HDR_SIZE + 1));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpSeek(lp, -4);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 4);
    TEST_ASSERT(lpLength(lp) == 0);
    TEST_ASSERT(lp[LP_HDR_SIZE] == LP_EOF);
    TEST_ASSERT(lpBytes(lp) == (LP_HDR_SIZE + 1));
    zfree(lp);

    return 0;
}

int test_listpackDeleteInclusiveRange0_0(int argc, char **argv, int flags) {
    /* Delete inclusive range 0,0 */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 0, 1);
    TEST_ASSERT(lpLength(lp) == 3);
    TEST_ASSERT(lpSkip(lpLast(lp))[0] == LP_EOF); /* check set LP_EOF correctly */
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpFirst(lp);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 1);
    TEST_ASSERT(lpLength(lp) == 3);
    TEST_ASSERT(lpSkip(lpLast(lp))[0] == LP_EOF); /* check set LP_EOF correctly */
    zfree(lp);

    return 0;
}

int test_listpackDeleteInclusiveRange0_1(int argc, char **argv, int flags) {
    /* Delete inclusive range 0,1 */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 0, 2);
    TEST_ASSERT(lpLength(lp) == 2);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[2], strlen(mixlist[2]));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpFirst(lp);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 2);
    TEST_ASSERT(lpLength(lp) == 2);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[2], strlen(mixlist[2]));
    zfree(lp);

    return 0;
}

int test_listpackDeleteInclusiveRange1_2(int argc, char **argv, int flags) {
    /* Delete inclusive range 1,2 */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 1, 2);
    TEST_ASSERT(lpLength(lp) == 2);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[0], strlen(mixlist[0]));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpSeek(lp, 1);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 2);
    TEST_ASSERT(lpLength(lp) == 2);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[0], strlen(mixlist[0]));
    zfree(lp);

    return 0;
}

int test_listpackDeleteWitStartIndexOutOfRange(int argc, char **argv, int flags) {
    /* Delete with start index out of range */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 5, 1);
    TEST_ASSERT(lpLength(lp) == 4);
    zfree(lp);

    return 0;
}

int test_listpackDeleteWitNumOverflow(int argc, char **argv, int flags) {
    /* Delete with num overflow */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    lp = lpDeleteRange(lp, 1, 5);
    TEST_ASSERT(lpLength(lp) == 1);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[0], strlen(mixlist[0]));
    zfree(lp);

    lp = createList();
    unsigned char *ptr = lpSeek(lp, 1);
    lp = lpDeleteRangeWithEntry(lp, &ptr, 5);
    TEST_ASSERT(lpLength(lp) == 1);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[0], strlen(mixlist[0]));
    zfree(lp);

    return 0;
}

int test_listpackBatchDelete(int argc, char **argv, int flags) {
    /* Batch delete */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp = createList(); /* char *mixlist[] = {"hello", "foo", "quux", "1024"} */
    TEST_ASSERT(lpLength(lp) == 4);   /* Pre-condition */
    unsigned char *p0 = lpFirst(lp), *p1 = lpNext(lp, p0), *p2 = lpNext(lp, p1), *p3 = lpNext(lp, p2);
    unsigned char *ps[] = {p0, p1, p3};
    lp = lpBatchDelete(lp, ps, 3);
    TEST_ASSERT(lpLength(lp) == 1);
    verifyEntry(lpFirst(lp), (unsigned char *)mixlist[2], strlen(mixlist[2]));
    TEST_ASSERT(lpValidateIntegrity(lp, lpBytes(lp), 1, NULL, NULL) == 1);
    lpFree(lp);

    return 0;
}

int test_listpackDeleteFooWhileIterating(int argc, char **argv, int flags) {
    /* Delete foo while iterating */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp, *p;

    lp = createList();
    p = lpFirst(lp);
    while (p) {
        if (lpCompare(p, (unsigned char *)"foo", 3)) {
            lp = lpDelete(lp, p, &p);
        } else {
            p = lpNext(lp, p);
        }
    }
    lpFree(lp);

    return 0;
}

int test_listpackReplaceWithSameSize(int argc, char **argv, int flags) {
    /* Replace with same size */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp, *p, *orig_lp;

    orig_lp = lp = createList(); /* "hello", "foo", "quux", "1024" */
    p = lpSeek(lp, 0);
    lp = lpReplace(lp, &p, (unsigned char *)"zoink", 5);
    p = lpSeek(lp, 3);
    lp = lpReplace(lp, &p, (unsigned char *)"y", 1);
    p = lpSeek(lp, 1);
    lp = lpReplace(lp, &p, (unsigned char *)"65536", 5);
    p = lpSeek(lp, 0);
    TEST_ASSERT(!memcmp((char *)p,
                        "\x85zoink\x06"
                        "\xf2\x00\x00\x01\x04" /* 65536 as int24 */
                        "\x84quux\05"
                        "\x81y\x02"
                        "\xff",
                        22));
    TEST_ASSERT(lp == orig_lp); /* no reallocations have happened */
    lpFree(lp);

    return 0;
}

int test_listpackReplaceWithDifferentSize(int argc, char **argv, int flags) {
    /* Replace with different size */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp, *p;

    lp = createList(); /* "hello", "foo", "quux", "1024" */
    p = lpSeek(lp, 1);
    lp = lpReplace(lp, &p, (unsigned char *)"squirrel", 8);
    p = lpSeek(lp, 0);
    TEST_ASSERT(!strncmp((char *)p,
                         "\x85hello\x06"
                         "\x88squirrel\x09"
                         "\x84quux\x05"
                         "\xc4\x00\x02"
                         "\xff",
                         27));
    lpFree(lp);

    return 0;
}

int test_listpackRegressionGt255Bytes(int argc, char **argv, int flags) {
    /* Regression test for >255 byte strings */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

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
    vstr = lpGet(p, &vlen, NULL);
    TEST_ASSERT(strncmp(v1, (char *)vstr, vlen) == 0);
    p = lpSeek(lp, 1);
    vstr = lpGet(p, &vlen, NULL);
    TEST_ASSERT(strncmp(v2, (char *)vstr, vlen) == 0);
    lpFree(lp);

    return 0;
}

int test_listpackCreateLongListAndCheckIndices(int argc, char **argv, int flags) {
    /* Create long list and check indices */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

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
        lpGet(p, &vlen, NULL);
        TEST_ASSERT(i == vlen);

        p = lpSeek(lp, -i - 1);
        lpGet(p, &vlen, NULL);
        TEST_ASSERT(999 - i == vlen);
    }
    lpFree(lp);

    return 0;
}

int test_listpackCompareStrsWithLpEntries(int argc, char **argv, int flags) {
    /* Compare strings with listpack entries */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp, *p;

    lp = createList();
    p = lpSeek(lp, 0);
    TEST_ASSERT(lpCompare(p, (unsigned char *)"hello", 5));
    TEST_ASSERT(!lpCompare(p, (unsigned char *)"hella", 5));

    p = lpSeek(lp, 3);
    TEST_ASSERT(lpCompare(p, (unsigned char *)"1024", 4));
    TEST_ASSERT(!lpCompare(p, (unsigned char *)"1025", 4));
    lpFree(lp);

    return 0;
}

int test_listpackLpMergeEmptyLps(int argc, char **argv, int flags) {
    /* lpMerge two empty listpacks */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp1 = lpNew(0);
    unsigned char *lp2 = lpNew(0);

    /* Merge two empty listpacks, get empty result back. */
    lp1 = lpMerge(&lp1, &lp2);
    TEST_ASSERT(lpLength(lp1) == 0);
    zfree(lp1);

    return 0;
}

int test_listpackLpMergeLp1Larger(int argc, char **argv, int flags) {
    /* lpMerge two listpacks - first larger than second */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp1 = createIntList();
    unsigned char *lp2 = createList();

    size_t lp1_bytes = lpBytes(lp1);
    size_t lp2_bytes = lpBytes(lp2);
    unsigned long lp1_len = lpLength(lp1);
    unsigned long lp2_len = lpLength(lp2);

    unsigned char *lp3 = lpMerge(&lp1, &lp2);
    TEST_ASSERT(lp3 == lp1);
    TEST_ASSERT(lp2 == NULL);
    TEST_ASSERT(lpLength(lp3) == (lp1_len + lp2_len));
    TEST_ASSERT(lpBytes(lp3) == (lp1_bytes + lp2_bytes - LP_HDR_SIZE - 1));
    verifyEntry(lpSeek(lp3, 0), (unsigned char *)"4294967296", 10);
    verifyEntry(lpSeek(lp3, 5), (unsigned char *)"much much longer non integer", 28);
    verifyEntry(lpSeek(lp3, 6), (unsigned char *)"hello", 5);
    verifyEntry(lpSeek(lp3, -1), (unsigned char *)"1024", 4);
    zfree(lp3);

    return 0;
}

int test_listpackLpMergeLp2Larger(int argc, char **argv, int flags) {
    /* lpMerge two listpacks - second larger than first */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp1 = createList();
    unsigned char *lp2 = createIntList();

    size_t lp1_bytes = lpBytes(lp1);
    size_t lp2_bytes = lpBytes(lp2);
    unsigned long lp1_len = lpLength(lp1);
    unsigned long lp2_len = lpLength(lp2);

    unsigned char *lp3 = lpMerge(&lp1, &lp2);
    TEST_ASSERT(lp3 == lp2);
    TEST_ASSERT(lp1 == NULL);
    TEST_ASSERT(lpLength(lp3) == (lp1_len + lp2_len));
    TEST_ASSERT(lpBytes(lp3) == (lp1_bytes + lp2_bytes - LP_HDR_SIZE - 1));
    verifyEntry(lpSeek(lp3, 0), (unsigned char *)"hello", 5);
    verifyEntry(lpSeek(lp3, 3), (unsigned char *)"1024", 4);
    verifyEntry(lpSeek(lp3, 4), (unsigned char *)"4294967296", 10);
    verifyEntry(lpSeek(lp3, -1), (unsigned char *)"much much longer non integer", 28);
    zfree(lp3);

    return 0;
}

int test_listpackLpNextRandom(int argc, char **argv, int flags) {
    /* lpNextRandom normal usage */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Create some data */
    unsigned char *lp = lpNew(0);
    unsigned char buf[100] = "asdf";
    unsigned int size = 100;
    for (size_t i = 0; i < size; i++) {
        lp = lpAppend(lp, buf, i);
    }
    TEST_ASSERT(lpLength(lp) == size);

    /* Pick a subset of the elements of every possible subset size */
    for (unsigned int count = 0; count <= size; count++) {
        unsigned int remaining = count;
        unsigned char *p = lpFirst(lp);
        unsigned char *prev = NULL;
        unsigned index = 0;
        while (remaining > 0) {
            TEST_ASSERT(p != NULL);
            p = lpNextRandom(lp, p, &index, remaining--, 0);
            TEST_ASSERT(p != NULL);
            TEST_ASSERT(p != prev);
            prev = p;
            p = lpNext(lp, p);
            index++;
        }
    }
    lpFree(lp);

    return 0;
}

int test_listpackLpNextRandomCC(int argc, char **argv, int flags) {
    /* lpNextRandom corner cases */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp = lpNew(0);
    unsigned i = 0;

    /* Pick from empty listpack returns NULL. */
    TEST_ASSERT(lpNextRandom(lp, NULL, &i, 2, 0) == NULL);

    /* Add some elements and find their pointers within the listpack. */
    lp = lpAppend(lp, (unsigned char *)"abc", 3);
    lp = lpAppend(lp, (unsigned char *)"def", 3);
    lp = lpAppend(lp, (unsigned char *)"ghi", 3);
    TEST_ASSERT(lpLength(lp) == 3);
    unsigned char *p0 = lpFirst(lp);
    unsigned char *p1 = lpNext(lp, p0);
    unsigned char *p2 = lpNext(lp, p1);
    TEST_ASSERT(lpNext(lp, p2) == NULL);

    /* Pick zero elements returns NULL. */
    i = 0;
    TEST_ASSERT(lpNextRandom(lp, lpFirst(lp), &i, 0, 0) == NULL);

    /* Pick all returns all. */
    i = 0;
    TEST_ASSERT(lpNextRandom(lp, p0, &i, 3, 0) == p0 && i == 0);
    i = 1;
    TEST_ASSERT(lpNextRandom(lp, p1, &i, 2, 0) == p1 && i == 1);
    i = 2;
    TEST_ASSERT(lpNextRandom(lp, p2, &i, 1, 0) == p2 && i == 2);

    /* Pick more than one when there's only one left returns the last one. */
    i = 2;
    TEST_ASSERT(lpNextRandom(lp, p2, &i, 42, 0) == p2 && i == 2);

    /* Pick all even elements returns p0 and p2. */
    i = 0;
    TEST_ASSERT(lpNextRandom(lp, p0, &i, 10, 1) == p0 && i == 0);
    i = 1;
    TEST_ASSERT(lpNextRandom(lp, p1, &i, 10, 1) == p2 && i == 2);

    /* Don't crash even for bad index. */
    for (int j = 0; j < 100; j++) {
        unsigned char *p;
        switch (j % 4) {
        case 0: p = p0; break;
        case 1: p = p1; break;
        case 2: p = p2; break;
        case 3: p = NULL; break;
        }
        i = j % 7;
        unsigned int remaining = j % 5;
        p = lpNextRandom(lp, p, &i, remaining, 0);
        TEST_ASSERT(p == p0 || p == p1 || p == p2 || p == NULL);
    }
    lpFree(lp);

    return 0;
}

int test_listpackRandomPairWithOneElement(int argc, char **argv, int flags) {
    /* Random pair with one element */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    listpackEntry key, val;
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char *)"abc", 3);
    lp = lpAppend(lp, (unsigned char *)"123", 3);
    lpRandomPair(lp, 1, &key, &val);
    TEST_ASSERT(memcmp(key.sval, "abc", key.slen) == 0);
    TEST_ASSERT(val.lval == 123);
    lpFree(lp);

    return 0;
}

int test_listpackRandomPairWithManyElements(int argc, char **argv, int flags) {
    /* Random pair with many elements */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    listpackEntry key, val;
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char *)"abc", 3);
    lp = lpAppend(lp, (unsigned char *)"123", 3);
    lp = lpAppend(lp, (unsigned char *)"456", 3);
    lp = lpAppend(lp, (unsigned char *)"def", 3);
    lpRandomPair(lp, 2, &key, &val);
    if (key.sval) {
        TEST_ASSERT(!memcmp(key.sval, "abc", key.slen));
        TEST_ASSERT(key.slen == 3);
        TEST_ASSERT(val.lval == 123);
    }
    if (!key.sval) {
        TEST_ASSERT(key.lval == 456);
        TEST_ASSERT(!memcmp(val.sval, "def", val.slen));
    }
    lpFree(lp);

    return 0;
}

int test_listpackRandomPairsWithOneElement(int argc, char **argv, int flags) {
    /* Random pairs with one element */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int count = 5;
    unsigned char *lp = lpNew(0);
    listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
    listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

    lp = lpAppend(lp, (unsigned char *)"abc", 3);
    lp = lpAppend(lp, (unsigned char *)"123", 3);
    lpRandomPairs(lp, count, keys, vals);
    TEST_ASSERT(memcmp(keys[4].sval, "abc", keys[4].slen) == 0);
    TEST_ASSERT(vals[4].lval == 123);
    zfree(keys);
    zfree(vals);
    lpFree(lp);

    return 0;
}

int test_listpackRandomPairsWithManyElements(int argc, char **argv, int flags) {
    /* Random pairs with many elements */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    int count = 5;
    lp = lpNew(0);
    listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
    listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

    lp = lpAppend(lp, (unsigned char *)"abc", 3);
    lp = lpAppend(lp, (unsigned char *)"123", 3);
    lp = lpAppend(lp, (unsigned char *)"456", 3);
    lp = lpAppend(lp, (unsigned char *)"def", 3);
    lpRandomPairs(lp, count, keys, vals);
    for (int i = 0; i < count; i++) {
        if (keys[i].sval) {
            TEST_ASSERT(!memcmp(keys[i].sval, "abc", keys[i].slen));
            TEST_ASSERT(keys[i].slen == 3);
            TEST_ASSERT(vals[i].lval == 123);
        }
        if (!keys[i].sval) {
            TEST_ASSERT(keys[i].lval == 456);
            TEST_ASSERT(!memcmp(vals[i].sval, "def", vals[i].slen));
        }
    }
    zfree(keys);
    zfree(vals);
    lpFree(lp);

    return 0;
}

int test_listpackRandomPairsUniqueWithOneElement(int argc, char **argv, int flags) {
    /* Random pairs unique with one element */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    unsigned picked;
    int count = 5;
    lp = lpNew(0);
    listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
    listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

    lp = lpAppend(lp, (unsigned char *)"abc", 3);
    lp = lpAppend(lp, (unsigned char *)"123", 3);
    picked = lpRandomPairsUnique(lp, count, keys, vals);
    TEST_ASSERT(picked == 1);
    TEST_ASSERT(memcmp(keys[0].sval, "abc", keys[0].slen) == 0);
    TEST_ASSERT(vals[0].lval == 123);
    zfree(keys);
    zfree(vals);
    lpFree(lp);

    return 0;
}

int test_listpackRandomPairsUniqueWithManyElements(int argc, char **argv, int flags) {
    /* Random pairs unique with many elements */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    unsigned picked;
    int count = 5;
    lp = lpNew(0);
    listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
    listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

    lp = lpAppend(lp, (unsigned char *)"abc", 3);
    lp = lpAppend(lp, (unsigned char *)"123", 3);
    lp = lpAppend(lp, (unsigned char *)"456", 3);
    lp = lpAppend(lp, (unsigned char *)"def", 3);
    picked = lpRandomPairsUnique(lp, count, keys, vals);
    TEST_ASSERT(picked == 2);
    for (int i = 0; i < 2; i++) {
        if (keys[i].sval) {
            TEST_ASSERT(!memcmp(keys[i].sval, "abc", keys[i].slen));
            TEST_ASSERT(keys[i].slen == 3);
            TEST_ASSERT(vals[i].lval == 123);
        }
        if (!keys[i].sval) {
            TEST_ASSERT(keys[i].lval == 456);
            TEST_ASSERT(!memcmp(vals[i].sval, "def", vals[i].slen));
        }
    }
    zfree(keys);
    zfree(vals);
    lpFree(lp);

    return 0;
}

int test_listpackPushVariousEncodings(int argc, char **argv, int flags) {
    /* push various encodings */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = lpNew(0);

    /* Push integer encode element using lpAppend */
    lp = lpAppend(lp, (unsigned char *)"127", 3);
    TEST_ASSERT(LP_ENCODING_IS_7BIT_UINT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)"4095", 4);
    TEST_ASSERT(LP_ENCODING_IS_13BIT_INT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)"32767", 5);
    TEST_ASSERT(LP_ENCODING_IS_16BIT_INT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)"8388607", 7);
    TEST_ASSERT(LP_ENCODING_IS_24BIT_INT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)"2147483647", 10);
    TEST_ASSERT(LP_ENCODING_IS_32BIT_INT(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)"9223372036854775807", 19);
    TEST_ASSERT(LP_ENCODING_IS_64BIT_INT(lpLast(lp)[0]));

    /* Push integer encode element using lpAppendInteger */
    lp = lpAppendInteger(lp, 127);
    TEST_ASSERT(LP_ENCODING_IS_7BIT_UINT(lpLast(lp)[0]));
    verifyEntry(lpLast(lp), (unsigned char *)"127", 3);
    lp = lpAppendInteger(lp, 4095);
    verifyEntry(lpLast(lp), (unsigned char *)"4095", 4);
    TEST_ASSERT(LP_ENCODING_IS_13BIT_INT(lpLast(lp)[0]));
    lp = lpAppendInteger(lp, 32767);
    verifyEntry(lpLast(lp), (unsigned char *)"32767", 5);
    TEST_ASSERT(LP_ENCODING_IS_16BIT_INT(lpLast(lp)[0]));
    lp = lpAppendInteger(lp, 8388607);
    verifyEntry(lpLast(lp), (unsigned char *)"8388607", 7);
    TEST_ASSERT(LP_ENCODING_IS_24BIT_INT(lpLast(lp)[0]));
    lp = lpAppendInteger(lp, 2147483647);
    verifyEntry(lpLast(lp), (unsigned char *)"2147483647", 10);
    TEST_ASSERT(LP_ENCODING_IS_32BIT_INT(lpLast(lp)[0]));
    lp = lpAppendInteger(lp, 9223372036854775807);
    verifyEntry(lpLast(lp), (unsigned char *)"9223372036854775807", 19);
    TEST_ASSERT(LP_ENCODING_IS_64BIT_INT(lpLast(lp)[0]));

    /* string encode */
    unsigned char *str = zmalloc(65535);
    memset(str, 0, 65535);
    lp = lpAppend(lp, (unsigned char *)str, 63);
    TEST_ASSERT(LP_ENCODING_IS_6BIT_STR(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)str, 4095);
    TEST_ASSERT(LP_ENCODING_IS_12BIT_STR(lpLast(lp)[0]));
    lp = lpAppend(lp, (unsigned char *)str, 65535);
    TEST_ASSERT(LP_ENCODING_IS_32BIT_STR(lpLast(lp)[0]));
    zfree(str);
    lpFree(lp);

    return 0;
}

int test_listpackLpFind(int argc, char **argv, int flags) {
    /* Test lpFind */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    TEST_ASSERT(lpFind(lp, lpFirst(lp), (unsigned char *)"abc", 3, 0) == NULL);
    verifyEntry(lpFind(lp, lpFirst(lp), (unsigned char *)"hello", 5, 0), (unsigned char *)"hello", 5);
    verifyEntry(lpFind(lp, lpFirst(lp), (unsigned char *)"1024", 4, 0), (unsigned char *)"1024", 4);
    lpFree(lp);

    return 0;
}

int test_listpackLpValidateIntegrity(int argc, char **argv, int flags) {
    /* Test lpValidateIntegrity */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = createList();
    long count = 0;
    TEST_ASSERT(lpValidateIntegrity(lp, lpBytes(lp), 1, lpValidation, &count) == 1);
    lpFree(lp);

    return 0;
}

int test_listpackNumberOfElementsExceedsLP_HDR_NUMELE_UNKNOWN(int argc, char **argv, int flags) {
    /* Test number of elements exceeds LP_HDR_NUMELE_UNKNOWN */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp;

    lp = lpNew(0);
    for (int i = 0; i < LP_HDR_NUMELE_UNKNOWN + 1; i++) lp = lpAppend(lp, (unsigned char *)"1", 1);

    TEST_ASSERT(lpGetNumElements(lp) == LP_HDR_NUMELE_UNKNOWN);
    TEST_ASSERT(lpLength(lp) == LP_HDR_NUMELE_UNKNOWN + 1);

    lp = lpDeleteRange(lp, -2, 2);
    TEST_ASSERT(lpGetNumElements(lp) == LP_HDR_NUMELE_UNKNOWN);
    TEST_ASSERT(lpLength(lp) == LP_HDR_NUMELE_UNKNOWN - 1);
    TEST_ASSERT(lpGetNumElements(lp) == LP_HDR_NUMELE_UNKNOWN - 1); /* update length after lpLength */
    lpFree(lp);

    return 0;
}

int test_listpackStressWithRandom(int argc, char **argv, int flags) {
    /* Stress with random payloads of different encoding */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned char *lp, *vstr;
    int64_t vlen;
    unsigned char intbuf[LP_INTBUF_SIZE];

    int accurate = (flags & UNIT_TEST_ACCURATE);

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
                default: TEST_ASSERT(NULL);
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
                TEST_ASSERT(NULL);
            }
        }

        TEST_ASSERT(listLength(ref) == lpLength(lp));
        for (j = 0; j < len; j++) {
            /* Naive way to get elements, but similar to the stresser
             * executed from the Tcl test suite. */
            p = lpSeek(lp, j);
            refnode = listIndex(ref, j);

            vstr = lpGet(p, &vlen, intbuf);
            TEST_ASSERT(memcmp(vstr, listNodeValue(refnode), vlen) == 0);
        }
        lpFree(lp);
        listRelease(ref);
    }
    printf("Done. usec=%lld\n\n", usec() - start);

    return 0;
}

int test_listpackSTressWithVariableSize(int argc, char **argv, int flags) {
    /* Stress with variable listpack size */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int accurate = (flags & UNIT_TEST_ACCURATE);

    unsigned long long start = usec();
    int maxsize = accurate ? 16384 : 16;
    stress(0, 100000, maxsize, 256);
    stress(1, 100000, maxsize, 256);
    printf("Done. usec=%lld\n\n", usec() - start);

    return 0;
}

/* Benchmarks */
static unsigned char *lp;
static int accurate, iteration;

int test_listpackBenchmarkInit(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);

    accurate = (flags & UNIT_TEST_ACCURATE);
    iteration = accurate ? 100000 : 100;
    lp = lpNew(0);

    return 0;
}

int test_listpackBenchmarkLpAppend(int argc, char **argv, int flags) {
    /* Benchmark lpAppend */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned long long start = usec();
    for (int i = 0; i < iteration; i++) {
        char buf[4096] = "asdf";
        lp = lpAppend(lp, (unsigned char *)buf, 4);
        lp = lpAppend(lp, (unsigned char *)buf, 40);
        lp = lpAppend(lp, (unsigned char *)buf, 400);
        lp = lpAppend(lp, (unsigned char *)buf, 4000);
        lp = lpAppend(lp, (unsigned char *)"1", 1);
        lp = lpAppend(lp, (unsigned char *)"10", 2);
        lp = lpAppend(lp, (unsigned char *)"100", 3);
        lp = lpAppend(lp, (unsigned char *)"1000", 4);
        lp = lpAppend(lp, (unsigned char *)"10000", 5);
        lp = lpAppend(lp, (unsigned char *)"100000", 6);
    }
    printf("Done. usec=%lld\n", usec() - start);
    return 0;
}

int test_listpackBenchmarkLpFindString(int argc, char **argv, int flags) {
    /* Benchmark lpFind string */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *fptr = lpFirst(lp);
        fptr = lpFind(lp, fptr, (unsigned char *)"nothing", 7, 1);
    }
    printf("Done. usec=%lld\n", usec() - start);
    return 0;
}

int test_listpackBenchmarkLpFindNumber(int argc, char **argv, int flags) {
    /* Benchmark lpFind number */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *fptr = lpFirst(lp);
        fptr = lpFind(lp, fptr, (unsigned char *)"99999", 5, 1);
    }
    printf("Done. usec=%lld\n", usec() - start);
    return 0;
}

int test_listpackBenchmarkLpSeek(int argc, char **argv, int flags) {
    /* Benchmark lpSeek */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        lpSeek(lp, 99999);
    }
    printf("Done. usec=%lld\n", usec() - start);
    return 0;
}

int test_listpackBenchmarkLpValidateIntegrity(int argc, char **argv, int flags) {
    /* Benchmark lpValidateIntegrity */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        lpValidateIntegrity(lp, lpBytes(lp), 1, NULL, NULL);
    }
    printf("Done. usec=%lld\n", usec() - start);
    return 0;
}

int test_listpackBenchmarkLpCompareWithString(int argc, char **argv, int flags) {
    /* Benchmark lpCompare with string */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *eptr = lpSeek(lp, 0);
        while (eptr != NULL) {
            lpCompare(eptr, (unsigned char *)"nothing", 7);
            eptr = lpNext(lp, eptr);
        }
    }
    printf("Done. usec=%lld\n", usec() - start);
    return 0;
}

int test_listpackBenchmarkLpCompareWithNumber(int argc, char **argv, int flags) {
    /* Benchmark lpCompare with number */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned long long start = usec();
    for (int i = 0; i < 2000; i++) {
        unsigned char *eptr = lpSeek(lp, 0);
        while (eptr != NULL) {
            lpCompare(lp, (unsigned char *)"99999", 5);
            eptr = lpNext(lp, eptr);
        }
    }
    printf("Done. usec=%lld\n", usec() - start);
    return 0;
}

int test_listpackBenchmarkFree(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    lpFree(lp);
    return 0;
}
