#ifndef ORDERED_INDEX_H
#define ORDERED_INDEX_H

#include "sds.h"

/* Opaque types for ordered index, positions, and iterators */
typedef struct OrderedIndex OrderedIndex;
typedef struct OrderedIndexItem OrderedIndexItem;
typedef uint64_t OrderedIndexIterator[3];

/* Callback invoked for each item removed during a range-delete operation. */
typedef void (*OrderedIndexOnDelete)(OrderedIndexItem *item, void *ctx);

/* ---- Production inline wrappers ----
 *
 * Currently hardcoded to the skiplist implementation. When additional ordered
 * index backends are added (e.g. B-tree), a compile-time switch can select
 * the implementation here without changing any call sites. */
#include "skiplist_ordered_index.h"

/* Lifecycle */
static inline OrderedIndex *orderedIndexCreate(void) {
    return skiplistCreate();
}

static inline void orderedIndexFree(OrderedIndex *idx) {
    skiplistFree(idx);
}

/* Modification */
static inline OrderedIndexItem *orderedIndexInsertRaw(OrderedIndex *idx, double score, const char *ele, size_t len) {
    return skiplistInsert(idx, score, ele, len);
}

static inline OrderedIndexItem *orderedIndexInsert(OrderedIndex *idx, double score, const_sds ele) {
    return skiplistInsert(idx, score, ele, sdslen(ele));
}

static inline void orderedIndexDelete(OrderedIndex *idx, OrderedIndexItem *pos) {
    skiplistDelete(idx, pos);
}

static inline OrderedIndexItem *orderedIndexUpdateScore(OrderedIndex *idx, OrderedIndexItem *pos, double newscore) {
    return skiplistUpdateScore(idx, pos, newscore);
}

static inline OrderedIndexItem *orderedIndexPopFirst(OrderedIndex *idx) {
    return skiplistPopFirst(idx);
}

static inline OrderedIndexItem *orderedIndexPopLast(OrderedIndex *idx) {
    return skiplistPopLast(idx);
}

static inline void orderedIndexFreeItem(OrderedIndexItem *item) {
    skiplistFreeItem(item);
}

static inline unsigned long orderedIndexDeleteRangeByScore(OrderedIndex *idx, double min, double max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) {
    return skiplistDeleteRangeByScore(idx, min, max, min_ex, max_ex, on_delete, ctx);
}

static inline unsigned long orderedIndexDeleteRangeByRank(OrderedIndex *idx, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *ctx) {
    return skiplistDeleteRangeByRank(idx, start, end, on_delete, ctx);
}

static inline unsigned long orderedIndexDeleteRangeByLex(OrderedIndex *idx, const_sds min, const_sds max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) {
    return skiplistDeleteRangeByLex(idx, min, max, min_ex, max_ex, on_delete, ctx);
}

/* Query */
static inline unsigned long orderedIndexLength(OrderedIndex *idx) {
    return skiplistLength(idx);
}

static inline OrderedIndexItem *orderedIndexGetByRank(OrderedIndex *idx, unsigned long rank) {
    return skiplistGetByRank(idx, rank);
}

static inline unsigned long orderedIndexGetRank(OrderedIndex *idx, const OrderedIndexItem *pos) {
    return skiplistGetRank(idx, pos);
}

static inline void orderedIndexGetElementRaw(const OrderedIndexItem *pos, const char **ptr, size_t *len) {
    skiplistGetElementRaw(pos, ptr, len);
}

static inline double orderedIndexGetScore(const OrderedIndexItem *pos) {
    return skiplistGetScore(pos);
}

/* Memory */
static inline void orderedIndexDismissMemory(OrderedIndex *idx) {
    skiplistDismissMemory(idx);
}

static inline size_t orderedIndexEstimateMemory(OrderedIndex *idx, size_t sample_size) {
    return skiplistEstimateMemory(idx, sample_size);
}

/* Iterator */
static inline void orderedIndexInitIterator(OrderedIndexIterator *iter, OrderedIndex *idx) {
    skiplistInitIterator(iter, idx);
}

static inline void orderedIndexResetIterator(OrderedIndexIterator *iter) {
    skiplistResetIterator(iter);
}

static inline bool orderedIndexNext(OrderedIndexIterator *iter, OrderedIndexItem **pos) {
    return skiplistNext(iter, pos);
}

static inline bool orderedIndexPrev(OrderedIndexIterator *iter, OrderedIndexItem **pos) {
    return skiplistPrev(iter, pos);
}

static inline void orderedIndexSeekToRank(OrderedIndexIterator *iter, unsigned long rank) {
    skiplistSeekToRank(iter, rank);
}

/* Seek to a position within a score/lex range.
 *
 * offset >= 0: positions for forward iteration (next() returns the element).
 *   offset 0 = first element in range, 1 = second, etc.
 * offset < 0:  positions for reverse iteration (prev() returns the element).
 *   offset -1 = last element in range, -2 = second-to-last, etc. */
static inline void orderedIndexSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset) {
    skiplistSeekToScoreRange(iter, min, max, min_ex, max_ex, offset);
}

static inline void orderedIndexSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset) {
    skiplistSeekToLexRange(iter, min, max, min_ex, max_ex, offset);
}

/* Defrag */
typedef void (*OrderedIndexDefragCallback)(OrderedIndexItem *old_item, OrderedIndexItem *new_item, void *ctx);

static inline OrderedIndex *orderedIndexDefragInternals(OrderedIndex *idx, void *(*defragfn)(void *)) {
    return skiplistDefragInternals(idx, defragfn);
}

static inline unsigned long orderedIndexScanDefrag(OrderedIndex *idx, unsigned long cursor,
                                                   OrderedIndexDefragCallback callback, void *ctx,
                                                   void *(*defragfn)(void *)) {
    return skiplistScanDefrag(idx, cursor, callback, ctx, defragfn);
}

#endif /* ORDERED_INDEX_H */
