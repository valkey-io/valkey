/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* OrderedIndex — skiplist-backed implementation of ordered (score, element) storage.
 *
 * Implements all functions declared in ordered_index.h using a skiplist as the
 * underlying data structure. */

#include "server.h"
#include "ordered_index.h"
#include "skiplist.h"

static_assert(sizeof(OrderedIndexIterator) >= sizeof(zslIter),
              "OrderedIndexIterator must be large enough to hold zslIter");

/*-----------------------------------------------------------------------------
 * Lifecycle
 *---------------------------------------------------------------------------*/

OrderedIndex *orderedIndexCreate(void) {
    return (OrderedIndex *)zslCreate();
}

void orderedIndexFree(OrderedIndex *oi) {
    zslFree((zskiplist *)oi);
}

/*-----------------------------------------------------------------------------
 * Modification
 *---------------------------------------------------------------------------*/

OrderedIndexItem *orderedIndexInsert(OrderedIndex *oi, double score, const char *ele, size_t len) {
    zskiplistNode *node = zslCreateNode(zslRandomLevel(), score, ele, len);
    zslInsertNode((zskiplist *)oi, node);
    return (OrderedIndexItem *)node;
}

void orderedIndexDelete(OrderedIndex *oi, OrderedIndexItem *node) {
    zslDelete((zskiplist *)oi, (zskiplistNode *)node);
}

OrderedIndexItem *orderedIndexUpdateScore(OrderedIndex *oi, OrderedIndexItem *node, double newscore) {
    zskiplistNode *result = zslUpdateScore((zskiplist *)oi, (zskiplistNode *)node, newscore);
    return result ? (OrderedIndexItem *)result : (OrderedIndexItem *)node;
}

OrderedIndexItem *orderedIndexPopFirst(OrderedIndex *oi) {
    zskiplist *zsl = (zskiplist *)oi;
    zskiplistNode *first = zslGetFirst(zsl);
    if (!first) return NULL;
    zslDetachNode(zsl, first);
    return (OrderedIndexItem *)first;
}

OrderedIndexItem *orderedIndexPopLast(OrderedIndex *oi) {
    zskiplist *zsl = (zskiplist *)oi;
    zskiplistNode *last = zslGetTail(zsl);
    if (!last) return NULL;
    zslDetachNode(zsl, last);
    return (OrderedIndexItem *)last;
}

void orderedIndexFreeItem(OrderedIndexItem *item) {
    zslFreeNode((zskiplistNode *)item);
}

OrderedIndexItem *orderedIndexCreateDetached(double score, const char *ele, size_t len) {
    zskiplistNode *node = zslCreateNode(zslRandomLevel(), score, ele, len);
    return (OrderedIndexItem *)node;
}

void orderedIndexDetachedSetScore(OrderedIndexItem *item, double score) {
    ((zskiplistNode *)item)->score = score;
}

OrderedIndexItem *orderedIndexInsertDetached(OrderedIndex *oi, OrderedIndexItem *item) {
    zskiplistNode *node = zslInsertNode((zskiplist *)oi, (zskiplistNode *)item);
    return (OrderedIndexItem *)node;
}

unsigned long orderedIndexDeleteRangeByScore(OrderedIndex *oi, double min, double max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) {
    zskiplist *zsl = (zskiplist *)oi;
    zrangespec range = {.min = min, .max = max, .minex = min_ex, .maxex = max_ex};
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zslGetHeader(zsl);
    for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (x->level[i].forward && !zsetScoreGteMin(x->level[i].forward->score, &range))
            x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x && zsetScoreLteMax(x->score, &range)) {
        zskiplistNode *next = x->level[0].forward;
        zslUnlinkNode(zsl, x, update);
        if (on_delete) on_delete((OrderedIndexItem *)x, ctx);
        zslFreeNode(x);

        removed++;
        x = next;
    }
    return removed;
}

unsigned long orderedIndexDeleteRangeByIndex(OrderedIndex *oi, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *ctx) {
    zskiplist *zsl = (zskiplist *)oi;
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    /* Convert 0-based inclusive range to 1-based for internal traversal. */
    start++;
    end++;

    x = zslGetHeader(zsl);
    for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + zslGetNodeSpanAtLevel(x, i)) < start) {
            traversed += zslGetNodeSpanAtLevel(x, i);
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    traversed++;
    x = x->level[0].forward;
    while (x && traversed <= end) {
        zskiplistNode *next = x->level[0].forward;
        zslUnlinkNode(zsl, x, update);
        if (on_delete) on_delete((OrderedIndexItem *)x, ctx);
        zslFreeNode(x);

        removed++;
        traversed++;
        x = next;
    }
    return removed;
}

unsigned long orderedIndexDeleteRangeByLex(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) {
    zskiplist *zsl = (zskiplist *)oi;
    zlexrangespec range = {.min = (sds)min, .max = (sds)max, .minex = min_ex, .maxex = max_ex};
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zslGetHeader(zsl);
    for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (x->level[i].forward) {
            sds fwd_ele = zslGetNodeElement(x->level[i].forward);
            if (zsetLexGteMin(fwd_ele, sdslen(fwd_ele), &range)) break;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* Current node is the last with element < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x) {
        sds ele = zslGetNodeElement(x);
        if (!zsetLexLteMax(ele, sdslen(ele), &range)) break;
        zskiplistNode *next = x->level[0].forward;
        zslUnlinkNode(zsl, x, update);
        if (on_delete) on_delete((OrderedIndexItem *)x, ctx);
        zslFreeNode(x);

        removed++;
        x = next;
    }
    return removed;
}

/*-----------------------------------------------------------------------------
 * Query
 *---------------------------------------------------------------------------*/

unsigned long orderedIndexLength(OrderedIndex *oi) {
    return zslGetLength((zskiplist *)oi);
}

OrderedIndexItem *orderedIndexGetByIndex(OrderedIndex *oi, unsigned long index) {
    return (OrderedIndexItem *)zslGetElementByRank((zskiplist *)oi, index + 1);
}

OrderedIndexItem *orderedIndexGetFirst(OrderedIndex *oi) {
    return (OrderedIndexItem *)zslGetFirst((const zskiplist *)oi);
}

OrderedIndexItem *orderedIndexGetLast(OrderedIndex *oi) {
    return (OrderedIndexItem *)zslGetTail((const zskiplist *)oi);
}

unsigned long orderedIndexGetIndex(OrderedIndex *oi, const OrderedIndexItem *node) {
    return zslGetRank((zskiplist *)oi, (const zskiplistNode *)node) - 1;
}

void orderedIndexGetElementRaw(const OrderedIndexItem *node, const char **ptr, size_t *len) {
    const zskiplistNode *znode = (const zskiplistNode *)node;
    sds ele = zslGetNodeElement(znode);
    *ptr = ele;
    *len = sdslen(ele);
}

double orderedIndexGetScore(const OrderedIndexItem *node) {
    return zslGetScore((const zskiplistNode *)node);
}

unsigned long orderedIndexCountScoreRange(OrderedIndex *oi, double min, double max, int min_ex, int max_ex) {
    zskiplist *zsl = (zskiplist *)oi;
    zrangespec range = {.min = min, .max = max, .minex = min_ex, .maxex = max_ex};
    long first_rank, last_rank;

    /* Find first element in range and its rank. */
    zskiplistNode *first = zslNthInRange(zsl, &range, 0, &first_rank);
    if (first == NULL) return 0;

    /* Find last element in range and its rank. */
    zskiplistNode *last_node = zslNthInRange(zsl, &range, -1, &last_rank);
    if (last_node == NULL) return 0;

    return (unsigned long)(last_rank - first_rank + 1);
}

unsigned long orderedIndexCountLexRange(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex) {
    zskiplist *zsl = (zskiplist *)oi;
    zlexrangespec range = {.min = (sds)min, .max = (sds)max, .minex = min_ex, .maxex = max_ex};

    /* Find first element in range. */
    zskiplistNode *first = zslNthInLexRange(zsl, &range, 0);
    if (first == NULL) return 0;
    unsigned long first_rank = zslGetRank(zsl, first);

    /* Find last element in range. */
    zskiplistNode *last_node = zslNthInLexRange(zsl, &range, -1);
    if (last_node == NULL) return 0;
    unsigned long last_rank = zslGetRank(zsl, last_node);

    return last_rank - first_rank + 1;
}

/*-----------------------------------------------------------------------------
 * Iterator
 *---------------------------------------------------------------------------*/

void orderedIndexInitIterator(OrderedIndexIterator *iter, OrderedIndex *oi) {
    zslInitIterator((zslIter *)iter, (zskiplist *)oi);
}

void orderedIndexResetIterator(OrderedIndexIterator *iter) {
    zslResetIterator((zslIter *)iter);
}

OrderedIndexItem *orderedIndexNext(OrderedIndexIterator *iter) {
    return (OrderedIndexItem *)zslNext((zslIter *)iter);
}

OrderedIndexItem *orderedIndexPrev(OrderedIndexIterator *iter) {
    return (OrderedIndexItem *)zslPrev((zslIter *)iter);
}

void orderedIndexSeekToIndex(OrderedIndexIterator *iter, unsigned long index) {
    zslSeekToRank((zslIter *)iter, index + 1);
}

void orderedIndexSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset) {
    zslSeekToScoreRange((zslIter *)iter, min, max, min_ex, max_ex, offset);
}

void orderedIndexSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset) {
    zslSeekToLexRange((zslIter *)iter, min, max, min_ex, max_ex, offset);
}

/*-----------------------------------------------------------------------------
 * Memory
 *---------------------------------------------------------------------------*/

void orderedIndexDismissMemory(OrderedIndex *oi) {
    zskiplist *zsl = (zskiplist *)oi;
    zskiplistNode *zn = zslGetTail(zsl);
    while (zn != NULL) {
        zskiplistNode *prev = zn->backward;
        dismissMemory(zn, 0);
        zn = prev;
    }
}

size_t orderedIndexEstimateMemory(OrderedIndex *oi, size_t sample_size) {
    zskiplist *zsl = (zskiplist *)oi;
    unsigned long length = zslGetLength(zsl);
    size_t asize = zslGetAllocSize();

    if (length == 0) return asize;

    size_t elesize = 0;
    size_t samples = 0;
    zskiplistNode *znode = zslGetHeader(zsl)->level[0].forward;
    while (znode != NULL && samples < sample_size) {
        elesize += zmalloc_size(znode);
        samples++;
        znode = znode->level[0].forward;
    }
    if (samples) asize += (double)elesize / samples * length;
    return asize;
}

/*-----------------------------------------------------------------------------
 * Defrag
 *---------------------------------------------------------------------------*/

OrderedIndex *orderedIndexDefragInternals(OrderedIndex *oi, void *(*defragfn)(void *)) {
    OrderedIndex *new_oi = defragfn(oi);
    return new_oi; /* NULL if no move needed */
}

/* Patch skiplist pointers after a node has been reallocated to a new address.
 * update[] contains the predecessor at each level. */
static void patchNodePointers(zskiplist *zsl, zskiplistNode *oldnode, zskiplistNode *newnode, zskiplistNode **update) {
    for (int i = 0; i < zslGetHeight(zsl); i++) {
        if (update[i]->level[i].forward == oldnode)
            update[i]->level[i].forward = newnode;
    }
    if (newnode->level[0].forward) {
        newnode->level[0].forward->backward = newnode;
    } else {
        zslSetTail(zsl, newnode);
    }
}

unsigned long orderedIndexScanDefrag(OrderedIndex *oi, unsigned long cursor, OrderedIndexDefragCallback callback, void *ctx, void *(*defragfn)(void *)) {
    zskiplist *zsl = (zskiplist *)oi;
    zskiplistNode *header = zslGetHeader(zsl);

    /* cursor is the 1-based rank of the next node to process, 0 means start */
    zskiplistNode *prev = header;
    if (cursor > 0) {
        /* Seek to the node just before cursor position */
        zskiplistNode *target = zslGetElementByRank(zsl, cursor);
        if (target == NULL) return 0; /* past end */
        prev = target->backward ? target->backward : header;
    }

    zskiplistNode *node = prev->level[0].forward;
    unsigned long count = 0;
    unsigned long rank = cursor > 0 ? cursor : 1;

    while (node != NULL && count < 16) {
        zskiplistNode *next = node->level[0].forward;
        zskiplistNode *newnode = defragfn(node);

        if (newnode) {
            /* Node was reallocated. Find predecessors and patch pointers. */
            zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
            zskiplistNode *x = header;
            double score = newnode->score;
            sds ele = zslGetNodeElement(newnode);
            for (int i = zslGetHeight(zsl) - 1; i >= 0; i--) {
                while (x->level[i].forward &&
                       (x->level[i].forward->score < score ||
                        (x->level[i].forward->score == score &&
                         sdscmp(zslGetNodeElement(x->level[i].forward), ele) < 0))) {
                    x = x->level[i].forward;
                }
                update[i] = x;
            }
            patchNodePointers(zsl, node, newnode, update);
            callback((OrderedIndexItem *)node, (OrderedIndexItem *)newnode, ctx);
        }

        node = next;
        rank++;
        count++;
    }

    return node ? rank : 0; /* 0 = done */
}

/*-----------------------------------------------------------------------------
 * Debug / Verification (used by unit tests and DEBUG command)
 *---------------------------------------------------------------------------*/

int orderedIndexGetHeight(OrderedIndex *oi) {
    return zslGetHeight((zskiplist *)oi);
}

int orderedIndexVerifyIntegrity(OrderedIndex *oi, char *errmsg, size_t errmsg_len) {
    zskiplist *zsl = (zskiplist *)oi;
    zskiplistNode *header = zslGetHeader(zsl);
    int height = zslGetHeight(zsl);
    unsigned long length = zslGetLength(zsl);

#define FAIL(...)                                  \
    do {                                           \
        snprintf(errmsg, errmsg_len, __VA_ARGS__); \
        return 0;                                  \
    } while (0)

    /* 1. Height must be in [1, ZSKIPLIST_MAXLEVEL]. */
    if (height < 1 || height > ZSKIPLIST_MAXLEVEL)
        FAIL("height %d out of range [1, %d]", height, ZSKIPLIST_MAXLEVEL);

    /* 2. All levels above height must have NULL forward from header. */
    for (int i = height; i < ZSKIPLIST_MAXLEVEL; i++) {
        if (header->level[i].forward != NULL)
            FAIL("header level %d forward is non-NULL above height %d", i, height);
    }

    /* 3. Walk level 0 to count nodes, verify ordering, backward pointers, and tail. */
    unsigned long count = 0;
    zskiplistNode *prev = NULL;
    zskiplistNode *node = header->level[0].forward;
    zskiplistNode *last = NULL;

    while (node != NULL) {
        count++;

        /* Verify backward pointer. */
        if (node->backward != prev)
            FAIL("node at rank %lu: backward pointer mismatch", count);

        /* Verify sort order (score, then element). */
        if (prev != NULL) {
            if (node->score < prev->score)
                FAIL("node at rank %lu: score %.17g < previous %.17g", count, node->score, prev->score);
            if (node->score == prev->score) {
                sds prev_ele = zslGetNodeElement(prev);
                sds node_ele = zslGetNodeElement(node);
                if (sdscmp(node_ele, prev_ele) <= 0)
                    FAIL("node at rank %lu: element not lexicographically after previous at same score", count);
            }
        }

        /* Verify node height is in valid range. */
        unsigned long node_height = zslGetNodeHeight(node);
        if (node_height < 1 || node_height > (unsigned long)ZSKIPLIST_MAXLEVEL)
            FAIL("node at rank %lu: height %lu out of range", count, node_height);

        /* Node height should not exceed skiplist height. */
        if (node_height > (unsigned long)height)
            FAIL("node at rank %lu: height %lu exceeds skiplist height %d", count, node_height, height);

        last = node;
        prev = node;
        node = node->level[0].forward;
    }

    /* 4. Verify length. */
    if (count != length)
        FAIL("length mismatch: stored %lu, counted %lu", length, count);

    /* 5. Verify tail pointer. */
    zskiplistNode *tail = zslGetTail(zsl);
    if (length == 0) {
        if (tail != NULL)
            FAIL("tail should be NULL for empty skiplist");
    } else {
        if (tail != last)
            FAIL("tail pointer does not point to last node");
    }

    /* 6. Verify the highest non-empty level matches height. */
    if (length > 0) {
        if (header->level[height - 1].forward == NULL)
            FAIL("highest level %d has NULL forward but skiplist is non-empty", height - 1);
    }

    /* 7. Verify spans at each level. */
    for (int i = 1; i < height; i++) {
        unsigned long rank = 0;
        zskiplistNode *x = header;

        while (x != NULL) {
            zskiplistNode *next_at_level = x->level[i].forward;
            unsigned long span = zslGetNodeSpanAtLevel(x, i);

            if (next_at_level == NULL) {
                unsigned long remaining = length - rank;
                if (span != remaining)
                    FAIL("level %d: node at rank %lu has span %lu but %lu nodes remain",
                         i, rank, span, remaining);
                break;
            }

            if (zslGetNodeHeight(next_at_level) <= (unsigned long)i)
                FAIL("level %d: forward node has height %lu, expected > %d",
                     i, zslGetNodeHeight(next_at_level), i);

            unsigned long actual_span = 0;
            zskiplistNode *walk_next = (x == header) ? header->level[0].forward : x->level[0].forward;
            while (walk_next != NULL && walk_next != next_at_level) {
                actual_span++;
                walk_next = walk_next->level[0].forward;
            }
            actual_span++;

            if (walk_next != next_at_level)
                FAIL("level %d: forward pointer from rank %lu does not appear in level-0 chain", i, rank);

            if (span != actual_span)
                FAIL("level %d: node at rank %lu has span %lu but actual distance is %lu",
                     i, rank, span, actual_span);

            rank += span;
            x = next_at_level;
        }
    }

#undef FAIL
    if (errmsg_len > 0) errmsg[0] = '\0';
    return 1;
}
