#ifndef ORDERED_INDEX_H
#define ORDERED_INDEX_H

#include "sds.h"

/* Opaque types for ordered index, positions, and iterators */
typedef struct OrderedIndex OrderedIndex;
typedef struct OrderedIndexItem OrderedIndexItem;
typedef uint64_t OrderedIndexIterator[3];

/* Operations interface for ordered index implementations */
typedef struct OrderedIndexOps {
    /* Lifecycle */
    OrderedIndex *(*create)(void);
    void (*free)(OrderedIndex *idx);

    /* Modification */
    OrderedIndexItem *(*insert)(OrderedIndex *idx, double score, const_sds ele);
    void (*deleteItem)(OrderedIndex *idx, OrderedIndexItem *pos);
    OrderedIndexItem *(*update_score)(OrderedIndex *idx, OrderedIndexItem *pos, double newscore);
    OrderedIndexItem *(*pop_first)(OrderedIndex *idx);
    OrderedIndexItem *(*pop_last)(OrderedIndex *idx);
    void (*free_item)(OrderedIndexItem *item);
    unsigned long (*delete_range_by_score)(OrderedIndex *idx, double min, double max, int min_ex, int max_ex);
    unsigned long (*delete_range_by_rank)(OrderedIndex *idx, unsigned long start, unsigned long end);

    /* Query */
    unsigned long (*length)(OrderedIndex *idx);
    OrderedIndexItem *(*get_by_rank)(OrderedIndex *idx, unsigned long rank);
    long (*get_rank)(OrderedIndex *idx, const OrderedIndexItem *pos);
    void (*get_element_raw)(const OrderedIndexItem *pos, const char **ptr, size_t *len);
    double (*get_score)(const OrderedIndexItem *pos);

    /* Iterator */
    void (*init_iterator)(OrderedIndexIterator *iter, OrderedIndex *idx);
    void (*reset_iterator)(OrderedIndexIterator *iter);
    bool (*next)(OrderedIndexIterator *iter, OrderedIndexItem **pos);
    bool (*prev)(OrderedIndexIterator *iter, OrderedIndexItem **pos);
    void (*seek_to_rank)(OrderedIndexIterator *iter, unsigned long rank);
    void (*seek_to_score_range)(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset);
    void (*seek_to_lex_range)(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset);

    /* TODO: Add interface methods for memory management:
     * - Memory dismiss (for CoW optimization during fork/snapshot)
     * - Memory defrag (for active defragmentation when nodes are relocated)
     * These will eliminate direct access to forward/backward pointers in defrag.c and object.c */
} OrderedIndexOps;

/* Inline wrappers for performance (compiler can inline these) */
static inline OrderedIndex *orderedIndexCreate(const OrderedIndexOps *ops) {
    return ops->create();
}

static inline void orderedIndexFree(const OrderedIndexOps *ops, OrderedIndex *idx) {
    ops->free(idx);
}

static inline OrderedIndexItem *orderedIndexInsert(const OrderedIndexOps *ops, OrderedIndex *idx, double score, const_sds ele) {
    return ops->insert(idx, score, ele);
}

static inline void orderedIndexDelete(const OrderedIndexOps *ops, OrderedIndex *idx, OrderedIndexItem *pos) {
    ops->deleteItem(idx, pos);
}

static inline OrderedIndexItem *orderedIndexUpdateScore(const OrderedIndexOps *ops, OrderedIndex *idx, OrderedIndexItem *pos, double newscore) {
    return ops->update_score(idx, pos, newscore);
}

static inline OrderedIndexItem *orderedIndexPopFirst(const OrderedIndexOps *ops, OrderedIndex *idx) {
    return ops->pop_first(idx);
}

static inline OrderedIndexItem *orderedIndexPopLast(const OrderedIndexOps *ops, OrderedIndex *idx) {
    return ops->pop_last(idx);
}

static inline void orderedIndexFreeItem(const OrderedIndexOps *ops, OrderedIndexItem *item) {
    ops->free_item(item);
}

static inline unsigned long orderedIndexDeleteRangeByScore(const OrderedIndexOps *ops, OrderedIndex *idx, double min, double max, int min_ex, int max_ex) {
    return ops->delete_range_by_score(idx, min, max, min_ex, max_ex);
}

static inline unsigned long orderedIndexDeleteRangeByRank(const OrderedIndexOps *ops, OrderedIndex *idx, unsigned long start, unsigned long end) {
    return ops->delete_range_by_rank(idx, start, end);
}

static inline unsigned long orderedIndexLength(const OrderedIndexOps *ops, OrderedIndex *idx) {
    return ops->length(idx);
}

static inline OrderedIndexItem *orderedIndexGetByRank(const OrderedIndexOps *ops, OrderedIndex *idx, unsigned long rank) {
    return ops->get_by_rank(idx, rank);
}

static inline long orderedIndexGetRank(const OrderedIndexOps *ops, OrderedIndex *idx, const OrderedIndexItem *pos) {
    return ops->get_rank(idx, pos);
}

static inline void orderedIndexGetElementRaw(const OrderedIndexOps *ops, const OrderedIndexItem *pos, const char **ptr, size_t *len) {
    ops->get_element_raw(pos, ptr, len);
}

static inline double orderedIndexGetScore(const OrderedIndexOps *ops, const OrderedIndexItem *pos) {
    return ops->get_score(pos);
}

static inline void orderedIndexInitIterator(const OrderedIndexOps *ops, OrderedIndexIterator *iter, OrderedIndex *idx) {
    ops->init_iterator(iter, idx);
}

static inline void orderedIndexResetIterator(const OrderedIndexOps *ops, OrderedIndexIterator *iter) {
    ops->reset_iterator(iter);
}

static inline bool orderedIndexNext(const OrderedIndexOps *ops, OrderedIndexIterator *iter, OrderedIndexItem **pos) {
    return ops->next(iter, pos);
}

static inline bool orderedIndexPrev(const OrderedIndexOps *ops, OrderedIndexIterator *iter, OrderedIndexItem **pos) {
    return ops->prev(iter, pos);
}

static inline void orderedIndexSeekToRank(const OrderedIndexOps *ops, OrderedIndexIterator *iter, unsigned long rank) {
    ops->seek_to_rank(iter, rank);
}

static inline void orderedIndexSeekToScoreRange(const OrderedIndexOps *ops, OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset) {
    ops->seek_to_score_range(iter, min, max, min_ex, max_ex, offset);
}

static inline void orderedIndexSeekToLexRange(const OrderedIndexOps *ops, OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset) {
    ops->seek_to_lex_range(iter, min, max, min_ex, max_ex, offset);
}

/* Available implementations */
extern const OrderedIndexOps skiplistOrderedIndexOps;

#endif /* ORDERED_INDEX_H */
