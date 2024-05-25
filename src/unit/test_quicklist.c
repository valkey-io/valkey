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

// static int fills[] = {-5, -4, -3, -2, -1, 0,
//                    1, 2, 32, 66, 128, 999};
// static int fill_count = 12;
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
__attribute__((unused)) static char *genstr(char *prefix, int i) {
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
__attribute__((unused)) static void ql_release_iterator(quicklistIter *iter) {
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
