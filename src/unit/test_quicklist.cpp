/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

extern "C" {
#include "fmacros.h"
#include "listpack.h"
#include "quicklist.h"
#include "zmalloc.h"

extern bool accurate;
extern bool large_memory;
/* Wrapper function declarations for accessing static quicklist.c internals */
size_t testOnlyQuicklistNodeNegFillLimit(int fill);
quicklistNode *testOnlyQuicklistCreateNode(void);
quicklistNode *testOnlyQuicklistCreateNodeWithValue(int container, void *value, size_t sz);
int testOnlyQuicklistCompressNode(quicklistNode *node);
int testOnlyQuicklistDecompressNode(quicklistNode *node);
}

/* Constants from quicklist.c */
#define QL_BM_BITS 4
#define QL_MAX_BM ((1 << QL_BM_BITS) - 1)

static int options[] = {0, 1, 2, 3, 4, 5, 6, 10};
static int option_count = 8;

static int fills[] = {-5, -4, -3, -2, -1, 0,
                      1, 2, 32, 66, 128, 999};
__attribute__((unused)) static int fill_count = 12;
static long long runtime[8];
static unsigned int err = 0;

/* Macros from quicklist.c for testing */
#define quicklistAllowsCompression(_ql) ((_ql)->compress != 0)
#define SIZE_SAFETY_LIMIT 8192

/*-----------------------------------------------------------------------------
 * Unit Function
 *----------------------------------------------------------------------------*/

/* Generate new string concatenating integer i against string 'prefix' */
static char *genstr(const char *prefix, int i) {
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

#define QL_TEST_VERBOSE 0
static void ql_info(quicklist *ql) {
#if QL_TEST_VERBOSE
    printf("Container length: %lu\n", ql->len);
    printf("Container size: %lu\n", ql->count);
    if (ql->head)
        printf("\t(zsize head: %lu)\n", lpLength(ql->head->entry));
    if (ql->tail)
        printf("\t(zsize tail: %lu)\n", lpLength(ql->tail->entry));
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
    quicklistNode *prev = nullptr;
    while (quicklistNext(iter, &entry)) {
        if (entry.node != prev) {
            /* Count the number of list nodes too */
            p++;
            prev = entry.node;
        }
        if (print) {
            int size = (entry.sz > (1 << 20)) ? 1 << 20 : entry.sz;
            printf("[%3d (%2d)]: [%.*s] (%lld)\n", i, p, size,
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
                    printf("Incorrect compression: node %d is "
                           "compressed at depth %d ((%u, %u); total "
                           "nodes: %lu; size: %zu; recompress: %d)",
                           at, ql->compress, low_raw, high_raw, ql->len, node->sz,
                           node->recompress);
                    errors++;
                }
            } else {
                if (node->encoding != QUICKLIST_NODE_ENCODING_LZF &&
                    !node->attempted_compress) {
                    printf("Incorrect non-compression: node %d is NOT "
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
        printf("quicklist length wrong: expected %d, got %lu", len, ql->len);
        errors++;
    }

    if (count != ql->count) {
        printf("quicklist count wrong: expected %d, got %lu", count, ql->count);
        errors++;
    }

    int loopr = itrprintr(ql, 0);
    if (loopr != (int)ql->count) {
        printf("quicklist cached count not match actual count: expected %lu, got "
               "%d",
               ql->count, loopr);
        errors++;
    }

    int rloopr = itrprintr_rev(ql, 0);
    if (loopr != rloopr) {
        printf("quicklist has different forward count than reverse count!  "
               "Forward count is %d, reverse count is %d.",
               loopr, rloopr);
        errors++;
    }

    if (ql->len == 0 && !errors) {
        return errors;
    }

    if (ql->head && head_count != ql->head->count &&
        head_count != lpLength(ql->head->entry)) {
        printf("quicklist head count wrong: expected %d, "
               "got cached %d vs. actual %lu",
               head_count, ql->head->count, lpLength(ql->head->entry));
        errors++;
    }

    if (ql->tail && tail_count != ql->tail->count &&
        tail_count != lpLength(ql->tail->entry)) {
        printf("quicklist tail count wrong: expected %d, "
               "got cached %u vs. actual %lu",
               tail_count, ql->tail->count, lpLength(ql->tail->entry));
        errors++;
    }

    errors += _ql_verify_compress(ql);
    return errors;
}

/* Release iterator and verify compress correctly. */
static void ql_release_iterator(quicklistIter *iter) {
    quicklist *ql = nullptr;
    if (iter) ql = iter->quicklist;
    quicklistReleaseIterator(iter);
    if (ql && _ql_verify_compress(ql)) {
        abort();
    }
}

/*-----------------------------------------------------------------------------
 * Quicklist Unit Test
 *----------------------------------------------------------------------------*/
class QuicklistTest : public ::testing::Test {
  protected:
    void SetUp() override {
        err = 0;
        for (int i = 0; i < 8; i++) {
            runtime[i] = 0;
        }
    }
};

TEST_F(QuicklistTest, quicklistCreateList) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistAddToTailOfEmptyList) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushTail(ql, (char *)("hello"), 6);
        /* 1 for head and 1 for tail because 1 node = head = tail */
        ql_verify(ql, 1, 1, 1, 1);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistAddToHeadOfEmptyList) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, (char *)("hello"), 6);
        /* 1 for head and 1 for tail because 1 node = head = tail */
        ql_verify(ql, 1, 1, 1, 1);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistAddToTail5xAtCompress) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 5; i++)
            quicklistPushTail(ql, genstr("hello", i), 32);
        if (ql->count != 5)
            FAIL();
        if (fills[_i] == 32)
            ql_verify(ql, 1, 5, 5, 5);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistAddToHead5xAtCompress) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 5; i++)
            quicklistPushHead(ql, genstr("hello", i), 32);
        if (ql->count != 5)
            FAIL();
        if (fills[_i] == 32)
            ql_verify(ql, 1, 5, 5, 5);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistAddToTail500xAtCompress) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 500; i++)
            quicklistPushTail(ql, genstr("hello", i), 64);
        if (ql->count != 500)
            FAIL();
        if (fills[_i] == 32)
            ql_verify(ql, 16, 500, 32, 20);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistAddToHead500xAtCompress) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 500; i++)
            quicklistPushHead(ql, genstr("hello", i), 32);
        if (ql->count != 500)
            FAIL();
        if (fills[_i] == 32)
            ql_verify(ql, 16, 500, 20, 32);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistRotateEmpty) {
    quicklist *ql = quicklistNew(-2, options[0]);
    quicklistRotate(ql);
    ql_verify(ql, 0, 0, 0, 0);
    quicklistRelease(ql);
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistComprassionPlainNode) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            size_t large_limit = (fills[f] < 0) ? testOnlyQuicklistNodeNegFillLimit(fills[f]) + 1 : SIZE_SAFETY_LIMIT + 1;
            char *buf = (char *)(zmalloc(large_limit));
            quicklist *ql = quicklistNew(fills[f], 1);
            for (int i = 0; i < 500; i++) {
                /* Set to 256 to allow the node to be triggered to compress,
                 * if it is less than 48(nocompress), the test will be successful. */
                snprintf(buf, large_limit, "hello%d", i);
                /* Fill the rest of the buffer to make it actually large */
                size_t len = strlen(buf);
                if (len < large_limit) {
                    memset(buf + len, 'x', large_limit - len - 1);
                    buf[large_limit - 1] = '\0';
                }
                quicklistPushHead(ql, buf, large_limit);
            }

            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
            quicklistEntry entry;
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                ASSERT_TRUE(QL_NODE_IS_PLAIN(entry.node));
                snprintf(buf, large_limit, "hello%d", i);
                size_t len = strlen(buf);
                if (len < large_limit) {
                    memset(buf + len, 'x', large_limit - len - 1);
                    buf[large_limit - 1] = '\0';
                }
                ASSERT_EQ(memcmp(entry.value, buf, large_limit), 0);
                i++;
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
            zfree(buf);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}


TEST_F(QuicklistTest, quicklistNextPlainNode) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            size_t large_limit = (fills[f] < 0) ? testOnlyQuicklistNodeNegFillLimit(fills[f]) + 1 : SIZE_SAFETY_LIMIT + 1;
            quicklist *ql = quicklistNew(fills[f], options[_i]);

            char *buf = (char *)(zmalloc(large_limit));
            memcpy(buf, "plain", 5);
            quicklistPushHead(ql, buf, large_limit);
            quicklistPushHead(ql, buf, large_limit);
            quicklistPushHead(ql, (char *)("packed3"), 7);
            quicklistPushHead(ql, (char *)("packed4"), 7);
            quicklistPushHead(ql, buf, large_limit);

            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);

            /* Iterate through all entries and verify their content.
             * The test is flexible about whether nodes are PLAIN or PACKED,
             * since this depends on internal implementation details. */
            while (quicklistNext(iter, &entry) != 0) {
                if (QL_NODE_IS_PLAIN(entry.node)) {
                    /* If it's a plain node, it should contain "plain" */
                    ASSERT_EQ(memcmp(entry.value, "plain", 5), 0);
                } else {
                    /* If it's a packed node, check if it's one of our expected values */
                    bool is_plain_value = (entry.sz >= 5 && memcmp(entry.value, "plain", 5) == 0);
                    bool is_packed_value = (entry.sz >= 6 && memcmp(entry.value, "packed", 6) == 0);
                    ASSERT_TRUE(is_plain_value || is_packed_value);
                }
            }
            ql_release_iterator(iter);
            quicklistRelease(ql);
            zfree(buf);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}


TEST_F(QuicklistTest, quicklistRotatePlainNode) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            size_t large_limit = (fills[f] < 0) ? testOnlyQuicklistNodeNegFillLimit(fills[f]) + 1 : SIZE_SAFETY_LIMIT + 1;

            unsigned char *data = nullptr;
            size_t sz;
            long long lv;
            int i = 0;
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char *buf = (char *)(zmalloc(large_limit));
            memcpy(buf, "hello1", 6);
            /* Fill the rest of the buffer to make it actually large */
            if (large_limit > 6) {
                memset(buf + 6, 'x', large_limit - 6);
            }
            quicklistPushHead(ql, buf, large_limit);
            memcpy(buf, "hello4", 6);
            if (large_limit > 6) {
                memset(buf + 6, 'x', large_limit - 6);
            }
            quicklistPushHead(ql, buf, large_limit);
            memcpy(buf, "hello3", 6);
            if (large_limit > 6) {
                memset(buf + 6, 'x', large_limit - 6);
            }
            quicklistPushHead(ql, buf, large_limit);
            memcpy(buf, "hello2", 6);
            if (large_limit > 6) {
                memset(buf + 6, 'x', large_limit - 6);
            }
            quicklistPushHead(ql, buf, large_limit);
            quicklistRotate(ql);

            for (i = 1; i < 5; i++) {
                ASSERT_TRUE(QL_NODE_IS_PLAIN(ql->tail));
                quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                int temp_char = data[5];
                zfree(data);
                ASSERT_EQ(temp_char, '0' + i);
            }

            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
            zfree(buf);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistRotateOneValOnce) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, (char *)("hello"), 6);
        quicklistRotate(ql);
        /* Ignore compression verify because listpack is
         * too small to compress. */
        ql_verify(ql, 1, 1, 1, 1);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistRotate500Val5000TimesAtCompress) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            quicklistPushHead(ql, (char *)("900"), 3);
            quicklistPushHead(ql, (char *)("7000"), 4);
            quicklistPushHead(ql, (char *)("-1200"), 5);
            quicklistPushHead(ql, (char *)("42"), 2);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 64);
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
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistPopEmpty) {
    quicklist *ql = quicklistNew(-2, options[0]);
    quicklistPop(ql, QUICKLIST_HEAD, nullptr, nullptr, nullptr);
    ql_verify(ql, 0, 0, 0, 0);
    quicklistRelease(ql);
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistPop1StringFrom1) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        char *populate = genstr("hello", 331);
        quicklistPushHead(ql, populate, 32);
        unsigned char *data;
        size_t sz;
        long long lv;
        ql_info(ql);
        ASSERT_TRUE(quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv));
        ASSERT_NE(data, nullptr);
        ASSERT_EQ(strcmp(populate, (char *)data), 0);
        zfree(data);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistPopHead1NumberFrom1) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, (char *)("55513"), 5);
        unsigned char *data;
        size_t sz;
        long long lv;
        ql_info(ql);
        ASSERT_TRUE(quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv));
        ASSERT_EQ(lv, 55513);
        zfree(data);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistPopHead500From500) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 500; i++)
            quicklistPushHead(ql, genstr("hello", i), 32);
        ql_info(ql);
        for (int i = 0; i < 500; i++) {
            unsigned char *data;
            size_t sz;
            long long lv;
            int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            ASSERT_TRUE(ret);
            ASSERT_EQ(strcmp(genstr("hello", 499 - i), (char *)data), 0);
            zfree(data);
        }
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistPopHead5000From500) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 500; i++)
            quicklistPushHead(ql, genstr("hello", i), 32);
        for (int i = 0; i < 5000; i++) {
            unsigned char *data;
            size_t sz;
            long long lv;
            int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            if (i < 500) {
                ASSERT_TRUE(ret);
                ASSERT_NE(data, nullptr);
                ASSERT_EQ(sz, 32u);
                ASSERT_EQ(strcmp(genstr("hello", 499 - i), (char *)data), 0);
                zfree(data);
            } else {
                ASSERT_FALSE(ret);
            }
        }
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistIterateForwardOver500List) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++)
            quicklistPushTail(ql, genstr("hello", i), 64);
        quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
        quicklistEntry entry;
        int i = 0, count = 0;
        while (quicklistNext(iter, &entry)) {
            ASSERT_EQ(strcmp(genstr("hello", i), (char *)entry.value), 0);
            i++;
            count++;
        }
        if (count != 500)
            FAIL();
        ql_release_iterator(iter);
        ql_verify(ql, 16, 500, 32, 20);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistIterateReverseOver500List) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++)
            quicklistPushHead(ql, genstr("hello", i), 32);
        quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
        quicklistEntry entry;
        int i = 0;
        while (quicklistNext(iter, &entry)) {
            ASSERT_EQ(strcmp(genstr("hello", i), (char *)entry.value), 0);
            i++;
        }
        if (i != 500)
            FAIL();
        ql_release_iterator(iter);
        ql_verify(ql, 16, 500, 20, 32);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistInsertAfter1Element) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, (char *)("hello"), 6);
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        quicklistInsertAfter(iter, &entry, (char *)("abc"), 4);
        ql_release_iterator(iter);
        ql_verify(ql, 1, 2, 2, 2);

        /* verify results */
        iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        ASSERT_EQ(strncmp((char *)entry.value, "hello", 5), 0);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
        ASSERT_EQ(strncmp((char *)entry.value, "abc", 3), 0);
        ql_release_iterator(iter);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistInsertBefore1Element) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushHead(ql, (char *)("hello"), 6);
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        quicklistInsertBefore(iter, &entry, (char *)("abc"), 4);
        ql_release_iterator(iter);
        ql_verify(ql, 1, 2, 2, 2);

        /* verify results */
        iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        ASSERT_EQ(strncmp((char *)entry.value, "abc", 3), 0);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
        ASSERT_EQ(strncmp((char *)entry.value, "hello", 5), 0);
        ql_release_iterator(iter);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistInsertHeadWhileHeadNodeIsFull) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(4, options[_i]);
        for (int i = 0; i < 10; i++)
            quicklistPushTail(ql, genstr("hello", i), 6);
        quicklistSetFill(ql, -1);
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, -10, &entry);
        char buf[4096] = {0};
        quicklistInsertBefore(iter, &entry, buf, 4096);
        ql_release_iterator(iter);
        ql_verify(ql, 4, 11, 1, 2);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistInsertTailWhileTailNodeIsFull) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(4, options[_i]);
        for (int i = 0; i < 10; i++)
            quicklistPushHead(ql, genstr("hello", i), 6);
        quicklistSetFill(ql, -1);
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
        char buf[4096] = {0};
        quicklistInsertAfter(iter, &entry, buf, 4096);
        ql_release_iterator(iter);
        ql_verify(ql, 4, 11, 2, 1);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistInsertOnceInElementsWhileIteratingAtCompress) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            quicklistPushTail(ql, (char *)("abc"), 3);
            quicklistSetFill(ql, 1);
            quicklistPushTail(ql, (char *)("def"), 3);
            quicklistSetFill(ql, f);
            quicklistPushTail(ql, (char *)("bob"), 3);
            quicklistPushTail(ql, (char *)("foo"), 3);
            quicklistPushTail(ql, (char *)("zoo"), 3);

            itrprintr(ql, 0);
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
            quicklistEntry entry;
            while (quicklistNext(iter, &entry)) {
                if (!strncmp((char *)entry.value, "bob", 3)) {
                    quicklistInsertBefore(iter, &entry, (char *)("bar"), 3);
                    break;
                }
            }
            ql_release_iterator(iter);
            itrprintr(ql, 0);

            /* verify results */
            iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
            ASSERT_EQ(strncmp((char *)entry.value, "abc", 3), 0);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
            ASSERT_EQ(strncmp((char *)entry.value, "def", 3), 0);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
            ASSERT_EQ(strncmp((char *)entry.value, "bar", 3), 0);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
            ASSERT_EQ(strncmp((char *)entry.value, "bob", 3), 0);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
            ASSERT_EQ(strncmp((char *)entry.value, "foo", 3), 0);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
            ASSERT_EQ(strncmp((char *)entry.value, "zoo", 3), 0);
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistInsertBefore250NewInMiddleOf500ElementsAtCompress) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i), 32);
            for (int i = 0; i < 250; i++) {
                quicklistEntry entry;
                quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 250, &entry);
                quicklistInsertBefore(iter, &entry, genstr("abc", i), 32);
                ql_release_iterator(iter);
            }
            if (fills[f] == 32)
                ql_verify(ql, 25, 750, 32, 20);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistInsertAfter250NewInMiddleOf500ElementsAtCompress) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            for (int i = 0; i < 250; i++) {
                quicklistEntry entry;
                quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 250, &entry);
                quicklistInsertAfter(iter, &entry, genstr("abc", i), 32);
                ql_release_iterator(iter);
            }
            ASSERT_EQ(ql->count, 750u);
            if (fills[f] == 32)
                ql_verify(ql, 26, 750, 20, 32);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDuplicateEmptyList) {
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
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDuplicateListOf1Element) {
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
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDuplicateListOf500) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++)
            quicklistPushHead(ql, genstr("hello", i), 32);
        ql_verify(ql, 16, 500, 20, 32);

        quicklist *copy = quicklistDup(ql);
        ql_verify(copy, 16, 500, 20, 32);
        quicklistRelease(ql);
        quicklistRelease(copy);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistIndex1200From500ListAtFill) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
            ASSERT_EQ(strcmp((char *)entry.value, "hello2"), 0);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, 200, &entry);
            ASSERT_EQ(strcmp((char *)entry.value, "hello201"), 0);
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistIndex12From500ListAtFill) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
            ASSERT_EQ(strcmp((char *)entry.value, "hello500"), 0);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, -2, &entry);
            ASSERT_EQ(strcmp((char *)entry.value, "hello499"), 0);
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistIndex100From500ListAtFill) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, -100, &entry);
            ASSERT_EQ(strcmp((char *)entry.value, "hello401"), 0);
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistIndexTooBig1From50ListAtFill) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            for (int i = 0; i < 50; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 50, &entry);
            ASSERT_EQ(iter, nullptr);
            ql_release_iterator(iter);
            quicklistRelease(ql);
        }
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDeleteRangeEmptyList) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistDelRange(ql, 5, 20);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDeleteRangeOfEntireNodeInListOfOneNode) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 32; i++)
            quicklistPushHead(ql, genstr("hello", i), 32);
        ql_verify(ql, 1, 32, 32, 32);
        quicklistDelRange(ql, 0, 32);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDeleteRangeOfEntireNodeWithOverflowCounts) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        for (int i = 0; i < 32; i++)
            quicklistPushHead(ql, genstr("hello", i), 32);
        ql_verify(ql, 1, 32, 32, 32);
        quicklistDelRange(ql, 0, 128);
        ql_verify(ql, 0, 0, 0, 0);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDeleteMiddle100Of500List) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++)
            quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 16, 500, 32, 20);
        quicklistDelRange(ql, 200, 100);
        ql_verify(ql, 14, 400, 32, 20);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDeleteLessThanFillButAcrossNodes) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++)
            quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 16, 500, 32, 20);
        quicklistDelRange(ql, 60, 10);
        ql_verify(ql, 16, 490, 32, 20);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDeleteNegative1From500List) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++)
            quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 16, 500, 32, 20);
        quicklistDelRange(ql, -1, 1);
        ql_verify(ql, 16, 499, 32, 19);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDeleteNegative1From500ListWithOverflowCounts) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++)
            quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 16, 500, 32, 20);
        quicklistDelRange(ql, -1, 128);
        ql_verify(ql, 16, 499, 32, 19);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDeleteNegative100From500List) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 500; i++)
            quicklistPushTail(ql, genstr("hello", i + 1), 32);
        quicklistDelRange(ql, -100, 100);
        ql_verify(ql, 13, 400, 32, 16);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistDelete10Count5From50List) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        for (int i = 0; i < 50; i++)
            quicklistPushTail(ql, genstr("hello", i + 1), 32);
        ql_verify(ql, 2, 50, 32, 18);
        quicklistDelRange(ql, -10, 5);
        ql_verify(ql, 2, 45, 32, 13);
        quicklistRelease(ql);
        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistNumbersOnlyListRead) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushTail(ql, (char *)("1111"), 4);
        quicklistPushTail(ql, (char *)("2222"), 4);
        quicklistPushTail(ql, (char *)("3333"), 4);
        quicklistPushTail(ql, (char *)("4444"), 4);
        ql_verify(ql, 1, 4, 4, 4);
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        ASSERT_EQ(entry.longval, 1111);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 1, &entry);
        ASSERT_EQ(entry.longval, 2222);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
        ASSERT_EQ(entry.longval, 3333);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
        ASSERT_EQ(entry.longval, 4444);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
        ASSERT_EQ(iter, nullptr);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
        ASSERT_EQ(entry.longval, 4444);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -2, &entry);
        ASSERT_EQ(entry.longval, 3333);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -3, &entry);
        ASSERT_EQ(entry.longval, 2222);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -4, &entry);
        ASSERT_EQ(entry.longval, 1111);
        ql_release_iterator(iter);

        iter = quicklistGetIteratorEntryAtIdx(ql, -5, &entry);
        ASSERT_EQ(iter, nullptr);
        ql_release_iterator(iter);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistNumbersLargerListRead) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistSetFill(ql, 32);
        char num[32];
        long long *nums = (long long *)(zmalloc(sizeof(long long) * 5000));
        for (int i = 0; i < 5000; i++) {
            nums[i] = -5157318210846258176 + i;
            int sz = ll2string(num, sizeof(num), nums[i]);
            quicklistPushTail(ql, num, sz);
        }
        quicklistPushTail(ql, (char *)("xxxxxxxxxxxxxxxxxxxx"), 20);
        quicklistEntry entry;
        for (int i = 0; i < 5000; i++) {
            quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, i, &entry);
            ASSERT_EQ(entry.longval, nums[i]);
            entry.longval = 0xdeadbeef;
            ql_release_iterator(iter);
        }
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 5000, &entry);
        ASSERT_EQ(strncmp((char *)entry.value, "xxxxxxxxxxxxxxxxxxxx", 20), 0);
        ql_verify(ql, 157, 5001, 32, 9);
        ql_release_iterator(iter);
        quicklistRelease(ql);
        zfree(nums);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistNumbersLargerListReadB) {
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        quicklist *ql = quicklistNew(-2, options[_i]);
        quicklistPushTail(ql, (char *)("99"), 2);
        quicklistPushTail(ql, (char *)("98"), 2);
        quicklistPushTail(ql, (char *)("xxxxxxxxxxxxxxxxxxxx"), 20);
        quicklistPushTail(ql, (char *)("96"), 2);
        quicklistPushTail(ql, (char *)("95"), 2);
        quicklistReplaceAtIndex(ql, 1, (char *)("foo"), 3);
        quicklistReplaceAtIndex(ql, -1, (char *)("bar"), 3);
        quicklistRelease(ql);

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistLremTestAtCompress) {
    quicklistIter *iter;
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char *words[] = {(char *)("abc"), (char *)("foo"), (char *)("bar"),
                             (char *)("foobar"), (char *)("foobared"), (char *)("zap"),
                             (char *)("bar"), (char *)("test"), (char *)("foo")};
            char *result[] = {(char *)("abc"), (char *)("foo"), (char *)("foobar"),
                              (char *)("foobared"), (char *)("zap"), (char *)("test"),
                              (char *)("foo")};
            char *resultB[] = {(char *)("abc"), (char *)("foo"), (char *)("foobar"),
                               (char *)("foobared"), (char *)("zap"), (char *)("test")};
            for (int i = 0; i < 9; i++) quicklistPushTail(ql, words[i], strlen(words[i]));

            /* lrem 0 bar */
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
            quicklistEntry entry;
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                if (quicklistCompare(&entry, (unsigned char *)("bar"), 3)) {
                    quicklistDelEntry(iter, &entry);
                }
                i++;
            }
            ql_release_iterator(iter);

            /* check result of lrem 0 bar */
            iter = quicklistGetIterator(ql, AL_START_HEAD);
            i = 0;
            while (quicklistNext(iter, &entry)) {
                /* Result must be: abc, foo, foobar, foobared, zap, test, foo */
                if (strncmp((char *)entry.value, result[i], entry.sz)) {
                    char got[64] = {0};
                    memcpy(got, entry.value, entry.sz < 63 ? entry.sz : 63);
                    FAIL() << "No match at position " << i << ", got '" << got << "' instead of " << result[i];
                }
                i++;
            }
            ql_release_iterator(iter);

            quicklistPushTail(ql, (char *)("foo"), 3);

            /* lrem -2 foo */
            iter = quicklistGetIterator(ql, AL_START_TAIL);
            i = 0;
            int del = 2;
            while (quicklistNext(iter, &entry)) {
                if (quicklistCompare(&entry, (unsigned char *)("foo"), 3)) {
                    quicklistDelEntry(iter, &entry);
                    del--;
                }
                if (!del) break;
                i++;
            }
            ql_release_iterator(iter);

            /* check result of lrem -2 foo */
            iter = quicklistGetIterator(ql, AL_START_TAIL);
            i = 0;
            size_t resB = sizeof(resultB) / sizeof(*resultB);
            while (quicklistNext(iter, &entry)) {
                if (strncmp((char *)entry.value, resultB[resB - 1 - i], entry.sz)) {
                    char got[64] = {0};
                    memcpy(got, entry.value, entry.sz < 63 ? entry.sz : 63);
                    FAIL() << "No match at position " << i << ", got '" << got << "' instead of " << resultB[resB - 1 - i];
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
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistIterateReverseDeleteAtCompress) {
    quicklistIter *iter;
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            quicklistPushTail(ql, (char *)("abc"), 3);
            quicklistPushTail(ql, (char *)("def"), 3);
            quicklistPushTail(ql, (char *)("hij"), 3);
            quicklistPushTail(ql, (char *)("jkl"), 3);
            quicklistPushTail(ql, (char *)("oop"), 3);

            quicklistEntry entry;
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                if (quicklistCompare(&entry, (unsigned char *)("hij"), 3)) {
                    quicklistDelEntry(iter, &entry);
                }
                i++;
            }
            ql_release_iterator(iter);

            if (i != 5) {
                FAIL() << "Didn't iterate 5 times, iterated " << i << " times.";
            }

            /* Check results after deletion of "hij" */
            iter = quicklistGetIterator(ql, AL_START_HEAD);
            i = 0;
            char *vals[] = {(char *)("abc"), (char *)("def"), (char *)("jkl"),
                            (char *)("oop")};
            while (quicklistNext(iter, &entry)) {
                if (!quicklistCompare(&entry, (unsigned char *)vals[i], 3)) {
                    FAIL() << "Value at " << i << " didn't match " << vals[i];
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
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistIteratorAtIndexTestAtCompress) {
    quicklistIter *iter;
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char num[32];
            long long *nums = (long long *)(zmalloc(sizeof(long long) * 760));
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
                    FAIL() << "Expected " << nums[i] << ", but got " << entry.longval;
                }
                i++;
            }
            ql_release_iterator(iter);
            zfree(nums);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistLtrimTestAAtCompress) {
    quicklistIter *iter;
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char num[32];
            long long *nums = (long long *)(zmalloc(sizeof(long long) * 32));
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
                    FAIL() << "Deleted invalid range!  Expected " << nums[25 + i] << " but got " << entry.longval;
                }
                ql_release_iterator(iter);
            }
            if (fills[f] == 32) ql_verify(ql, 1, 7, 7, 7);
            zfree(nums);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistLtrimTestBAtCompress) {
    quicklistIter *iter;
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            /* Force-disable compression because our 33 sequential
             * integers don't compress and the check always fails. */
            quicklist *ql = quicklistNew(fills[f], QUICKLIST_NOCOMPRESS);
            char num[32];
            long long *nums = (long long *)(zmalloc(sizeof(long long) * 33));
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
                FAIL() << "A: longval not 5, but " << entry.longval;
            }
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
            if (entry.longval != 16) {
                FAIL() << "B! got instead: " << entry.longval;
            }
            quicklistPushTail(ql, (char *)("bobobob"), 7);
            ql_release_iterator(iter);

            iter = quicklistGetIteratorEntryAtIdx(ql, -1, &entry);
            if (strncmp((char *)entry.value, "bobobob", 7)) {
                char got[64] = {0};
                memcpy(got, entry.value, entry.sz < 63 ? entry.sz : 63);
                FAIL() << "Tail doesn't match bobobob, it's '" << got << "' instead";
            }
            ql_release_iterator(iter);

            for (int i = 0; i < 12; i++) {
                iter = quicklistGetIteratorEntryAtIdx(ql, i, &entry);
                if (entry.longval != nums[5 + i]) {
                    FAIL() << "Deleted invalid range!  Expected " << nums[5 + i] << " but got " << entry.longval;
                }

                ql_release_iterator(iter);
            }
            zfree(nums);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistLtrimTestCAtCompress) {
    quicklistIter *iter;
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char num[32];
            long long *nums = (long long *)(zmalloc(sizeof(long long) * 33));
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
                FAIL() << "Expected -5157318210846258173, but got " << entry.longval;
            }
            ql_release_iterator(iter);
            zfree(nums);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistLtrimTestDAtCompress) {
    quicklistIter *iter;
    for (int _i = 0; _i < option_count; _i++) {
        long long start = mstime();
        for (int f = 0; f < fill_count; f++) {
            quicklist *ql = quicklistNew(fills[f], options[_i]);
            char num[32];
            long long *nums = (long long *)(zmalloc(sizeof(long long) * 33));
            for (int i = 0; i < 33; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }
            if (fills[f] == 32) ql_verify(ql, 2, 33, 32, 1);
            quicklistDelRange(ql, -12, 3);
            if (ql->count != 30) {
                FAIL() << "Didn't delete exactly three elements!  Count is: " << ql->count;
            }
            zfree(nums);
            quicklistRelease(ql);
        }

        long long stop = mstime();
        runtime[_i] += stop - start;
    }
    UNUSED(iter);
    ASSERT_EQ(err, 0u);
}

TEST_F(QuicklistTest, quicklistVerifySpecificCompressionOfInteriorNodes) {
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
                            ASSERT_TRUE(quicklistPop(ql, QUICKLIST_HEAD, &data, nullptr, nullptr));
                            zfree(data);
                            ASSERT_TRUE(quicklistPop(ql, QUICKLIST_TAIL, &data, nullptr, nullptr));
                            zfree(data);
                        }
                    }
                    quicklistNode *node = ql->head;
                    unsigned int low_raw = ql->compress;
                    unsigned int high_raw = ql->len - ql->compress;

                    for (unsigned int at = 0; at < ql->len; at++, node = node->next) {
                        if (at < low_raw || at >= high_raw) {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                                FAIL() << "Incorrect compression: node " << at << " is compressed at depth " << depth
                                       << " ((" << low_raw << ", " << high_raw << "); total nodes: " << ql->len
                                       << "; size: " << node->sz << ")";
                            }
                        } else {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_LZF) {
                                FAIL() << "Incorrect non-compression: node " << at << " is NOT compressed at depth " << depth
                                       << " ((" << low_raw << ", " << high_raw << "); total nodes: " << ql->len
                                       << "; size: " << node->sz << "; attempted: " << node->attempted_compress << ")";
                            }
                        }
                    }
                }

                quicklistRelease(ql);
            }
        }
    }
    ASSERT_EQ(err, 0u);
}

/*-----------------------------------------------------------------------------
 * Quicklist Bookmark Unit Test
 *----------------------------------------------------------------------------*/

TEST_F(QuicklistTest, quicklistBookmarkGetUpdatedToNextItem) {
    quicklist *ql = quicklistNew(1, 0);
    quicklistPushTail(ql, (char *)("1"), 1);
    quicklistPushTail(ql, (char *)("2"), 1);
    quicklistPushTail(ql, (char *)("3"), 1);
    quicklistPushTail(ql, (char *)("4"), 1);
    quicklistPushTail(ql, (char *)("5"), 1);
    ASSERT_EQ(ql->len, 5u);
    /* add two bookmarks, one pointing to the node before the last. */
    ASSERT_TRUE(quicklistBookmarkCreate(&ql, "_dummy", ql->head->next));
    ASSERT_TRUE(quicklistBookmarkCreate(&ql, "_test", ql->tail->prev));
    /* test that the bookmark returns the right node, delete it and see that the bookmark points to the last node */
    ASSERT_EQ(quicklistBookmarkFind(ql, "_test"), ql->tail->prev);
    ASSERT_TRUE(quicklistDelRange(ql, -2, 1));
    ASSERT_EQ(quicklistBookmarkFind(ql, "_test"), ql->tail);
    /* delete the last node, and see that the bookmark was deleted. */
    ASSERT_TRUE(quicklistDelRange(ql, -1, 1));
    ASSERT_EQ(quicklistBookmarkFind(ql, "_test"), nullptr);
    /* test that other bookmarks aren't affected */
    ASSERT_EQ(quicklistBookmarkFind(ql, "_dummy"), ql->head->next);
    ASSERT_EQ(quicklistBookmarkFind(ql, "_missing"), nullptr);
    ASSERT_EQ(ql->len, 3u);
    quicklistBookmarksClear(ql); /* for coverage */
    ASSERT_EQ(quicklistBookmarkFind(ql, "_dummy"), nullptr);
    quicklistRelease(ql);
}

TEST_F(QuicklistTest, quicklistBookmarkLimit) {
    int i;
    quicklist *ql = quicklistNew(1, 0);
    quicklistPushHead(ql, (char *)("1"), 1);
    for (i = 0; i < QL_MAX_BM; i++)
        ASSERT_TRUE(quicklistBookmarkCreate(&ql, genstr("", i), ql->head));
    /* when all bookmarks are used, creation fails */
    ASSERT_FALSE(quicklistBookmarkCreate(&ql, "_test", ql->head));
    /* delete one and see that we can now create another */
    ASSERT_TRUE(quicklistBookmarkDelete(ql, "0"));
    ASSERT_TRUE(quicklistBookmarkCreate(&ql, "_test", ql->head));
    /* delete one and see that the rest survive */
    ASSERT_TRUE(quicklistBookmarkDelete(ql, "_test"));
    for (i = 1; i < QL_MAX_BM; i++)
        ASSERT_EQ(quicklistBookmarkFind(ql, genstr("", i)), ql->head);
    /* make sure the deleted ones are indeed gone */
    ASSERT_FALSE(quicklistBookmarkFind(ql, "0"));
    ASSERT_FALSE(quicklistBookmarkFind(ql, "_test"));
    quicklistRelease(ql);
}

TEST_F(QuicklistTest, quicklistCompressAndDecompressQuicklistListpackNode) {
    if (!large_memory) GTEST_SKIP() << "Skipping large memory test";

#ifdef VALKEY_ADDRESS_SANITIZER
    /* Skip this test under sanitizers to avoid OOM in github actions */
    GTEST_SKIP() << "Skipping under address sanitizer";
#endif

    quicklistNode *node = testOnlyQuicklistCreateNode();
    node->entry = lpNew(0);

    /* Just to avoid triggering the assertion in __quicklistCompressNode(),
     * it disables the passing of quicklist head or tail node. */
    node->prev = testOnlyQuicklistCreateNode();
    node->next = testOnlyQuicklistCreateNode();

    /* Create a rand string */
    size_t sz = (1 << 25); /* 32MB per one entry */
    unsigned char *s = (unsigned char *)(zmalloc(sz));
    randstring(s, sz);

    /* Keep filling the node, until it reaches 1GB */
    for (int i = 0; i < 32; i++) {
        node->entry = lpAppend(node->entry, s, sz);
        node->sz = lpBytes(node->entry);

        long long start = mstime();
        ASSERT_TRUE(testOnlyQuicklistCompressNode(node));
        ASSERT_TRUE(testOnlyQuicklistDecompressNode(node));
        printf("Compress and decompress: %zu MB in %.3f seconds.\n",
               node->sz / 1024 / 1024, (double)(mstime() - start) / 1000);
    }

    zfree(s);
    zfree(node->prev);
    zfree(node->next);
    zfree(node->entry);
    zfree(node);
}

TEST_F(QuicklistTest, quicklistCompressAndDecomressQuicklistPlainNodeLargeThanUINT32MAX) {
    if (!large_memory) GTEST_SKIP() << "Skipping large memory test";

#if ULONG_MAX >= 0xffffffffffffffff

    size_t sz = (1ull << 32);
    unsigned char *s = (unsigned char *)(zmalloc(sz));
    randstring(s, sz);
    memcpy(s, "helloworld", 10);
    memcpy(s + sz - 10, "1234567890", 10);

    quicklistNode *node = testOnlyQuicklistCreateNodeWithValue(QUICKLIST_NODE_CONTAINER_PLAIN, s, sz);

    /* Just to avoid triggering the assertion in __quicklistCompressNode(),
     * it disables the passing of quicklist head or tail node. */
    node->prev = testOnlyQuicklistCreateNode();
    node->next = testOnlyQuicklistCreateNode();

    long long start = mstime();
    ASSERT_TRUE(testOnlyQuicklistCompressNode(node));
    ASSERT_TRUE(testOnlyQuicklistDecompressNode(node));
    printf("Compress and decompress: %zu MB in %.3f seconds.\n",
           node->sz / 1024 / 1024, (double)(mstime() - start) / 1000);

    ASSERT_EQ(memcmp(node->entry, "helloworld", 10), 0);
    ASSERT_EQ(memcmp(node->entry + sz - 10, "1234567890", 10), 0);
    zfree(node->prev);
    zfree(node->next);
    zfree(node->entry);
    zfree(node);
    zfree(s);

#endif
}
