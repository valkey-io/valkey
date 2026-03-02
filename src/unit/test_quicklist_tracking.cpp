/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>

extern "C" {
#include "listpack.h"
#include "quicklist.h"
#include "server.h"
#include "zmalloc.h"

size_t objectComputeSize(robj *key, robj *o, size_t sample_size, int dbid);
size_t objectComputeSizeWithTrackedSize(robj *key, robj *o, size_t sample_size, int dbid);
}

class QuicklistTrackingTest : public ::testing::Test {
};

/* Helper: assert tracked_size matches ground-truth objectComputeSize.
 * objectComputeSize crashes on empty quicklists (do-while on NULL head),
 * so we guard against that by only comparing when the list is non-empty.
 * For empty lists, we just verify tracked_size equals sizeof(quicklist). */
#define ASSERT_TRACKED_SIZE_CORRECT(list)                                                     \
    do {                                                                                      \
        quicklist *_ql = (quicklist *)objectGetVal(list);                                     \
        if (_ql->len > 0) {                                                                   \
            size_t _computed = objectComputeSize(nullptr, (list), SIZE_MAX, 0);               \
            size_t _tracked = objectComputeSizeWithTrackedSize(nullptr, (list), SIZE_MAX, 0); \
            ASSERT_EQ(_computed, _tracked);                                                   \
        } else {                                                                              \
            ASSERT_EQ(_ql->tracked_size, sizeof(quicklist));                                  \
        }                                                                                     \
    } while (0)

/* 1. Basic pushHead / pushTail into existing nodes */
TEST_F(QuicklistTrackingTest, BasicPushHeadTail) {
    robj *list = createQuicklistObject(3, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 50; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "element_%d", i);
        if (i % 2 == 0)
            quicklistPushTail(ql, buf, strlen(buf));
        else
            quicklistPushHead(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);
    decrRefCount(list);
}

/* 2. pushHead / pushTail that create new nodes (fill=1 forces new node per element) */
TEST_F(QuicklistTrackingTest, PushCreatesNewNodes) {
    robj *list = createQuicklistObject(1, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 20; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "single_node_element_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 20ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);
    decrRefCount(list);
}

/* 3. Pop from head and tail */
TEST_F(QuicklistTrackingTest, PopHeadAndTail) {
    robj *list = createQuicklistObject(3, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "pop_test_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    for (int i = 0; i < 8; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    for (int i = 0; i < 8; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 4. Delete entire node (pop all elements) */
TEST_F(QuicklistTrackingTest, DeleteEntireNode) {
    robj *list = createQuicklistObject(2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "del_node_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 5ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    while (quicklistCount(ql) > 0) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
        ASSERT_TRACKED_SIZE_CORRECT(list);
    }
    decrRefCount(list);
}

/* 5. Compression and decompression */
TEST_F(QuicklistTrackingTest, CompressDecompress) {
    robj *list = createQuicklistObject(-2, 1);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 500; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "compress_test_value_%d_padding_data", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_GE(ql->len, 3ul);
    bool has_compressed = false;
    for (quicklistNode *n = ql->head; n; n = n->next) {
        if (quicklistNodeIsCompressed(n)) {
            has_compressed = true;
            break;
        }
    }
    ASSERT_TRUE(has_compressed);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    for (int i = 0; i < 50; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    for (int i = 0; i < 50; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    for (int i = 0; i < 50; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "after_compress_push_%d", i);
        quicklistPushHead(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 6. InsertBefore / InsertAfter into non-full node */
TEST_F(QuicklistTrackingTest, InsertNonFullNode) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "insert_test_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertAfter(iter, &entry, (void *)"INSERTED_AFTER", 14);
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertBefore(iter, &entry, (void *)"INSERTED_BEFORE", 15);
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 7. Insert that triggers node split */
TEST_F(QuicklistTrackingTest, InsertTriggersSplit) {
    robj *list = createQuicklistObject(3, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 12; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "split_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 4ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertAfter(iter, &entry, (void *)"SPLIT_INSERT", 12);
    quicklistReleaseIterator(iter);
    ASSERT_GT(ql->len, 4ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 8a. Insert into next neighbor (full node, at_tail, avail_next) */
TEST_F(QuicklistTrackingTest, InsertNextNeighbor) {
    robj *list = createQuicklistObject(4, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 6; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "neighbor_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 2ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertAfter(iter, &entry, (void *)"NEIGHBOR_NEXT", 13);
    quicklistReleaseIterator(iter);
    ASSERT_EQ(ql->len, 2ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 8b. Insert into prev neighbor (full node, at_head, avail_prev) */
TEST_F(QuicklistTrackingTest, InsertPrevNeighbor) {
    robj *list = createQuicklistObject(4, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 8; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "avprev_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 2ul);
    unsigned char *data;
    size_t sz;
    long long val;
    quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
    if (data) zfree(data);
    ASSERT_EQ(ql->head->count, 3u);
    ASSERT_EQ(ql->tail->count, 4u);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertBefore(iter, &entry, (void *)"NEIGHBOR_PREV", 13);
    quicklistReleaseIterator(iter);
    ASSERT_EQ(ql->len, 2ul);
    ASSERT_EQ(ql->head->count, 4u);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 9. ReplaceEntry - listpack path (lpReplace) */
TEST_F(QuicklistTrackingTest, ReplaceEntryListpack) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "replace_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistReplaceEntry(iter, &entry, (void *)"S", 1);
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
    ASSERT_NE(iter, nullptr);
    char big_replace[200];
    memset(big_replace, 'X', sizeof(big_replace));
    quicklistReplaceEntry(iter, &entry, big_replace, sizeof(big_replace));
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 10. ReplaceEntry - plain node path */
TEST_F(QuicklistTrackingTest, ReplaceEntryPlainNode) {
    quicklistSetPackedThreshold(64);

    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    char large[128];
    memset(large, 'A', sizeof(large));
    quicklistPushTail(ql, large, sizeof(large));
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "mixed_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
    ASSERT_NE(iter, nullptr);
    ASSERT_TRUE(QL_NODE_IS_PLAIN(entry.node));
    char new_large[256];
    memset(new_large, 'B', sizeof(new_large));
    quicklistReplaceEntry(iter, &entry, new_large, sizeof(new_large));
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistSetPackedThreshold(0);
    decrRefCount(list);
}

/* 11. DelRange - partial and full node deletion */
TEST_F(QuicklistTrackingTest, DelRange) {
    robj *list = createQuicklistObject(5, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "delrange_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistDelRange(ql, 3, 2);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistDelRange(ql, 2, 15);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistDelRange(ql, 0, 3);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistDelRange(ql, -3, 3);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 12. Rotate (move tail to head) */
TEST_F(QuicklistTrackingTest, Rotate) {
    robj *list = createQuicklistObject(3, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 15; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rotate_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    for (int i = 0; i < 10; i++) {
        quicklistRotate(ql);
        ASSERT_TRACKED_SIZE_CORRECT(list);
    }

    decrRefCount(list);
}

/* 13. Dup (deep copy) */
TEST_F(QuicklistTrackingTest, Dup) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "dup_test_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklist *copy = quicklistDup(ql);
    robj *copy_list = createObject(OBJ_LIST, copy);
    copy_list->encoding = OBJ_ENCODING_QUICKLIST;
    ASSERT_TRACKED_SIZE_CORRECT(copy_list);

    for (int i = 0; i < 20; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(copy, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_SIZE_CORRECT(copy_list);

    decrRefCount(copy_list);
    decrRefCount(list);
}

/* 14. Dup with compression */
TEST_F(QuicklistTrackingTest, DupWithCompression) {
    robj *list = createQuicklistObject(-2, 1);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 500; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "dup_compress_%d_padding_data_here", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklist *copy = quicklistDup(ql);
    robj *copy_list = createObject(OBJ_LIST, copy);
    copy_list->encoding = OBJ_ENCODING_QUICKLIST;
    ASSERT_TRACKED_SIZE_CORRECT(copy_list);

    decrRefCount(copy_list);
    decrRefCount(list);
}

/* 15. Node merge via _quicklistListpackMerge */
TEST_F(QuicklistTrackingTest, NodeMerge) {
    robj *list = createQuicklistObject(4, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 12; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "merge_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_EQ(ql->len, 3ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistDelRange(ql, 4, 3);
    ASSERT_EQ(ql->len, 3ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistDelRange(ql, 5, 3);
    ASSERT_EQ(ql->len, 3ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    unsigned long nodes_before = ql->len;

    quicklistEntry entry;
    quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
    ASSERT_NE(iter, nullptr);
    quicklistInsertAfter(iter, &entry, (void *)"MERGE_TRIGGER", 13);
    quicklistReleaseIterator(iter);

    ASSERT_LE(ql->len, nodes_before + 1);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 16. Plain node insert */
TEST_F(QuicklistTrackingTest, PlainNodeInsert) {
    quicklistSetPackedThreshold(64);

    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "small_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    for (int i = 0; i < 5; i++) {
        char large[128];
        memset(large, 'P' + i, sizeof(large));
        quicklistPushTail(ql, large, sizeof(large));
        ASSERT_TRACKED_SIZE_CORRECT(list);
    }

    for (int i = 0; i < 3; i++) {
        unsigned char *data;
        size_t sz;
        long long val;
        quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
        ASSERT_TRACKED_SIZE_CORRECT(list);
    }

    quicklistSetPackedThreshold(0);
    decrRefCount(list);
}

/* 17. Insert into empty list (no reference node) */
TEST_F(QuicklistTrackingTest, InsertEmptyList) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    quicklistEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.quicklist = ql;
    quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
    quicklistInsertAfter(iter, &entry, (void *)"first_ever", 10);
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 18. Compression with operations interleaved */
TEST_F(QuicklistTrackingTest, InterleavedCompressOps) {
    robj *list = createQuicklistObject(-2, 2);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 1000; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "interleaved_%d_extra_padding_here", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    for (int i = 0; i < 100; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "interleaved_new_%d", i);
        if (i % 2 == 0)
            quicklistPushHead(ql, buf, strlen(buf));
        else
            quicklistPushTail(ql, buf, strlen(buf));

        unsigned char *data;
        size_t sz;
        long long val;
        if (i % 3 == 0)
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        else
            quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
        if (data) zfree(data);
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    for (int i = 0; i < 10; i++) {
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, (long)quicklistCount(ql) / 2, &entry);
        if (iter) {
            char buf[100];
            memset(buf, 'R', sizeof(buf));
            quicklistReplaceEntry(iter, &entry, buf, sizeof(buf));
            quicklistReleaseIterator(iter);
        }
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 19. AppendListpack / AppendPlainNode (RDB load paths) */
TEST_F(QuicklistTrackingTest, AppendListpackAndPlainNode) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    unsigned char *lp = lpNew(0);
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rdb_lp_%d", i);
        lp = lpAppend(lp, (unsigned char *)buf, strlen(buf));
    }
    quicklistAppendListpack(ql, lp);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    unsigned char *lp2 = lpNew(0);
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rdb_lp2_%d", i);
        lp2 = lpAppend(lp2, (unsigned char *)buf, strlen(buf));
    }
    quicklistAppendListpack(ql, lp2);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    size_t plain_sz = 256;
    unsigned char *plain_data = (unsigned char *)zmalloc(plain_sz);
    memset(plain_data, 'Z', plain_sz);
    quicklistAppendPlainNode(ql, plain_data, plain_sz);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 20. DelEntry via iterator */
TEST_F(QuicklistTrackingTest, DelEntryViaIterator) {
    robj *list = createQuicklistObject(5, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 25; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "delentry_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
    quicklistEntry entry;
    int count = 0;
    while (quicklistNext(iter, &entry)) {
        if (count % 2 == 0) {
            quicklistDelEntry(iter, &entry);
        }
        count++;
    }
    quicklistReleaseIterator(iter);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 21. ReplaceAtIndex */
TEST_F(QuicklistTrackingTest, ReplaceAtIndex) {
    robj *list = createQuicklistObject(-2, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    for (int i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "replaceatidx_%d", i);
        quicklistPushTail(ql, buf, strlen(buf));
    }
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistReplaceAtIndex(ql, 0, (void *)"HEAD_REPLACED", 13);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistReplaceAtIndex(ql, 15, (void *)"MID_REPLACED", 12);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistReplaceAtIndex(ql, 29, (void *)"TAIL_REPLACED", 13);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}

/* 23. Same-size replacement: ensures lpLastAllocSize is not stale.
 *     When lpReplace replaces an element with one of the exact same encoded
 *     size, lpInsert may skip lp_realloc entirely, which used to leave
 *     lp_last_alloc_size holding a value from a previous (different node)
 *     operation.  This test creates two nodes, mutates node A (changing
 *     lp_last_alloc_size), then does a same-size replace on node B to
 *     verify that tracked_size remains accurate. */
TEST_F(QuicklistTrackingTest, SameSizeReplaceNoStale) {
    /* fill=4 to create multiple nodes quickly. */
    robj *list = createQuicklistObject(4, 0);
    quicklist *ql = (quicklist *)objectGetVal(list);

    /* Fill two nodes: 4 entries of "aaaa" each. */
    for (int i = 0; i < 8; i++) {
        quicklistPushTail(ql, (void *)"aaaa", 4);
    }
    ASSERT_EQ(ql->len, 2ul);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    /* Mutate node A (head) with a differently-sized value to change
     * lpLastAllocSize to something different from node B's alloc size. */
    quicklistReplaceAtIndex(ql, 0, (void *)"bbbbbbbbbbbbbbbb", 16);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    /* Now do a same-size replace on node B (tail): replace "aaaa" with
     * "cccc" (same 4 bytes).  lpInsert will compute new == old bytes
     * and may skip lp_realloc.  If lp_last_alloc_size is stale from
     * the head node's operation, tracked_size will become wrong. */
    quicklistReplaceAtIndex(ql, 7, (void *)"cccc", 4);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    /* Also test multiple same-size replacements in a row. */
    quicklistReplaceAtIndex(ql, 5, (void *)"dddd", 4);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    quicklistReplaceAtIndex(ql, 6, (void *)"eeee", 4);
    ASSERT_TRACKED_SIZE_CORRECT(list);

    decrRefCount(list);
}
