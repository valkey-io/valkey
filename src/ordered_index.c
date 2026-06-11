/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* OrderedIndex implementation — delegates to the active backend.
 * Currently only the skiplist backend exists. When a B+ tree backend is added,
 * a compile-time or link-time switch will select the implementation. */

#include "ordered_index.h"
#include "skiplist_ordered_index.h"

/* Lifecycle */

OrderedIndex *orderedIndexCreate(void) {
    return skiplistCreate();
}

void orderedIndexFree(OrderedIndex *oi) {
    skiplistFree(oi);
}

/* Modification */

OrderedIndexItem *orderedIndexInsert(OrderedIndex *oi, double score, const char *ele, size_t len) {
    return skiplistInsert(oi, score, ele, len);
}

void orderedIndexDelete(OrderedIndex *oi, OrderedIndexItem *item) {
    skiplistDelete(oi, item);
}

OrderedIndexItem *orderedIndexUpdateScore(OrderedIndex *oi, OrderedIndexItem *item, double newscore) {
    return skiplistUpdateScore(oi, item, newscore);
}

OrderedIndexItem *orderedIndexPopFirst(OrderedIndex *oi) {
    return skiplistPopFirst(oi);
}

OrderedIndexItem *orderedIndexPopLast(OrderedIndex *oi) {
    return skiplistPopLast(oi);
}

void orderedIndexFreeItem(OrderedIndexItem *item) {
    skiplistFreeItem(item);
}

OrderedIndexItem *orderedIndexCreateDetached(double score, const char *ele, size_t len) {
    return skiplistCreateDetached(score, ele, len);
}

void orderedIndexDetachedSetScore(OrderedIndexItem *item, double score) {
    skiplistDetachedSetScore(item, score);
}

OrderedIndexItem *orderedIndexInsertDetached(OrderedIndex *oi, OrderedIndexItem *item) {
    return skiplistInsertDetached(oi, item);
}

unsigned long orderedIndexDeleteRangeByScore(OrderedIndex *oi, double min, double max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) {
    return skiplistDeleteRangeByScore(oi, min, max, min_ex, max_ex, on_delete, ctx);
}

unsigned long orderedIndexDeleteRangeByIndex(OrderedIndex *oi, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *ctx) {
    return skiplistDeleteRangeByIndex(oi, start, end, on_delete, ctx);
}

unsigned long orderedIndexDeleteRangeByLex(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) {
    return skiplistDeleteRangeByLex(oi, min, max, min_ex, max_ex, on_delete, ctx);
}

/* Query */

unsigned long orderedIndexLength(OrderedIndex *oi) {
    return skiplistLength(oi);
}

OrderedIndexItem *orderedIndexGetByIndex(OrderedIndex *oi, unsigned long rank) {
    return skiplistGetByIndex(oi, rank);
}

OrderedIndexItem *orderedIndexGetFirst(OrderedIndex *oi) {
    return skiplistGetFirst(oi);
}

OrderedIndexItem *orderedIndexGetLast(OrderedIndex *oi) {
    return skiplistGetLast(oi);
}

unsigned long orderedIndexGetIndex(OrderedIndex *oi, const OrderedIndexItem *item) {
    return skiplistGetIndex(oi, item);
}

void orderedIndexGetElementRaw(const OrderedIndexItem *item, const char **ptr, size_t *len) {
    skiplistGetElementRaw(item, ptr, len);
}

double orderedIndexGetScore(const OrderedIndexItem *item) {
    return skiplistGetScore(item);
}

unsigned long orderedIndexCountScoreRange(OrderedIndex *oi, double min, double max, int min_ex, int max_ex) {
    return skiplistCountScoreRange(oi, min, max, min_ex, max_ex);
}

unsigned long orderedIndexCountLexRange(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex) {
    return skiplistCountLexRange(oi, min, max, min_ex, max_ex);
}

/* Iterator */

void orderedIndexInitIterator(OrderedIndexIterator *iter, OrderedIndex *oi) {
    skiplistInitIterator(iter, oi);
}

void orderedIndexResetIterator(OrderedIndexIterator *iter) {
    skiplistResetIterator(iter);
}

OrderedIndexItem *orderedIndexNext(OrderedIndexIterator *iter) {
    return skiplistNext(iter);
}

OrderedIndexItem *orderedIndexPrev(OrderedIndexIterator *iter) {
    return skiplistPrev(iter);
}

void orderedIndexSeekToIndex(OrderedIndexIterator *iter, unsigned long rank) {
    skiplistSeekToIndex(iter, rank);
}

void orderedIndexSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset) {
    skiplistSeekToScoreRange(iter, min, max, min_ex, max_ex, offset);
}

void orderedIndexSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset) {
    skiplistSeekToLexRange(iter, min, max, min_ex, max_ex, offset);
}

/* Memory */

void orderedIndexDismissMemory(OrderedIndex *oi) {
    skiplistDismissMemory(oi);
}

size_t orderedIndexEstimateMemory(OrderedIndex *oi, size_t sample_size) {
    return skiplistEstimateMemory(oi, sample_size);
}

OrderedIndex *orderedIndexDefragInternals(OrderedIndex *oi, void *(*defragfn)(void *)) {
    return skiplistDefragInternals(oi, defragfn);
}

unsigned long orderedIndexScanDefrag(OrderedIndex *oi, unsigned long cursor, OrderedIndexDefragCallback callback, void *ctx, void *(*defragfn)(void *)) {
    return skiplistScanDefrag(oi, cursor, callback, ctx, defragfn);
}

/* Not declared in ordered_index.h — debug-only introspection. */
int orderedIndexGetDepth(OrderedIndex *oi) {
    return skiplistGetHeight(oi);
}
