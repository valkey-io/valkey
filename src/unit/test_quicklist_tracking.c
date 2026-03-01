#include <stdio.h>
#include <limits.h>
#include <string.h>

#include "../server.h"
#include "../quicklist.h"
#include "test_help.h"

/* These are defined in object.c but not declared in any header. */
extern size_t objectComputeSize(robj *key, robj *o, size_t sample_size, int dbid);
extern size_t objectComputeSizeWithTrackedSize(robj *key, robj *o, size_t sample_size, int dbid);

/* Helper: assert tracked_size matches ground-truth objectComputeSize.
 * objectComputeSize crashes on empty quicklists (do-while on NULL head),
 * so we guard against that by only comparing when the list is non-empty.
 * For empty lists, we just verify tracked_size equals sizeof(quicklist). */
#define ASSERT_TRACKED_SIZE_CORRECT(list) do {                                          \
    quicklist *_ql = objectGetVal(list);                                                \
    if (_ql->len > 0) {                                                                \
        size_t _computed = objectComputeSize(NULL, (list), LLONG_MAX, 0);               \
        size_t _tracked = objectComputeSizeWithTrackedSize(NULL, (list), LLONG_MAX, 0); \
        TEST_ASSERT(_computed == _tracked);                                             \
    } else {                                                                            \
        TEST_ASSERT(_ql->tracked_size == sizeof(quicklist));                            \
    }                                                                                   \
} while (0)

int test_quicklist_tracked_size(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Note: quicklistSetPackedThreshold() is the public API for packed_threshold. */

    /* ========================================================
     * 1. Basic pushHead / pushTail into existing nodes
     *    Exercises: lpPrepend/lpAppend on existing node + lp_last_alloc_size capture
     * ======================================================== */
    {
        robj *list = createQuicklistObject(3, 0); /* fill=3 (count-based), no compression */
        quicklist *ql = objectGetVal(list);

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

    /* ========================================================
     * 2. pushHead / pushTail that create new nodes
     *    Exercises: new node creation with lpPrepend(lpNew(0), ...)
     *    fill=1 forces a new node per element
     * ======================================================== */
    {
        robj *list = createQuicklistObject(1, 0);
        quicklist *ql = objectGetVal(list);

        for (int i = 0; i < 20; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "single_node_element_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        TEST_ASSERT(ql->len == 20); /* one element per node */
        ASSERT_TRACKED_SIZE_CORRECT(list);
        decrRefCount(list);
    }

    /* ========================================================
     * 3. Pop from head and tail
     *    Exercises: quicklistDelIndex -> lpDelete + tracked_size delta
     *    Also exercises node deletion when node becomes empty
     * ======================================================== */
    {
        robj *list = createQuicklistObject(3, 0);
        quicklist *ql = objectGetVal(list);

        for (int i = 0; i < 30; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "pop_test_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Pop from head */
        for (int i = 0; i < 8; i++) {
            unsigned char *data;
            size_t sz;
            long long val;
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
            if (data) zfree(data);
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Pop from tail */
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

    /* ========================================================
     * 4. Delete entire node (pop all elements from a node)
     *    Exercises: __quicklistDelNode -> tracked_size -= entry_alloc_sz + sizeof(quicklistNode)
     * ======================================================== */
    {
        robj *list = createQuicklistObject(2, 0); /* 2 elements per node */
        quicklist *ql = objectGetVal(list);

        for (int i = 0; i < 10; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "del_node_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        TEST_ASSERT(ql->len == 5); /* 10 elements / 2 per node */
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Pop all elements, forcing node deletions */
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

    /* ========================================================
     * 5. Compression and decompression
     *    Exercises: __quicklistCompressNode (zrealloc_usable for LZF)
     *              __quicklistDecompressNode (zmalloc_usable for decompressed buf)
     * ======================================================== */
    {
        robj *list = createQuicklistObject(-2, 1); /* size-based fill, compress depth=1 */
        quicklist *ql = objectGetVal(list);

        /* Add enough elements to create multiple nodes so interior ones get compressed */
        for (int i = 0; i < 500; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "compress_test_value_%d_padding_data", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        TEST_ASSERT(ql->len >= 3); /* need at least 3 nodes for compression to kick in */
        /* Verify at least one interior node is actually compressed */
        int has_compressed = 0;
        for (quicklistNode *n = ql->head; n; n = n->next) {
            if (quicklistNodeIsCompressed(n)) { has_compressed = 1; break; }
        }
        TEST_ASSERT(has_compressed);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Pop from head — forces decompression of the new head's neighbor */
        for (int i = 0; i < 50; i++) {
            unsigned char *data;
            size_t sz;
            long long val;
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
            if (data) zfree(data);
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Pop from tail — forces decompression of the new tail's neighbor */
        for (int i = 0; i < 50; i++) {
            unsigned char *data;
            size_t sz;
            long long val;
            quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &val);
            if (data) zfree(data);
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Push to head — may decompress/recompress nodes */
        for (int i = 0; i < 50; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "after_compress_push_%d", i);
            quicklistPushHead(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 6. InsertBefore / InsertAfter into non-full node
     *    Exercises: lpInsertString + lp_last_alloc_size in _quicklistInsert
     * ======================================================== */
    {
        robj *list = createQuicklistObject(-2, 0);
        quicklist *ql = objectGetVal(list);

        for (int i = 0; i < 20; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "insert_test_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Insert after the 5th element */
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
        TEST_ASSERT(iter != NULL);
        quicklistInsertAfter(iter, &entry, "INSERTED_AFTER", 14);
        quicklistReleaseIterator(iter);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Insert before the 10th element */
        iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
        TEST_ASSERT(iter != NULL);
        quicklistInsertBefore(iter, &entry, "INSERTED_BEFORE", 15);
        quicklistReleaseIterator(iter);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 7. Insert that triggers node split
     *    Exercises: _quicklistSplitNode (zmalloc_usable + lpDeleteRange)
     *              + insert into split node
     * ======================================================== */
    {
        robj *list = createQuicklistObject(3, 0); /* fill=3, nodes fill up fast */
        quicklist *ql = objectGetVal(list);

        for (int i = 0; i < 12; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "split_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        TEST_ASSERT(ql->len == 4); /* 12 / 3 = 4 nodes */
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Insert in the middle of a full node — forces split */
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 4, &entry);
        TEST_ASSERT(iter != NULL);
        quicklistInsertAfter(iter, &entry, "SPLIT_INSERT", 12);
        quicklistReleaseIterator(iter);
        TEST_ASSERT(ql->len > 4); /* split must have created a new node */
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 8a. Insert into next neighbor (full node, at_tail, avail_next)
     *     Exercises: lpPrepend into next node in _quicklistInsert
     * ======================================================== */
    {
        robj *list = createQuicklistObject(4, 0); /* fill=4 */
        quicklist *ql = objectGetVal(list);

        /* Create two nodes: first full (4 items), second with 2 items */
        for (int i = 0; i < 6; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "neighbor_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        TEST_ASSERT(ql->len == 2);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Insert at end of first node (at_tail && after && avail_next)
         * First node is full (4), second has room (2 < 4). */
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
        TEST_ASSERT(iter != NULL);
        quicklistInsertAfter(iter, &entry, "NEIGHBOR_NEXT", 13);
        quicklistReleaseIterator(iter);
        TEST_ASSERT(ql->len == 2); /* should NOT have created a new node */
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 8b. Insert into prev neighbor (full node, at_head, avail_prev)
     *     Exercises: lpAppend into prev node in _quicklistInsert
     * ======================================================== */
    {
        robj *list = createQuicklistObject(4, 0); /* fill=4 */
        quicklist *ql = objectGetVal(list);

        /* Create: node0(4 full) + node1(4 full), then pop one from head
         * so node0 has room. */
        for (int i = 0; i < 8; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "avprev_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        TEST_ASSERT(ql->len == 2);
        /* Pop one from head to make room in node0 (3 items) while node1 stays full (4) */
        unsigned char *data; size_t sz; long long val;
        quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &val);
        if (data) zfree(data);
        TEST_ASSERT(ql->head->count == 3);
        TEST_ASSERT(ql->tail->count == 4);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Insert before head of node1 (at_head && !after && avail_prev)
         * node1 is full (4), its prev (node0) has room (3 < 4). */
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 3, &entry);
        TEST_ASSERT(iter != NULL);
        quicklistInsertBefore(iter, &entry, "NEIGHBOR_PREV", 13);
        quicklistReleaseIterator(iter);
        TEST_ASSERT(ql->len == 2); /* should NOT have created a new node */
        TEST_ASSERT(ql->head->count == 4); /* prev absorbed the insert */
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 9. ReplaceEntry — listpack path (lpReplace)
     *    Exercises: entry_alloc_sz read + lp_last_alloc_size after lpReplace
     * ======================================================== */
    {
        robj *list = createQuicklistObject(-2, 0);
        quicklist *ql = objectGetVal(list);

        for (int i = 0; i < 20; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "replace_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Replace with a smaller value */
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 5, &entry);
        TEST_ASSERT(iter != NULL);
        quicklistReplaceEntry(iter, &entry, "S", 1);
        quicklistReleaseIterator(iter);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Replace with a larger value */
        iter = quicklistGetIteratorEntryAtIdx(ql, 10, &entry);
        TEST_ASSERT(iter != NULL);
        char big_replace[200];
        memset(big_replace, 'X', sizeof(big_replace));
        quicklistReplaceEntry(iter, &entry, big_replace, sizeof(big_replace));
        quicklistReleaseIterator(iter);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 10. ReplaceEntry — plain node path (zmalloc_usable for new entry)
     *     Exercises: plain node replace with large element
     * ======================================================== */
    {
        quicklistSetPackedThreshold(64);

        robj *list = createQuicklistObject(-2, 0);
        quicklist *ql = objectGetVal(list);

        /* Create a plain node with a large element */
        char large[128];
        memset(large, 'A', sizeof(large));
        quicklistPushTail(ql, large, sizeof(large));
        /* Add some normal elements too */
        for (int i = 0; i < 10; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "mixed_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Replace the plain node with another large element */
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);
        TEST_ASSERT(iter != NULL);
        TEST_ASSERT(QL_NODE_IS_PLAIN(entry.node));
        char new_large[256];
        memset(new_large, 'B', sizeof(new_large));
        quicklistReplaceEntry(iter, &entry, new_large, sizeof(new_large));
        quicklistReleaseIterator(iter);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        quicklistSetPackedThreshold(0);
        decrRefCount(list);
    }

    /* ========================================================
     * 11. DelRange — partial and full node deletion
     *     Exercises: lpDeleteRange + tracked_size delta, and __quicklistDelNode
     * ======================================================== */
    {
        robj *list = createQuicklistObject(5, 0);
        quicklist *ql = objectGetVal(list);

        for (int i = 0; i < 30; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "delrange_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Delete a partial range from the middle (doesn't delete whole nodes) */
        quicklistDelRange(ql, 3, 2);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Delete a range spanning multiple nodes */
        quicklistDelRange(ql, 2, 15);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Delete from head */
        quicklistDelRange(ql, 0, 3);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Delete from tail (negative index) */
        quicklistDelRange(ql, -3, 3);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 12. Rotate (move tail to head)
     *     Exercises: quicklistPushHead + quicklistDelIndex on tail
     * ======================================================== */
    {
        robj *list = createQuicklistObject(3, 0);
        quicklist *ql = objectGetVal(list);

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

    /* ========================================================
     * 13. Dup (deep copy)
     *     Exercises: quicklistDup -> zmalloc_usable for each node entry
     * ======================================================== */
    {
        robj *list = createQuicklistObject(-2, 0);
        quicklist *ql = objectGetVal(list);

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

        /* Mutate the copy and verify tracking stays correct */
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

    /* ========================================================
     * 14. Dup with compression
     *     Exercises: quicklistDup with LZF-compressed nodes
     * ======================================================== */
    {
        robj *list = createQuicklistObject(-2, 1); /* compress depth=1 */
        quicklist *ql = objectGetVal(list);

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

    /* ========================================================
     * 15. Node merge via _quicklistListpackMerge
     *     Exercises: lpMerge + lp_last_alloc_size for merged node
     *     _quicklistMergeNodes is called after insert-triggered splits.
     *     Use fill=4: create 3 full nodes (12 elements), then delete
     *     elements from 2 adjacent nodes so they become small enough to
     *     merge when a split triggers _quicklistMergeNodes.
     * ======================================================== */
    {
        robj *list = createQuicklistObject(4, 0); /* fill=4, count-based */
        quicklist *ql = objectGetVal(list);

        /* Create 3 full nodes of 4 elements each = 12 elements */
        for (int i = 0; i < 12; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "merge_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        TEST_ASSERT(ql->len == 3);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Delete 3 elements from the second node to make it small (count=1) */
        quicklistDelRange(ql, 4, 3);
        /* Now: node0 has 4, node1 has 1, node2 has 4. Total 3 nodes, 9 elements. */
        TEST_ASSERT(ql->len == 3);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Delete 3 elements from the third node to make it small (count=1) */
        quicklistDelRange(ql, 5, 3);
        /* Now: node0 has 4, node1 has 1, node2 has 1. Total 3 nodes, 6 elements. */
        TEST_ASSERT(ql->len == 3);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        unsigned long nodes_before = ql->len;

        /* Insert into the middle of the full first node to trigger a split.
         * After split, _quicklistMergeNodes will try to merge neighbors.
         * node1 (count=1) and node2 (count=1) can merge since 1+1 <= fill=4. */
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
        TEST_ASSERT(iter != NULL);
        quicklistInsertAfter(iter, &entry, "MERGE_TRIGGER", 13);
        quicklistReleaseIterator(iter);

        /* The split created a new node, but merges should have reduced the count */
        TEST_ASSERT(ql->len <= nodes_before + 1); /* split adds 1, merges may reduce */
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 16. Plain node insert
     *     Exercises: __quicklistCreateNode with PLAIN container, zmalloc_usable
     * ======================================================== */
    {
        quicklistSetPackedThreshold(64);

        robj *list = createQuicklistObject(-2, 0);
        quicklist *ql = objectGetVal(list);

        /* Push small elements first */
        for (int i = 0; i < 5; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "small_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Push large elements that become plain nodes */
        for (int i = 0; i < 5; i++) {
            char large[128];
            memset(large, 'P' + i, sizeof(large));
            quicklistPushTail(ql, large, sizeof(large));
            ASSERT_TRACKED_SIZE_CORRECT(list);
        }

        /* Pop plain nodes */
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

    /* ========================================================
     * 17. Insert into empty list (no reference node)
     *     Exercises: _quicklistInsert with node==NULL path
     * ======================================================== */
    {
        robj *list = createQuicklistObject(-2, 0);
        quicklist *ql = objectGetVal(list);

        quicklistEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.quicklist = ql;
        quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
        quicklistInsertAfter(iter, &entry, "first_ever", 10);
        quicklistReleaseIterator(iter);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 18. Compression with operations interleaved
     *     Exercises: repeated compress/decompress cycles via mixed ops
     * ======================================================== */
    {
        robj *list = createQuicklistObject(-2, 2); /* compress depth=2 */
        quicklist *ql = objectGetVal(list);

        /* Build a large list so inner nodes get compressed */
        for (int i = 0; i < 1000; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "interleaved_%d_extra_padding_here", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Interleave pushes and pops from both ends */
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

        /* Replace elements in the middle (forces decompress + recompress) */
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

    /* ========================================================
     * 19. AppendListpack / AppendPlainNode (RDB load paths)
     *     Exercises: zmalloc_size at load time for entry_alloc_sz
     * ======================================================== */
    {
        robj *list = createQuicklistObject(-2, 0);
        quicklist *ql = objectGetVal(list);

        /* Simulate RDB load: create a listpack externally and append */
        unsigned char *lp = lpNew(0);
        for (int i = 0; i < 10; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "rdb_lp_%d", i);
            lp = lpAppend(lp, (unsigned char *)buf, strlen(buf));
        }
        quicklistAppendListpack(ql, lp);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Append another listpack */
        unsigned char *lp2 = lpNew(0);
        for (int i = 0; i < 5; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "rdb_lp2_%d", i);
            lp2 = lpAppend(lp2, (unsigned char *)buf, strlen(buf));
        }
        quicklistAppendListpack(ql, lp2);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Append a plain node */
        size_t plain_sz = 256;
        unsigned char *plain_data = zmalloc(plain_sz);
        memset(plain_data, 'Z', plain_sz);
        quicklistAppendPlainNode(ql, plain_data, plain_sz);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    /* ========================================================
     * 20. DelEntry via iterator
     *     Exercises: quicklistDelEntry -> quicklistDelIndex for each entry
     * ======================================================== */
    {
        robj *list = createQuicklistObject(5, 0);
        quicklist *ql = objectGetVal(list);

        for (int i = 0; i < 25; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "delentry_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Delete every other element via iterator */
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

    /* ========================================================
     * 21. ReplaceAtIndex
     *     Exercises: quicklistReplaceAtIndex -> quicklistReplaceEntry
     * ======================================================== */
    {
        robj *list = createQuicklistObject(-2, 0);
        quicklist *ql = objectGetVal(list);

        for (int i = 0; i < 30; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "replaceatidx_%d", i);
            quicklistPushTail(ql, buf, strlen(buf));
        }
        ASSERT_TRACKED_SIZE_CORRECT(list);

        /* Replace at various indices */
        quicklistReplaceAtIndex(ql, 0, "HEAD_REPLACED", 13);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        quicklistReplaceAtIndex(ql, 15, "MID_REPLACED", 12);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        quicklistReplaceAtIndex(ql, 29, "TAIL_REPLACED", 13);
        ASSERT_TRACKED_SIZE_CORRECT(list);

        decrRefCount(list);
    }

    return 0;
}
