#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "test_help.h"
#include <sys/time.h>
#include <stdlib.h>

#include "../zmalloc.h"
#include "../listpack.h"
#include "../quicklist.c"

static int options[] = {0, 1, 2, 3, 4, 5, 6, 10};
static int option_count = 8;

static int fills[] = {-5, -4, -3, -2, -1, 0,
                      1, 2, 32, 66, 128, 999};
static int fill_count = 12;
static long long runtime[8];
static unsigned int err = 0;

/*-----------------------------------------------------------------------------
 * Unit Function
 *----------------------------------------------------------------------------*/
/* Return the UNIX time in microseconds */
static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
static long long mstime(void) {
    return ustime() / 1000;
}

/* Generate new string concatenating integer i against string 'prefix' */
static char *genstr(char *prefix, int i) {
    static char result[64] = {0};
    snprintf(result, sizeof(result), "%s%d", prefix, i);
    return result;
}

__attribute__((unused)) static void randstring(unsigned char *target, size_t sz) {
    size_t p = 0;
    int minval, maxval;
    switch (rand() % 3) {
    case 0:
        minval = 'a';
        maxval = 'z';
        break;
    case 1:
        minval = '0';
        maxval = '9';
        break;
    case 2:
        minval = 'A';
        maxval = 'Z';
        break;
    default:
        abort();
    }

    while (p < sz)
        target[p++] = minval + rand() % (maxval - minval + 1);
}

#define TEST(name) printf("test — %s\n", name);

#define QL_TEST_VERBOSE 0
static void ql_info(quicklist *ql) {
#if QL_TEST_VERBOSE
    TEST_PRINT_INFO("Container length: %lu\n", ql->len);
    TEST_PRINT_INFO("Container size: %lu\n", ql->count);
    if (ql->head)
        TEST_PRINT_INFO("\t(zsize head: %lu)\n", lpLength(ql->head->entry));
    if (ql->tail)
        TEST_PRINT_INFO("\t(zsize tail: %lu)\n", lpLength(ql->tail->entry));
#else
    UNUSED(ql);
#endif
}

/* Iterate over an entire quicklist.
 * Print the list if 'print' == 1.
 *
 * Returns physical count of elements found by iterating over the list. */
static int _itrprintr(quicklist *ql, int print, int forward) {
    quicklistIter *iter =
        quicklistGetIterator(ql, forward ? AL_START_HEAD : AL_START_TAIL);
    quicklistEntry entry;
    int i = 0;
    int p = 0;
    quicklistNode *prev = NULL;
    while (quicklistNext(iter, &entry)) {
        if (entry.node != prev) {
            /* Count the number of list nodes too */
            p++;
            prev = entry.node;
        }
        if (print) {
            int size = (entry.sz > (1 << 20)) ? 1 << 20 : entry.sz;
            TEST_PRINT_INFO("[%3d (%2d)]: [%.*s] (%lld)\n", i, p, size,
                            (char *)entry.value, entry.longval);
        }
        i++;
    }
    quicklistReleaseIterator(iter);
    return i;
}

static int itrprintr(quicklist *ql, int print) {
    return _itrprintr(ql, print, 1);
}

static int itrprintr_rev(quicklist *ql, int print) {
    return _itrprintr(ql, print, 0);
}

#define ql_verify(a, b, c, d, e)                    \
    do {                                            \
        err += _ql_verify((a), (b), (c), (d), (e)); \
    } while (0)

static int _ql_verify_compress(quicklist *ql) {
    int errors = 0;
    if (quicklistAllowsCompression(ql)) {
        quicklistNode *node = ql->head;
        unsigned int low_raw = ql->compress;
        unsigned int high_raw = ql->len - ql->compress;

        for (unsigned int at = 0; at < ql->len; at++, node = node->next) {
            if (node && (at < low_raw || at >= high_raw)) {
                if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                    TEST_PRINT_INFO("Incorrect compression: node %d is "
                                    "compressed at depth %d ((%u, %u); total "
                                    "nodes: %lu; size: %zu; recompress: %d)",
                                    at, ql->compress, low_raw, high_raw, ql->len, node->sz,
                                    node->recompress);
                    errors++;
                }
            } else {
                if (node->encoding != QUICKLIST_NODE_ENCODING_LZF &&
                    !node->attempted_compress) {
                    TEST_PRINT_INFO("Incorrect non-compression: node %d is NOT "
                                    "compressed at depth %d ((%u, %u); total "
                                    "nodes: %lu; size: %zu; recompress: %d; attempted: %d)",
                                    at, ql->compress, low_raw, high_raw, ql->len, node->sz,
                                    node->recompress, node->attempted_compress);
                    errors++;
                }
            }
        }
    }
    return errors;
}

/* Verify list metadata matches physical list contents. */
static int _ql_verify(quicklist *ql, uint32_t len, uint32_t count, uint32_t head_count, uint32_t tail_count) {
    int errors = 0;

    ql_info(ql);
    if (len != ql->len) {
        TEST_PRINT_INFO("quicklist length wrong: expected %d, got %lu", len, ql->len);
        errors++;
    }

    if (count != ql->count) {
        TEST_PRINT_INFO("quicklist count wrong: expected %d, got %lu", count, ql->count);
        errors++;
    }

    int loopr = itrprintr(ql, 0);
    if (loopr != (int)ql->count) {
        TEST_PRINT_INFO("quicklist cached count not match actual count: expected %lu, got "
                        "%d",
                        ql->count, loopr);
        errors++;
    }

    int rloopr = itrprintr_rev(ql, 0);
    if (loopr != rloopr) {
        TEST_PRINT_INFO("quicklist has different forward count than reverse count!  "
                        "Forward count is %d, reverse count is %d.",
                        loopr, rloopr);
        errors++;
    }

    if (ql->len == 0 && !errors) {
        return errors;
    }

    if (ql->head && head_count != ql->head->count &&
        head_count != lpLength(ql->head->entry)) {
        TEST_PRINT_INFO("quicklist head count wrong: expected %d, "
                        "got cached %d vs. actual %lu",
                        head_count, ql->head->count, lpLength(ql->head->entry));
        errors++;
    }

    if (ql->tail && tail_count != ql->tail->count &&
        tail_count != lpLength(ql->tail->entry)) {
        TEST_PRINT_INFO("quicklist tail count wrong: expected %d, "
                        "got cached %u vs. actual %lu",
                        tail_count, ql->tail->count, lpLength(ql->tail->entry));
        errors++;
    }

    errors += _ql_verify_compress(ql);
    return errors;
}

/* Release iterator and verify compress correctly. */
static void ql_release_iterator(quicklistIter *iter) {
    quicklist *ql = NULL;
    if (iter) ql = iter->quicklist;
    quicklistReleaseIterator(iter);
    if (ql && _ql_verify_compress(ql)) {
        abort();
    }
}

/*-----------------------------------------------------------------------------
 * Quicklist Unit Test
 *----------------------------------------------------------------------------*/
int test_quicklistCreateList(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    TEST("create list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistAddToTailOfEmptyList(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("add to tail of empty list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushTail(ql, "hello", 6);
        /* 1 for head and 1 for tail because 1 node = head = tail */
        ql_verify(ql, 1, 1, 1, 1);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistAddToHeadOfEmptyList(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("add to head of empty list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, "hello", 6);
        /* 1 for head and 1 for tail because 1 node = head = tail */
        ql_verify(ql, 1, 1, 1, 1);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistAddToTail5xAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("add to tail 5x at compress");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 5; i++) quicklistPushTail(ql, genstr("hello", i), 32);
            if (ql->count != 5) {
                err++;
            };
            if (fills[f] == 32) ql_verify(ql, 1, 5, 5, 5);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistAddToHead5xAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("add to head 5x at compress");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 5; i++) quicklistPushHead(ql, genstr("hello", i), 32);
            if (ql->count != 5) {
                err++;
            };
            if (fills[f] == 32) ql_verify(ql, 1, 5, 5, 5);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistAddToTail500xAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("add to tail 500x at compress");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i), 64);
            if (ql->count != 500) {
                err++;
            };
            if (fills[f] == 32) ql_verify(ql, 16, 500, 32, 20);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistAddToHead500xAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("add to head 500x at compress");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++) quicklistPushHead(ql, genstr("hello", i), 32);
            if (ql->count != 500) {
                err++;
            };
            if (fills[f] == 32) ql_verify(ql, 16, 500, 20, 32);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistRotateEmpty(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("rotate empty");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistRotate(ql);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistComprassionPlainNode(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("Comprassion Plain node");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            size_t large_limit = (fills[f] < 0) ? quicklistNodeNegFillLimit(fills[f]) + 1 : SIZE_SAFETY_LIMIT + 1;

            char buf[large_limit];
            quicklist *ql = quicklistNew(fills[f], 1);
            for (int i = 0; i < 500; i++) {
                /* Set to 256 to allow the node to be triggered to compress,
                 * if it is less than 48(nocompress), the test will be successful. */
                snprintf(buf, sizeof(buf), "hello%d", i);
                quicklistPushHead(ql, buf, large_limit);
            }

            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
            quicklistEntry entry;
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                TEST_ASSERT(QL_NODE_IS_PLAIN(entry.node));
                snprintf(buf, sizeof(buf), "hello%d", i);
                if (strcmp((char *)entry.value, buf)) {
                    TEST_PRINT_INFO("value [%s] didn't match [%s] at position %d", entry.value, buf, i);
                    err++;
                }
                i++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistNextPlainNode(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("NEXT plain node");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            size_t large_limit = (fills[f] < 0) ? quicklistNodeNegFillLimit(fills[f]) + 1 : SIZE_SAFETY_LIMIT + 1;
            quicklist *ql = quicklistNew(fills[f], options[_i]);

            char buf[large_limit];
            memcpy(buf, "plain", 5);
            quicklistPushHead(ql, buf, large_limit);
            quicklistPushHead(ql, buf, large_limit);
            quicklistPushHead(ql, "packed3", 7);
            quicklistPushHead(ql, "packed4", 7);
            quicklistPushHead(ql, buf, large_limit);

            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);

            while (quicklistNext(iter, &entry) != 0) {
                if (QL_NODE_IS_PLAIN(entry.node))
                    TEST_ASSERT(!memcmp(entry.value, "plain", 5));
                else
                    TEST_ASSERT(!memcmp(entry.value, "packed", 6));
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistRotatePlainNode(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("rotate plain node");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            size_t large_limit = (fills[f] < 0) ? quicklistNodeNegFillLimit(fills[f]) + 1 : SIZE_SAFETY_LIMIT + 1;

            unsigned char *data = NULL;
            size_t sz;
            long long lv;
            int i = 0;
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char buf[large_limit];
            memcpy(buf, "hello1", 6);
            quicklistPushHead(ql, buf, large_limit);
            memcpy(buf, "hello4", 6);
            quicklistPushHead(ql, buf, large_limit);
            memcpy(buf, "hello3", 6);
            quicklistPushHead(ql, buf, large_limit);
            memcpy(buf, "hello2", 6);
            quicklistPushHead(ql, buf, large_limit);
            quicklistRotate(ql);

            for (i = 1; i < 5; i++) {
                TEST_ASSERT(QL_NODE_IS_PLAIN(ql->tail));
                quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                int temp_char = data[5];
                zfree(data);
                TEST_ASSERT(temp_char == ('0' + i));
            }

            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistRotateOneValOnce(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("rotate one val once");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            quicklistPushHead(ql, "hello", 6);
            quicklistRotate(ql);
            /* Ignore compression verify because listpack is
             * too small to compress. */
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistRotate500Val5000TimesAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("rotate 500 val 5000 times at compress");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            quicklistPushHead(ql, "900", 3);
            quicklistPushHead(ql, "7000", 4);
            quicklistPushHead(ql, "-1200", 5);
            quicklistPushHead(ql, "42", 2);
            for (int i = 0; i < 500; i++) quicklistPushHead(ql, genstr("hello", i), 64);
            ql_info(ql);
            for (int i = 0; i < 5000; i++) {
                ql_info(ql);
                quicklistRotate(ql);
            }
            if (fills[f] == 1)
                ql_verify(ql, 504, 504, 1, 1);
            else if (fills[f] == 2)
                ql_verify(ql, 252, 504, 2, 2);
            else if (fills[f] == 32)
                ql_verify(ql, 16, 504, 32, 24);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistPopEmpty(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("pop empty");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPop(ql, QUICKLIST_HEAD, NULL, NULL, NULL);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistPop1StringFrom1(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("pop 1 string from 1");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        char *populate = genstr("hello", 331);
        quicklistPushHead(ql, populate, 32);
        unsigned char *data;
        size_t sz;
        long long lv;
        ql_info(ql);
        TEST_ASSERT(quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv));
        TEST_ASSERT(data != NULL);
        TEST_ASSERT(sz == 32);
        if (strcmp(populate, (char *)data)) {
            int size = sz;
            TEST_PRINT_INFO("Pop'd value (%.*s) didn't equal original value (%s)", size, data, populate);
            err++;
        }
        zfree(data);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistPopHead1NumberFrom1(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("pop head 1 number from 1");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, "55513", 5);
        unsigned char *data;
        size_t sz;
        long long lv;
        ql_info(ql);
        TEST_ASSERT(quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv));
        TEST_ASSERT(data == NULL);
        TEST_ASSERT(lv == 55513);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistPopHead500From500(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("pop head 500 from 500");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 500; i++) quicklistPushHead(ql, genstr("hello", i), 32);
        ql_info(ql);
        for (int i = 0; i < 500; i++) {
            unsigned char *data;
            size_t sz;
            long long lv;
            int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            TEST_ASSERT(ret == 1);
            TEST_ASSERT(data != NULL);
            TEST_ASSERT(sz == 32);
            if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                int size = sz;
                TEST_PRINT_INFO("Pop'd value (%.*s) didn't equal original value (%s)", size, data, genstr("hello", 499 - i));
                err++;
            }
            zfree(data);
        }
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistPopHead5000From500(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("pop head 5000 from 500");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 500; i++) quicklistPushHead(ql, genstr("hello", i), 32);
        for (int i = 0; i < 5000; i++) {
            unsigned char *data;
            size_t sz;
            long long lv;
            int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            if (i < 500) {
                TEST_ASSERT(ret == 1);
                TEST_ASSERT(data != NULL);
                TEST_ASSERT(sz == 32);
                if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                    int size = sz;
                    TEST_PRINT_INFO("Pop'd value (%.*s) didn't equal original value "
                                    "(%s)",
                                    size, data, genstr("hello", 499 - i));
                    err++;
                }
                zfree(data);
            } else {
                TEST_ASSERT(ret == 0);
            }
        }
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistIterateForwardOver500List(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("iterate forward over 500 list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++) quicklistPushHead(ql, genstr("hello", i), 32);
        quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
        quicklistEntry entry;
        int i = 499, count = 0;
        while (quicklistNext(iter, &entry)) {
            char *h = genstr("hello", i);
            if (strcmp((char *)entry.value, h)) {
                TEST_PRINT_INFO("value [%s] didn't match [%s] at position %d", entry.value, h, i);
                err++;
            }
            i--;
            count++;
        }
        if (count != 500) {
            TEST_PRINT_INFO("Didn't iterate over exactly 500 elements (%d)", i);
            err++;
        }
        ql_verify(ql, 16, 500, 20, 32);
        ql_release_iterator(iter);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistIterateReverseOver500List(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("iterate reverse over 500 list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++) quicklistPushHead(ql, genstr("hello", i), 32);
        quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
        quicklistEntry entry;
        int i = 0;
        while (quicklistNext(iter, &entry)) {
            char *h = genstr("hello", i);
            if (strcmp((char *)entry.value, h)) {
                TEST_PRINT_INFO("value [%s] didn't match [%s] at position %d", entry.value, h, i);
                err++;
            }
            i++;
        }
        if (i != 500) {
            TEST_PRINT_INFO("Didn't iterate over exactly 500 elements (%d)", i);
            err++;
        }
        ql_verify(ql, 16, 500, 20, 32);
        ql_release_iterator(iter);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistInsertAfter1Element(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("insert after 1 element");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, "hello", 6);
        quicklistEntry entry;
        iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        quicklistInsertAfter(iter, &entry, "abc", 4);
        ql_release_iterator(iter);
        ql_verify(ql, 1, 2, 2, 2);

        /* verify results */
        iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        int sz = entry.sz;
        if (strncmp((char *)entry.value, "hello", 5)) {
            TEST_PRINT_INFO("Value 0 didn't match, instead got: %.*s", sz, entry.value);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
        sz = entry.sz;
        if (strncmp((char *)entry.value, "abc", 3)) {
            TEST_PRINT_INFO("Value 1 didn't match, instead got: %.*s", sz, entry.value);
            err++;
        }
        ql_release_iterator(iter);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistInsertBefore1Element(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("insert before 1 element");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, "hello", 6);
        quicklistEntry entry;
        iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        quicklistInsertBefore(iter, &entry, "abc", 4);
        ql_release_iterator(iter);
        ql_verify(ql, 1, 2, 2, 2);

        /* verify results */
        iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        int sz = entry.sz;
        if (strncmp((char *)entry.value, "abc", 3)) {
            TEST_PRINT_INFO("Value 0 didn't match, instead got: %.*s", sz, entry.value);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
        sz = entry.sz;
        if (strncmp((char *)entry.value, "hello", 5)) {
            TEST_PRINT_INFO("Value 1 didn't match, instead got: %.*s", sz, entry.value);
            err++;
        }
        ql_release_iterator(iter);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistInsertHeadWhileHeadNodeIsFull(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("insert head while head node is full");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(4, options[_i]);
        for (int i = 0; i < 10; i++) quicklistPushTail(ql, genstr("hello", i), 6);
        quicklistSetFill(ql, -1);
        quicklistEntry entry;
        iter = quicklistGetIteratorEntryAtIdx(ql, -10, &entry);
        char buf[4096] = {0};
        quicklistInsertBefore(iter, &entry, buf, 4096);
        ql_release_iterator(iter);
        ql_verify(ql, 4, 11, 1, 2);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistInsertTailWhileTailNodeIsFull(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("insert tail while tail node is full");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(4, options[_i]);
        for (int i = 0; i < 10; i++) quicklistPushHead(ql, genstr("hello", i), 6);
        quicklistSetFill(ql, -1);
        quicklistEntry entry;
        iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
        char buf[4096] = {0};
        quicklistInsertAfter(iter, &entry, buf, 4096);
        ql_release_iterator(iter);
        ql_verify(ql, 4, 11, 2, 1);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistInsertOnceInElementsWhileIteratingAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("insert once in elements while iterating at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            quicklistPushTail(ql, "abc", 3);
            quicklistSetFill(ql, 1);
            quicklistPushTail(ql, "def", 3); /* force to unique node */
            quicklistSetFill(ql, f);
            quicklistPushTail(ql, "bob", 3); /* force to reset for +3 */
            quicklistPushTail(ql, "foo", 3);
            quicklistPushTail(ql, "zoo", 3);

            itrprintr(ql, 0);
            /* insert "bar" before "bob" while iterating over list. */
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
            quicklistEntry entry;
            while (quicklistNext(iter, &entry)) {
                if (!strncmp((char *)entry.value, "bob", 3)) {
                    /* Insert as fill = 1 so it spills into new node. */
                    quicklistInsertBefore(iter, &entry, "bar", 3);
                    break; /* didn't we fix insert-while-iterating? */
                }
            }
            ql_release_iterator(iter);
            itrprintr(ql, 0);

            /* verify results */
            iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
            int sz = entry.sz;

            if (strncmp((char *)entry.value, "abc", 3)) {
                TEST_PRINT_INFO("Value 0 didn't match, instead got: %.*s", sz, entry.value);
                err++;
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
            if (strncmp((char *)entry.value, "def", 3)) {
                TEST_PRINT_INFO("Value 1 didn't match, instead got: %.*s", sz, entry.value);
                err++;
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
            if (strncmp((char *)entry.value, "bar", 3)) {
                TEST_PRINT_INFO("Value 2 didn't match, instead got: %.*s", sz, entry.value);
                err++;
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
            if (strncmp((char *)entry.value, "bob", 3)) {
                TEST_PRINT_INFO("Value 3 didn't match, instead got: %.*s", sz, entry.value);
                err++;
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
            if (strncmp((char *)entry.value, "foo", 3)) {
                TEST_PRINT_INFO("Value 4 didn't match, instead got: %.*s", sz, entry.value);
                err++;
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
            if (strncmp((char *)entry.value, "zoo", 3)) {
                TEST_PRINT_INFO("Value 5 didn't match, instead got: %.*s", sz, entry.value);
                err++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }

    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistInsertBefore250NewInMiddleOf500ElementsAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("insert [before] 250 new in middle of 500 elements at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i), 32);
            for (int i = 0; i < 250; i++) {
                quicklistEntry entry;
                iter = quicklistGetIteratorEntryAtIdx(ql, 250, &entry);
                quicklistInsertBefore(iter, &entry, genstr("abc", i), 32);
                ql_release_iterator(iter);
            }
            if (fills[f] == 32) ql_verify(ql, 25, 750, 32, 20);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistInsertAfter250NewInMiddleOf500ElementsAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("insert [after] 250 new in middle of 500 elements at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++) quicklistPushHead(ql, genstr("hello", i), 32);
            for (int i = 0; i < 250; i++) {
                quicklistEntry entry;
                iter = quicklistGetIteratorEntryAtIdx(ql, 250, &entry);
                quicklistInsertAfter(iter, &entry, genstr("abc", i), 32);
                ql_release_iterator(iter);
            }

            if (ql->count != 750) {
                TEST_PRINT_INFO("List size not 750, but rather %ld", ql->count);
                err++;
            }

            if (fills[f] == 32) ql_verify(ql, 26, 750, 20, 32);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDuplicateEmptyList(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("duplicate empty list");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        ql_verify(ql, 0, 0, 0, 0);
        quicklist *copy = quicklistDup(ql);
        ql_verify(copy, 0, 0, 0, 0);
        quicklistRelease(ql);
        quicklistRelease(copy);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDuplicateListOf1Element(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("duplicate list of 1 element");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, genstr("hello", 3), 32);
        ql_verify(ql, 1, 1, 1, 1);
        quicklist *copy = quicklistDup(ql);
        ql_verify(copy, 1, 1, 1, 1);
        quicklistRelease(ql);
        quicklistRelease(copy);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }

    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDuplicateListOf500(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("duplicate list of 500");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++) quicklistPushHead(ql, genstr("hello", i), 32);
        ql_verify(ql, 16, 500, 20, 32);

        quicklist *copy = quicklistDup(ql);
        ql_verify(copy, 16, 500, 20, 32);
        quicklistRelease(ql);
        quicklistRelease(copy);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }

    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistIndex1200From500ListAtFill(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("index 1,200 from 500 list at fill at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistEntry entry;
            iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
            if (strcmp((char *)entry.value, "hello2") != 0) {
                TEST_PRINT_INFO("Value: %s", entry.value);
                err++;
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 200, &entry);
            if (strcmp((char *)entry.value, "hello201") != 0) {
                TEST_PRINT_INFO("Value: %s", entry.value);
                err++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistIndex12From500ListAtFill(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("index -1,-2 from 500 list at fill at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistEntry entry;
            iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
            if (strcmp((char *)entry.value, "hello500") != 0) {
                TEST_PRINT_INFO("Value: %s", entry.value);
                err++;
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, -2, &entry);
            if (strcmp((char *)entry.value, "hello499") != 0) {
                TEST_PRINT_INFO("Value: %s", entry.value);
                err++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistIndex100From500ListAtFill(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("index -100 from 500 list at fill at compress");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistEntry entry;
            iter = quicklistGetIteratorEntryAtIdx(ql, -100, &entry);
            if (strcmp((char *)entry.value, "hello401") != 0) {
                TEST_PRINT_INFO("Value: %s", entry.value);
                err++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistIndexTooBig1From50ListAtFill(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("index too big +1 from 50 list at fill at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 50; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistEntry entry;
            int sz = entry.sz;
            iter = quicklistGetIteratorEntryAtIdx(ql, 50, &entry);
            if (iter) {
                TEST_PRINT_INFO("Index found at 50 with 50 list: %.*s", sz, entry.value);
                err++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }


        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDeleteRangeEmptyList(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("delete range empty list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();

        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistDelRange(ql, 5, 20);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDeleteRangeOfEntireNodeInListOfOneNode(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("delete range of entire node in list of one node");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 32; i++) quicklistPushHead(ql, genstr("hello", i), 32);
        ql_verify(ql, 1, 32, 32, 32);
        quicklistDelRange(ql, 0, 32);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDeleteRangeOfEntireNodeWithOverflowCounts(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("delete range of entire node with overflow counts");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 32; i++) quicklistPushHead(ql, genstr("hello", i), 32);
        ql_verify(ql, 1, 32, 32, 32);
        quicklistDelRange(ql, 0, 128);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);


        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDeleteMiddle100Of500List(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("delete middle 100 of 500 list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 16, 500, 32, 20);
        quicklistDelRange(ql, 200, 100);
        ql_verify(ql, 14, 400, 32, 20);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDeleteLessThanFillButAcrossNodes(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("delete less than fill but across nodes");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 16, 500, 32, 20);
        quicklistDelRange(ql, 60, 10);
        ql_verify(ql, 16, 490, 32, 20);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDeleteNegative1From500List(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("delete negative 1 from 500 list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 16, 500, 32, 20);
        quicklistDelRange(ql, -1, 1);
        ql_verify(ql, 16, 499, 32, 19);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDeleteNegative1From500ListWithOverflowCounts(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("delete negative 1 from 500 list with overflow counts");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 16, 500, 32, 20);
        quicklistDelRange(ql, -1, 128);
        ql_verify(ql, 16, 499, 32, 19);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDeleteNegative100From500List(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("delete negative 100 from 500 list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
        quicklistDelRange(ql, -100, 100);
        ql_verify(ql, 13, 400, 32, 16);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistDelete10Count5From50List(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("delete -10 count 5 from 50 list");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 50; i++) quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 2, 50, 32, 18);
        quicklistDelRange(ql, -10, 5);
        ql_verify(ql, 2, 45, 32, 13);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistNumbersOnlyListRead(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("numbers only list read");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushTail(ql, "1111", 4);
        quicklistPushTail(ql, "2222", 4);
        quicklistPushTail(ql, "3333", 4);
        quicklistPushTail(ql, "4444", 4);
        ql_verify(ql, 1, 4, 4, 4);
        quicklistEntry entry;
        iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        if (entry.longval != 1111) {
            TEST_PRINT_INFO("Not 1111, %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
        if (entry.longval != 2222) {
            TEST_PRINT_INFO("Not 2222, %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
        if (entry.longval != 3333) {
            TEST_PRINT_INFO("Not 3333, %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
        if (entry.longval != 4444) {
            TEST_PRINT_INFO("Not 4444, %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
        if (iter) {
            TEST_PRINT_INFO("Index past elements: %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
        if (entry.longval != 4444) {
            TEST_PRINT_INFO("Not 4444 (reverse), %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -2, &entry);
        if (entry.longval != 3333) {
            TEST_PRINT_INFO("Not 3333 (reverse), %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -3, &entry);
        if (entry.longval != 2222) {
            TEST_PRINT_INFO("Not 2222 (reverse), %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -4, &entry);
        if (entry.longval != 1111) {
            TEST_PRINT_INFO("Not 1111 (reverse), %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -5, &entry);
        if (iter) {
            TEST_PRINT_INFO("Index past elements (reverse), %lld", entry.longval);
            err++;
        }
        ql_release_iterator(iter);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistNumbersLargerListRead(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("numbers larger list read");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        char num[32];
        long long nums[5000];
        for (int i = 0; i < 5000; i++) {
            nums[i] = -5157318210846258176 + i;
            int sz = ll2string(num, sizeof(num), nums[i]);
            quicklistPushTail(ql, num, sz);
        }
        quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
        quicklistEntry entry;
        for (int i = 0; i < 5000; i++) {
            iter = quicklistGetIteratorEntryAtIdx(ql, i, &entry);
            if (entry.longval != nums[i]) {
                TEST_PRINT_INFO("[%d] Not longval %lld but rather %lld", i, nums[i], entry.longval);
                err++;
            }
            entry.longval = 0xdeadbeef;
            ql_release_iterator(iter);
        }
        iter = quicklistGetIteratorEntryAtIdx(ql, 5000, &entry);
        if (strncmp((char *)entry.value, "xxxxxxxxxxxxxxxxxxxx", 20)) {
            TEST_PRINT_INFO("String val not match: %s", entry.value);
            err++;
        }
        ql_verify(ql, 157, 5001, 32, 9);
        ql_release_iterator(iter);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistNumbersLargerListReadB(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("numbers larger list read B");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushTail(ql, "99", 2);
        quicklistPushTail(ql, "98", 2);
        quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
        quicklistPushTail(ql, "96", 2);
        quicklistPushTail(ql, "95", 2);
        quicklistReplaceAtIndex(ql, 1, "foo", 3);
        quicklistReplaceAtIndex(ql, -1, "bar", 3);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistLremTestAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("lrem test at compress");
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char *words[] = {"abc", "foo", "bar", "foobar", "foobared", "zap", "bar", "test", "foo"};
            char *result[] = {"abc", "foo", "foobar", "foobared", "zap", "test", "foo"};
            char *resultB[] = {"abc", "foo", "foobar", "foobared", "zap", "test"};
            for (int i = 0; i < 9; i++) quicklistPushTail(ql, words[i], strlen(words[i]));

            /* lrem 0 bar */
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
            quicklistEntry entry;
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                if (quicklistCompare(&entry, (unsigned char *)"bar", 3)) {
                    quicklistDelEntry(iter, &entry);
                }
                i++;
            }
            ql_release_iterator(iter);

            /* check result of lrem 0 bar */
            iter = quicklistGetIterator(ql, AL_START_HEAD);
            i = 0;
            while (quicklistNext(iter, &entry)) {
                /* Result must be: abc, foo, foobar, foobared, zap, test,
                 * foo */
                int sz = entry.sz;
                if (strncmp((char *)entry.value, result[i], entry.sz)) {
                    TEST_PRINT_INFO("No match at position %d, got %.*s instead of %s", i, sz, entry.value, result[i]);
                    err++;
                }
                i++;
            }
            ql_release_iterator(iter);

            quicklistPushTail(ql, "foo", 3);

            /* lrem -2 foo */
            iter = quicklistGetIterator(ql, AL_START_TAIL);
            i = 0;
            int del = 2;
            while (quicklistNext(iter, &entry)) {
                if (quicklistCompare(&entry, (unsigned char *)"foo", 3)) {
                    quicklistDelEntry(iter, &entry);
                    del--;
                }
                if (!del) break;
                i++;
            }
            ql_release_iterator(iter);

            /* check result of lrem -2 foo */
            /* (we're ignoring the '2' part and still deleting all foo
             * because
             * we only have two foo) */
            iter = quicklistGetIterator(ql, AL_START_TAIL);
            i = 0;
            size_t resB = sizeof(resultB) / sizeof(*resultB);
            while (quicklistNext(iter, &entry)) {
                /* Result must be: abc, foo, foobar, foobared, zap, test,
                 * foo */
                int sz = entry.sz;
                if (strncmp((char *)entry.value, resultB[resB - 1 - i], sz)) {
                    TEST_PRINT_INFO("No match at position %d, got %.*s instead of %s", i, sz, entry.value,
                                    resultB[resB - 1 - i]);
                    err++;
                }
                i++;
            }

            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistIterateReverseDeleteAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("iterate reverse + delete at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            quicklistPushTail(ql, "abc", 3);
            quicklistPushTail(ql, "def", 3);
            quicklistPushTail(ql, "hij", 3);
            quicklistPushTail(ql, "jkl", 3);
            quicklistPushTail(ql, "oop", 3);

            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                if (quicklistCompare(&entry, (unsigned char *)"hij", 3)) {
                    quicklistDelEntry(iter, &entry);
                }
                i++;
            }
            ql_release_iterator(iter);

            if (i != 5) {
                TEST_PRINT_INFO("Didn't iterate 5 times, iterated %d times.", i);
                err++;
            }

            /* Check results after deletion of "hij" */
            iter = quicklistGetIterator(ql, AL_START_HEAD);
            i = 0;
            char *vals[] = {"abc", "def", "jkl", "oop"};
            while (quicklistNext(iter, &entry)) {
                if (!quicklistCompare(&entry, (unsigned char *)vals[i], 3)) {
                    TEST_PRINT_INFO("Value at %d didn't match %s\n", i, vals[i]);
                    err++;
                }
                i++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistIteratorAtIndexTestAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("iterator at index test at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 760; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }

            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIteratorAtIdx(ql, AL_START_HEAD, 437);
            int i = 437;
            while (quicklistNext(iter, &entry)) {
                if (entry.longval != nums[i]) {
                    TEST_PRINT_INFO("Expected %lld, but got %lld", entry.longval, nums[i]);
                    err++;
                }
                i++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistLtrimTestAAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("ltrim test A at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 32; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }
            if (fills[f] == 32) ql_verify(ql, 1, 32, 32, 32);
            /* ltrim 25 53 (keep [25,32] inclusive = 7 remaining) */
            quicklistDelRange(ql, 0, 25);
            quicklistDelRange(ql, 0, 0);
            quicklistEntry entry;
            for (int i = 0; i < 7; i++) {
                iter = quicklistGetIteratorEntryAtIdx(ql, i, &entry);
                if (entry.longval != nums[25 + i]) {
                    TEST_PRINT_INFO("Deleted invalid range!  Expected %lld but got "
                                    "%lld",
                                    entry.longval, nums[25 + i]);
                    err++;
                }
                ql_release_iterator(iter);
            }
            if (fills[f] == 32) ql_verify(ql, 1, 7, 7, 7);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistLtrimTestBAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("ltrim test B at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            /* Force-disable compression because our 33 sequential
             * integers don't compress and the check always fails. */
            quicklist *ql = quicklistNew(fills[f], QUICKLIST_NOCOMPRESS);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 33; i++) {
                nums[i] = i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }
            if (fills[f] == 32) ql_verify(ql, 2, 33, 32, 1);
            /* ltrim 5 16 (keep [5,16] inclusive = 12 remaining) */
            quicklistDelRange(ql, 0, 5);
            quicklistDelRange(ql, -16, 16);
            if (fills[f] == 32) ql_verify(ql, 1, 12, 12, 12);
            quicklistEntry entry;

            iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
            if (entry.longval != 5) {
                TEST_PRINT_INFO("A: longval not 5, but %lld", entry.longval);
                err++;
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
            if (entry.longval != 16) {
                TEST_PRINT_INFO("B! got instead: %lld", entry.longval);
                err++;
            }
            quicklistPushTail(ql, "bobobob", 7);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
            int sz = entry.sz;
            if (strncmp((char *)entry.value, "bobobob", 7)) {
                TEST_PRINT_INFO("Tail doesn't match bobobob, it's %.*s instead", sz, entry.value);
                err++;
            }
            ql_release_iterator(iter);

            for (int i = 0; i < 12; i++) {
                iter = quicklistGetIteratorEntryAtIdx(ql, i, &entry);
                if (entry.longval != nums[5 + i]) {
                    TEST_PRINT_INFO("Deleted invalid range!  Expected %lld but got "
                                    "%lld",
                                    entry.longval, nums[5 + i]);
                    err++;
                }

                ql_release_iterator(iter);
            }
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistLtrimTestCAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("ltrim test C at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 33; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }
            if (fills[f] == 32) ql_verify(ql, 2, 33, 32, 1);
            /* ltrim 3 3 (keep [3,3] inclusive = 1 remaining) */
            quicklistDelRange(ql, 0, 3);
            quicklistDelRange(ql, -29, 4000); /* make sure not loop forever */
            if (fills[f] == 32) ql_verify(ql, 1, 1, 1, 1);
            quicklistEntry entry;
            iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
            if (entry.longval != -5157318210846258173) {
                err++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistLtrimTestDAtCompress(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    quicklistIter *iter;
    TEST("ltrim test D at compress");

    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 33; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }
            if (fills[f] == 32) ql_verify(ql, 2, 33, 32, 1);
            quicklistDelRange(ql, -12, 3);
            if (ql->count != 30) {
                TEST_PRINT_INFO("Didn't delete exactly three elements!  Count is: %lu", ql->count);
                err++;
            }
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    TEST_ASSERT(err == 0);
    return 0;
}

int test_quicklistVerifySpecificCompressionOfInteriorNodes(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    int accurate = flags & UNIT_TEST_ACCURATE;

    TEST("verify specific compression of interior nodes");

    /* Run a longer test of compression depth outside of primary test loop. */
    int list_sizes[] = {250, 251, 500, 999, 1000};
    int list_count = accurate ? (int)(sizeof(list_sizes) / sizeof(*list_sizes)) : 1;
    for (int list = 0; list < list_count; list++) {
        for (int f = 0; f < fill_count; f++) {
            for (int depth = 1; depth < 40; depth++) {
                /* skip over many redundant test cases */
                quicklist *ql = quicklistNew(fills[f], depth);
                for (int i = 0; i < list_sizes[list]; i++) {
                    quicklistPushTail(ql, genstr("hello TAIL", i + 1), 64);
                    quicklistPushHead(ql, genstr("hello HEAD", i + 1), 64);
                }

                for (int step = 0; step < 2; step++) {
                    /* test remove node */
                    if (step == 1) {
                        for (int i = 0; i < list_sizes[list] / 2; i++) {
                            unsigned char *data;
                            TEST_ASSERT(quicklistPop(ql, QUICKLIST_HEAD, &data,
                                                     NULL, NULL));
                            zfree(data);
                            TEST_ASSERT(quicklistPop(ql, QUICKLIST_TAIL, &data,
                                                     NULL, NULL));
                            zfree(data);
                        }
                    }
                    quicklistNode *node = ql->head;
                    unsigned int low_raw = ql->compress;
                    unsigned int high_raw = ql->len - ql->compress;

                    for (unsigned int at = 0; at < ql->len;
                         at++, node = node->next) {
                        if (at < low_raw || at >= high_raw) {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                                TEST_PRINT_INFO("Incorrect compression: node %d is "
                                                "compressed at depth %d ((%u, %u); total "
                                                "nodes: %lu; size: %zu)",
                                                at, depth, low_raw, high_raw, ql->len,
                                                node->sz);
                                err++;
                            }
                        } else {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_LZF) {
                                TEST_PRINT_INFO("Incorrect non-compression: node %d is NOT "
                                                "compressed at depth %d ((%u, %u); total "
                                                "nodes: %lu; size: %zu; attempted: %d)",
                                                at, depth, low_raw, high_raw, ql->len,
                                                node->sz, node->attempted_compress);
                                err++;
                            }
                        }
                    }
                }

                quicklistRelease(ql);
            }
        }
    }
    TEST_ASSERT(err == 0);
    return 0;
}

/*-----------------------------------------------------------------------------
 * Quicklist Bookmark Unit Test
 *----------------------------------------------------------------------------*/

int test_quicklistBookmarkGetUpdatedToNextItem(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    TEST("bookmark get updated to next item");

    quicklist *ql = quicklistNew(1, 0);
    quicklistPushTail(ql, "1", 1);
    quicklistPushTail(ql, "2", 1);
    quicklistPushTail(ql, "3", 1);
    quicklistPushTail(ql, "4", 1);
    quicklistPushTail(ql, "5", 1);
    TEST_ASSERT(ql->len == 5);
    /* add two bookmarks, one pointing to the node before the last. */
    TEST_ASSERT(quicklistBookmarkCreate(&ql, "_dummy", ql->head->next));
    TEST_ASSERT(quicklistBookmarkCreate(&ql, "_test", ql->tail->prev));
    /* test that the bookmark returns the right node, delete it and see that the bookmark points to the last node */
    TEST_ASSERT(quicklistBookmarkFind(ql, "_test") == ql->tail->prev);
    TEST_ASSERT(quicklistDelRange(ql, -2, 1));
    TEST_ASSERT(quicklistBookmarkFind(ql, "_test") == ql->tail);
    /* delete the last node, and see that the bookmark was deleted. */
    TEST_ASSERT(quicklistDelRange(ql, -1, 1));
    TEST_ASSERT(quicklistBookmarkFind(ql, "_test") == NULL);
    /* test that other bookmarks aren't affected */
    TEST_ASSERT(quicklistBookmarkFind(ql, "_dummy") == ql->head->next);
    TEST_ASSERT(quicklistBookmarkFind(ql, "_missing") == NULL);
    TEST_ASSERT(ql->len == 3);
    quicklistBookmarksClear(ql); /* for coverage */
    TEST_ASSERT(quicklistBookmarkFind(ql, "_dummy") == NULL);
    quicklistRelease(ql);
    return 0;
}

int test_quicklistBookmarkLimit(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    TEST("bookmark limit");

    int i;
    quicklist *ql = quicklistNew(1, 0);
    quicklistPushHead(ql, "1", 1);
    for (i = 0; i < QL_MAX_BM; i++)
        TEST_ASSERT(quicklistBookmarkCreate(&ql, genstr("", i), ql->head));
    /* when all bookmarks are used, creation fails */
    TEST_ASSERT(!quicklistBookmarkCreate(&ql, "_test", ql->head));
    /* delete one and see that we can now create another */
    TEST_ASSERT(quicklistBookmarkDelete(ql, "0"));
    TEST_ASSERT(quicklistBookmarkCreate(&ql, "_test", ql->head));
    /* delete one and see that the rest survive */
    TEST_ASSERT(quicklistBookmarkDelete(ql, "_test"));
    for (i = 1; i < QL_MAX_BM; i++)
        TEST_ASSERT(quicklistBookmarkFind(ql, genstr("", i)) == ql->head);
    /* make sure the deleted ones are indeed gone */
    TEST_ASSERT(!quicklistBookmarkFind(ql, "0"));
    TEST_ASSERT(!quicklistBookmarkFind(ql, "_test"));
    quicklistRelease(ql);
    return 0;
}

int test_quicklistCompressAndDecompressQuicklistListpackNode(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    TEST("compress and decompress quicklist listpack node");

    if (!(flags & UNIT_TEST_LARGE_MEMORY)) return 0;

    quicklistNode *node = quicklistCreateNode();
    node->entry = lpNew(0);

    /* Just to avoid triggering the assertion in __quicklistCompressNode(),
     * it disables the passing of quicklist head or tail node. */
    node->prev = quicklistCreateNode();
    node->next = quicklistCreateNode();

    /* Create a rand string */
    size_t sz = (1 << 25); /* 32MB per one entry */
    unsigned char *s = zmalloc(sz);
    randstring(s, sz);

    /* Keep filling the node, until it reaches 1GB */
    for (int i = 0; i < 32; i++) {
        node->entry = lpAppend(node->entry, s, sz);
        node->sz = lpBytes((node)->entry);

        long long start = mstime();
        TEST_ASSERT(__quicklistCompressNode(node));
        TEST_ASSERT(__quicklistDecompressNode(node));
        TEST_PRINT_INFO("Compress and decompress: %zu MB in %.2f seconds.\n",
                        node->sz / 1024 / 1024, (float)(mstime() - start) / 1000);
    }

    zfree(s);
    zfree(node->prev);
    zfree(node->next);
    zfree(node->entry);
    zfree(node);
    return 0;
}

int test_quicklistCompressAndDecomressQuicklistPlainNodeLargeThanUINT32MAX(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    TEST("compress and decomress quicklist plain node large than UINT32_MAX");

    if (!(flags & UNIT_TEST_LARGE_MEMORY)) return 0;

#if ULONG_MAX >= 0xffffffffffffffff

    size_t sz = (1ull << 32);
    unsigned char *s = zmalloc(sz);
    randstring(s, sz);
    memcpy(s, "helloworld", 10);
    memcpy(s + sz - 10, "1234567890", 10);

    quicklistNode *node = __quicklistCreateNode(QUICKLIST_NODE_CONTAINER_PLAIN, s, sz);

    /* Just to avoid triggering the assertion in __quicklistCompressNode(),
     * it disables the passing of quicklist head or tail node. */
    node->prev = quicklistCreateNode();
    node->next = quicklistCreateNode();

    long long start = mstime();
    TEST_ASSERT(__quicklistCompressNode(node));
    TEST_ASSERT(__quicklistDecompressNode(node));
    TEST_PRINT_INFO("Compress and decompress: %zu MB in %.2f seconds.\n",
                    node->sz / 1024 / 1024, (float)(mstime() - start) / 1000);

    TEST_ASSERT(memcmp(node->entry, "helloworld", 10) == 0);
    TEST_ASSERT(memcmp(node->entry + sz - 10, "1234567890", 10) == 0);
    zfree(node->prev);
    zfree(node->next);
    zfree(node->entry);
    zfree(node);
    zfree(s);

#endif
    return 0;
}
