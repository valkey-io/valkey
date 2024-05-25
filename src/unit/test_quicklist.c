#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "test_help.h"
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>

#include "../zmalloc.h"
#include "../listpack.h"
#include "../quicklist.c"
#include "../util.h"

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
static long long mstime(void) { return ustime() / 1000; }

/* Generate new string concatenating integer i against string 'prefix' */
static char *genstr(char *prefix, int i) {
    static char result[64] = {0};
    snprintf(result, sizeof(result), "%s%d", prefix, i);
    return result;
}

__attribute__((unused)) static void randstring(unsigned char *target, size_t sz) {
    size_t p = 0;
    int minval, maxval;
    switch(rand() % 3) {
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

    while(p < sz)
        target[p++] = minval+rand()%(maxval-minval+1);
}

#define TEST(name) printf("test — %s\n", name);
#define TEST_DESC(name, ...) printf("test — " name "\n", __VA_ARGS__);
#define UNUSED(x) (void)(x)

/*-----------------------------------------------------------------------------
 * Quicklist verify Function
 *----------------------------------------------------------------------------*/

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
            int size = (entry.sz > (1<<20)) ? 1<<20 : entry.sz;
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

#define ql_verify(a, b, c, d, e)                                               \
    do {                                                                       \
        err += _ql_verify((a), (b), (c), (d), (e));                            \
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
static int _ql_verify(quicklist *ql, uint32_t len, uint32_t count,
                      uint32_t head_count, uint32_t tail_count) {
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
