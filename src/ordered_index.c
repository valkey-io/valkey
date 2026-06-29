/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* OrderedIndex implementation backed by fbtree (feature B+tree).
 *
 * The fbtree stores packed keys: [8-byte normalized score][element bytes].
 * Lexicographic byte comparison of packed keys gives correct score+element
 * ordering. The hashtable uses lookup-key marking to distinguish plain sds
 * lookup keys from packed stored items. */

#include "server.h"
#include "ordered_index.h"
#include "fbtree.h"
#include "endianconv.h"

static_assert(sizeof(OrderedIndexIterator) >= sizeof(fbtreeIterator),
              "OrderedIndexIterator must be large enough to hold fbtreeIterator");

#define SCORE_SIZE 8 /* Normalized score prefix size */

/* Forward declarations for range seek helpers. */
static void seekForBound(fbtreeIterator *fbt_iter, sds packed, int reverse, int inclusive);
static void skipElements(fbtreeIterator *fbt_iter, long count, int reverse);
static sds packLexBound(uint64_t score_prefix, const_sds element);

/* ==========================================================================
 * Score Normalization
 * Converts IEEE 754 double to a sortable 8-byte big-endian representation.
 * Lexicographic byte comparison matches numeric order after transformation.
 * ========================================================================== */

static inline uint64_t scoreToSortable(double score) {
    uint64_t bits;
    memcpy(&bits, &score, sizeof(bits));
    if (bits & (1ULL << 63)) {
        bits = ~bits;
    } else {
        bits ^= (1ULL << 63);
    }
    return htonu64(bits);
}

static inline double sortableToScore(uint64_t be) {
    uint64_t bits = ntohu64(be);
    if (bits & (1ULL << 63)) {
        bits ^= (1ULL << 63);
    } else {
        bits = ~bits;
    }
    double score;
    memcpy(&score, &bits, sizeof(score));
    return score;
}

/* Pack score and element into sds: [8-byte sortable score][element] */
static sds packScoreElement(double score, const char *ele, size_t ele_len) {
    uint64_t sortable = scoreToSortable(score);
    size_t total = SCORE_SIZE + ele_len;
    sds packed = sdsnewlen(NULL, total);
    memcpy(packed, &sortable, SCORE_SIZE);
    memcpy(packed + SCORE_SIZE, ele, ele_len);
    return packed;
}

static inline const char *unpackElement(const_sds packed, size_t *len) {
    *len = sdslen(packed) - SCORE_SIZE;
    return packed + SCORE_SIZE;
}

static inline double unpackScore(const_sds packed) {
    uint64_t sortable;
    memcpy(&sortable, packed, SCORE_SIZE);
    return sortableToScore(sortable);
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

OrderedIndex *orderedIndexCreate(void) {
    return (OrderedIndex *)fbtreeCreate();
}

void orderedIndexFree(OrderedIndex *oi) {
    fbtreeFree((fbtreeIndex *)oi);
}

/* ==========================================================================
 * Modification
 * ========================================================================== */

OrderedIndexItem *orderedIndexInsert(OrderedIndex *oi, double score, const char *ele, size_t len) {
    sds packed = packScoreElement(score, ele, len);
    return (OrderedIndexItem *)fbtreeInsert((fbtreeIndex *)oi, packed);
}

void orderedIndexDelete(OrderedIndex *oi, OrderedIndexItem *item) {
    fbtreeDelete((fbtreeIndex *)oi, (const_sds)item);
}

OrderedIndexItem *orderedIndexUpdateScore(OrderedIndex *oi, OrderedIndexItem *item, double newscore) {
    const_sds packed = (const_sds)item;
    size_t ele_len;
    const char *ele = unpackElement(packed, &ele_len);
    sds new_packed = packScoreElement(newscore, ele, ele_len);
    fbtreeDelete((fbtreeIndex *)oi, packed);
    return (OrderedIndexItem *)fbtreeInsert((fbtreeIndex *)oi, new_packed);
}

OrderedIndexItem *orderedIndexPopFirst(OrderedIndex *oi) {
    return (OrderedIndexItem *)fbtreePopMin((fbtreeIndex *)oi);
}

OrderedIndexItem *orderedIndexPopLast(OrderedIndex *oi) {
    return (OrderedIndexItem *)fbtreePopMax((fbtreeIndex *)oi);
}

void orderedIndexItemFree(OrderedIndexItem *item) {
    sdsfree((sds)item);
}

OrderedIndexItem *orderedIndexItemCreate(double score, const char *ele, size_t len) {
    return (OrderedIndexItem *)packScoreElement(score, ele, len);
}

void orderedIndexItemSetScore(OrderedIndexItem *item, double score) {
    uint64_t sortable = scoreToSortable(score);
    memcpy((char *)item, &sortable, SCORE_SIZE);
}

OrderedIndexItem *orderedIndexInsertItem(OrderedIndex *oi, OrderedIndexItem *item) {
    return (OrderedIndexItem *)fbtreeInsert((fbtreeIndex *)oi, (sds)item);
}

/* ==========================================================================
 * Range Deletes
 * ========================================================================== */

/* Helper: range delete with on_delete callback.
 * The fbtree frees the sds after this callback returns,
 * so we must NOT free here -- only notify the caller. */
typedef struct {
    OrderedIndexOnDelete on_delete;
    void *user_ctx;
} rangeDeleteArgs;

static void rangeDeleteCallback(sds item, void *ctx) {
    rangeDeleteArgs *args = (rangeDeleteArgs *)ctx;
    if (args->on_delete) {
        args->on_delete((OrderedIndexItem *)item, args->user_ctx);
    }
}

unsigned long orderedIndexDeleteRangeByScore(OrderedIndex *oi, double min, double max, bool min_ex, bool max_ex, OrderedIndexOnDelete on_delete, void *privdata) {
    uint64_t min_sortable = scoreToSortable(min);
    uint64_t max_sortable = scoreToSortable(max);
    rangeDeleteArgs args = {on_delete, privdata};
    return fbtreeDeleteRangeByScore((fbtreeIndex *)oi, (const char *)&min_sortable, (const char *)&max_sortable, min_ex, max_ex, rangeDeleteCallback, &args);
}

unsigned long orderedIndexDeleteRangeByIndex(OrderedIndex *oi, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *privdata) {
    rangeDeleteArgs args = {on_delete, privdata};
    return fbtreeDeleteRangeByRank((fbtreeIndex *)oi, start, end, rangeDeleteCallback, &args);
}

unsigned long orderedIndexDeleteRangeByLex(OrderedIndex *oi, const_sds min, const_sds max, bool min_ex, bool max_ex, OrderedIndexOnDelete on_delete, void *privdata) {
    fbtreeIndex *fbt = (fbtreeIndex *)oi;
    if (fbtreeLength(fbt) == 0) return 0;
    if (max == shared.minstring || min == shared.maxstring) return 0;

    const_sds first = fbtreePeekMin(fbt);
    if (!first) return 0;
    uint64_t score_prefix;
    memcpy(&score_prefix, first, SCORE_SIZE);

    sds min_packed, max_packed;

    if (min == shared.minstring) {
        min_packed = sdsnewlen(NULL, SCORE_SIZE);
        memcpy(min_packed, &score_prefix, SCORE_SIZE);
    } else {
        size_t min_len = sdslen(min);
        min_packed = sdsnewlen(NULL, SCORE_SIZE + min_len);
        memcpy(min_packed, &score_prefix, SCORE_SIZE);
        memcpy(min_packed + SCORE_SIZE, min, min_len);
    }

    if (max == shared.maxstring) {
        max_packed = sdsnewlen(NULL, SCORE_SIZE + 1);
        memcpy(max_packed, &score_prefix, SCORE_SIZE);
        memset(max_packed + SCORE_SIZE, 0xFF, 1);
    } else {
        size_t max_len = sdslen(max);
        max_packed = sdsnewlen(NULL, SCORE_SIZE + max_len);
        memcpy(max_packed, &score_prefix, SCORE_SIZE);
        memcpy(max_packed + SCORE_SIZE, max, max_len);
    }

    rangeDeleteArgs args = {on_delete, privdata};
    unsigned long deleted = fbtreeDeleteRangeByValue(fbt, min_packed, max_packed, min_ex, max_ex, rangeDeleteCallback, &args);
    sdsfree(min_packed);
    sdsfree(max_packed);
    return deleted;
}

/* ==========================================================================
 * Query
 * ========================================================================== */

unsigned long orderedIndexLength(const OrderedIndex *oi) {
    return fbtreeLength((fbtreeIndex *)oi);
}

OrderedIndexItem *orderedIndexGetByIndex(const OrderedIndex *oi, unsigned long index) {
    return (OrderedIndexItem *)fbtreeGetAtRank((fbtreeIndex *)oi, index);
}

OrderedIndexItem *orderedIndexGetFirst(const OrderedIndex *oi) {
    return (OrderedIndexItem *)fbtreePeekMin((fbtreeIndex *)oi);
}

OrderedIndexItem *orderedIndexGetLast(const OrderedIndex *oi) {
    return (OrderedIndexItem *)fbtreePeekMax((fbtreeIndex *)oi);
}

unsigned long orderedIndexGetIndex(const OrderedIndex *oi, const OrderedIndexItem *item) {
    long rank = fbtreeGetIndexOfItem((fbtreeIndex *)oi, (const_sds)item);
    return (unsigned long)rank;
}

void orderedIndexItemGetElement(const OrderedIndexItem *item, const char **ptr, size_t *len) {
    *len = sdslen((const_sds)item) - SCORE_SIZE;
    *ptr = (const char *)item + SCORE_SIZE;
}

double orderedIndexItemGetScore(const OrderedIndexItem *item) {
    return unpackScore((const_sds)item);
}

unsigned long orderedIndexCountScoreRange(const OrderedIndex *oi, double min, double max, bool min_ex, bool max_ex) {
    fbtreeIndex *fbt = (fbtreeIndex *)oi;
    fbtreeIterator iter;
    fbtreeInitIterator(&iter, fbt);

    /* Count via rank arithmetic: count = end_rank - start_rank, where each
     * rank is an O(log N) tree descent. fbtreeSeekToScore returns the rank of
     * the first item whose score is >= the seek key (i.e. the number of items
     * with score strictly less than it). scoreToSortable is order-preserving,
     * so +1 in sortable space is the next representable score (nextafter toward
     * +inf); the increment is done in native byte order since sortable is BE. */

    /* Lower bound: rank of the first in-range item.
     * Inclusive min -> first item with score >= min.
     * Exclusive min -> first item with score > min (seek to min+1). */
    uint64_t lo = scoreToSortable(min);
    if (min_ex) {
        uint64_t native = ntohu64(lo);
        native++;
        lo = htonu64(native);
    }
    long start_rank = fbtreeSeekToScore((const char *)&lo, &iter);

    /* Upper bound: rank of the first item past the range.
     * Exclusive max -> first item with score >= max (seek to max).
     * Inclusive max -> first item with score > max (seek to max+1). */
    uint64_t hi = scoreToSortable(max);
    if (!max_ex) {
        uint64_t native = ntohu64(hi);
        native++;
        hi = htonu64(native);
    }
    long end_rank = fbtreeSeekToScore((const char *)&hi, &iter);

    return (end_rank > start_rank) ? (unsigned long)(end_rank - start_rank) : 0;
}

unsigned long orderedIndexCountLexRange(const OrderedIndex *oi, const_sds min, const_sds max, bool min_ex, bool max_ex) {
    fbtreeIndex *fbt = (fbtreeIndex *)oi;
    unsigned long len = fbtreeLength(fbt);
    if (len == 0) return 0;

    if (min == shared.minstring && max == shared.maxstring) return len;
    if (max == shared.minstring || min == shared.maxstring) return 0;

    const_sds first = fbtreePeekMin(fbt);
    if (!first) return 0;
    uint64_t score_prefix;
    memcpy(&score_prefix, first, SCORE_SIZE);

    fbtreeIterator iter;
    fbtreeInitIterator(&iter, fbt);

    /* Count via rank arithmetic: count = end_rank - start_rank. fbtreeSeekToValue
     * returns the rank of the first item whose packed value is >= the bound.
     * Unlike scores, lex elements are variable-length with no "next representable"
     * value, so inclusive/exclusive boundaries are resolved by probing whether the
     * bound itself is present (packed (score,element) values are unique). */

    /* Lower bound: rank of the first in-range item. */
    long start_rank;
    if (min == shared.minstring) {
        start_rank = 0;
    } else {
        sds packed = packLexBound(score_prefix, min);
        start_rank = fbtreeSeekToValue(packed, &iter);
        if (min_ex) {
            /* Exclusive min: skip the bound itself if present. */
            const_sds at = fbtreeGetAtRank(fbt, (unsigned long)start_rank);
            if (at && sdscmp(at, packed) == 0) start_rank++;
        }
        sdsfree(packed);
    }

    /* Upper bound: rank of the first item past the range. */
    long end_rank;
    if (max == shared.maxstring) {
        end_rank = (long)len;
    } else {
        sds packed = packLexBound(score_prefix, max);
        end_rank = fbtreeSeekToValue(packed, &iter);
        if (!max_ex) {
            /* Inclusive max: include the bound itself if present. */
            const_sds at = fbtreeGetAtRank(fbt, (unsigned long)end_rank);
            if (at && sdscmp(at, packed) == 0) end_rank++;
        }
        sdsfree(packed);
    }

    return (end_rank > start_rank) ? (unsigned long)(end_rank - start_rank) : 0;
}

/* ==========================================================================
 * Range Seek Helpers
 * ========================================================================== */

/* Unified seek helper: position iterator at a range boundary.
 *
 * After return, the iterator is positioned such that:
 *   - Forward (reverse=0): fbtreeNext() returns the first in-range element
 *   - Reverse (reverse=1): fbtreePrev() returns the last in-range element
 *
 * 'packed' is the [score][element] boundary value to seek to.
 * 'inclusive' means the boundary element itself is in-range. */
static void seekForBound(fbtreeIterator *fbt_iter, sds packed, int reverse, int inclusive) {
    fbtreeSeekToValue(packed, fbt_iter);

    if (!reverse && !inclusive) {
        /* Forward + exclusive: if positioned at exact match, advance past it. */
        const_sds pos;
        if (fbtreeNext(fbt_iter, &pos)) {
            if (sdscmp(pos, packed) != 0) {
                /* First element > bound -- re-seek so next() returns it. */
                fbtreeSeekToValue(pos, fbt_iter);
            }
            /* Else: was exact match, consumed it. next() returns next element. */
        }
    } else if (reverse && inclusive) {
        /* Reverse + inclusive: seek is at first >= bound.
         * If bound exists, advance past so prev() returns bound.
         * If not, re-seek to first > bound so prev() returns last < bound. */
        const_sds pos;
        if (fbtreeNext(fbt_iter, &pos)) {
            if (sdscmp(pos, packed) != 0) {
                /* Not exact match -- re-seek so prev() returns last < bound */
                fbtreeSeekToValue(pos, fbt_iter);
            }
            /* Else: exact match consumed, prev() now returns bound. */
        }
    }
    /* Forward + inclusive: seek already at first >= bound. next() returns it. */
    /* Reverse + exclusive: seek at first >= bound. prev() returns last < bound. */
}

/* Skip N elements in the given direction. */
static void skipElements(fbtreeIterator *fbt_iter, long count, int reverse) {
    const_sds pos;
    for (long i = 0; i < count; i++) {
        if (reverse) {
            if (!fbtreePrev(fbt_iter, &pos)) return;
        } else {
            if (!fbtreeNext(fbt_iter, &pos)) return;
        }
    }
}

/* Pack a lex element with a score prefix for seeking. */
static sds packLexBound(uint64_t score_prefix, const_sds element) {
    size_t ele_len = sdslen(element);
    sds packed = sdsnewlen(NULL, SCORE_SIZE + ele_len);
    memcpy(packed, &score_prefix, SCORE_SIZE);
    memcpy(packed + SCORE_SIZE, element, ele_len);
    return packed;
}

/* ==========================================================================
 * Iterator
 * ========================================================================== */

void orderedIndexInitIterator(OrderedIndexIterator *iter, const OrderedIndex *oi) {
    fbtreeInitIterator((fbtreeIterator *)iter, (fbtreeIndex *)oi);
}

void orderedIndexResetIterator(OrderedIndexIterator *iter) {
    fbtreeResetIterator((fbtreeIterator *)iter);
}

OrderedIndexItem *orderedIndexNext(OrderedIndexIterator *iter) {
    const_sds pos;
    if (fbtreeNext((fbtreeIterator *)iter, &pos)) {
        return (OrderedIndexItem *)pos;
    }
    return NULL;
}

OrderedIndexItem *orderedIndexPrev(OrderedIndexIterator *iter) {
    const_sds pos;
    if (fbtreePrev((fbtreeIterator *)iter, &pos)) {
        return (OrderedIndexItem *)pos;
    }
    return NULL;
}

void orderedIndexSeekToIndex(OrderedIndexIterator *iter, unsigned long index) {
    fbtreeSeekToRank((fbtreeIterator *)iter, index);
}

void orderedIndexSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, bool min_ex, bool max_ex, long offset) {
    fbtreeIterator *fbt_iter = (fbtreeIterator *)iter;
    fbtreeIndex *fbt = fbtreeIteratorGetIndex(fbt_iter);
    if (!fbt) return;

    if (min > max || (min == max && (min_ex || max_ex))) {
        fbtreeResetIterator(fbt_iter);
        return;
    }

    uint64_t sortable;
    if (offset >= 0) {
        sortable = scoreToSortable(min);
        if (min_ex) {
            /* Next representable score (see orderedIndexCountScoreRange). */
            uint64_t native = ntohu64(sortable);
            native++;
            sortable = htonu64(native);
        }
    } else {
        sortable = scoreToSortable(max);
        if (!max_ex) {
            /* Next representable score — seek past max so prev() returns it. */
            uint64_t native = ntohu64(sortable);
            native++;
            sortable = htonu64(native);
        }
    }
    unsigned long len = fbtreeLength(fbt);
    long base = fbtreeSeekToScore((const char *)&sortable, fbt_iter);
    long target = offset + base;

    if (target < 0 || (unsigned long)target >= len) {
        fbtreeResetIterator(fbt_iter);
        return;
    }

    /* Validate the element at target is within [min, max]. */
    const_sds item = fbtreeGetAtRank(fbt, (unsigned long)target);
    if (item) {
        double score = unpackScore(item);
        if (score > max || (max_ex && score == max) ||
            score < min || (min_ex && score == min)) {
            fbtreeResetIterator(fbt_iter);
            return;
        }
    }

    /* Position cursor: next() returns target for forward, prev() for reverse. */
    fbtreeSeekToRank(fbt_iter, (unsigned long)target + (offset < 0 ? 1 : 0));
}

void orderedIndexSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, bool min_ex, bool max_ex, long offset) {
    fbtreeIterator *fbt_iter = (fbtreeIterator *)iter;
    fbtreeIndex *fbt = fbtreeIteratorGetIndex(fbt_iter);
    if (!fbt || fbtreeLength(fbt) == 0) return;

    /* Get score prefix from first element (all share same score in lex zsets) */
    const_sds first = fbtreePeekMin(fbt);
    if (!first) return;
    uint64_t score_prefix;
    memcpy(&score_prefix, first, SCORE_SIZE);

    unsigned long len = fbtreeLength(fbt);
    int reverse = (offset < 0);

    if (!reverse) {
        /* Forward: seek to min bound */
        if (min == shared.minstring) {
            fbtreeSeekToRank(fbt_iter, 0);
        } else {
            sds packed = packLexBound(score_prefix, min);
            seekForBound(fbt_iter, packed, 0, !min_ex);
            sdsfree(packed);
        }
        skipElements(fbt_iter, offset, 0);
    } else {
        /* Reverse: seek to max bound */
        if (max == shared.maxstring) {
            fbtreeSeekToRank(fbt_iter, len);
        } else {
            sds packed = packLexBound(score_prefix, max);
            seekForBound(fbt_iter, packed, 1, !max_ex);
            sdsfree(packed);
        }
        skipElements(fbt_iter, -(offset + 1), 1);
    }
}

/* ==========================================================================
 * Memory
 * ========================================================================== */

void orderedIndexDismissMemory(OrderedIndex *oi) {
    fbtreeDismissMemory((fbtreeIndex *)oi);
}

size_t orderedIndexEstimateMemory(const OrderedIndex *oi, size_t sample_size) {
    UNUSED(sample_size);
    unsigned long len = fbtreeLength((fbtreeIndex *)oi);
    /* Rough estimate: 64 bytes per item (sds + node slot overhead) */
    return len * 64;
}

/* ==========================================================================
 * Defrag
 * ========================================================================== */

OrderedIndex *orderedIndexDefragInternals(OrderedIndex *oi, void *(*defragfn)(void *)) {
    void *newptr = defragfn(oi);
    return newptr ? (OrderedIndex *)newptr : oi;
}

/* Bridge context: fbtreeDefragScan uses (sds old, sds new, void *ctx) callback,
 * but OrderedIndexDefragCallback uses (OrderedIndexItem *, OrderedIndexItem *, void *).
 * Since OrderedIndexItem* IS sds (both are char*), this is just a type cast. */
typedef struct {
    OrderedIndexDefragCallback callback;
    void *privdata;
} defragScanCtx;

static void defragItemCallback(sds old_item, sds new_item, void *ctx_raw) {
    defragScanCtx *ctx = (defragScanCtx *)ctx_raw;
    ctx->callback((OrderedIndexItem *)old_item, (OrderedIndexItem *)new_item, ctx->privdata);
}

unsigned long orderedIndexScanDefrag(OrderedIndex *oi, unsigned long cursor, OrderedIndexDefragCallback callback, void *privdata, void *(*defragfn)(void *)) {
    defragScanCtx ctx = {callback, privdata};
    return fbtreeDefragScan((fbtreeIndex *)oi, cursor, defragItemCallback, &ctx, defragfn);
}

/* ==========================================================================
 * Debug
 * ========================================================================== */

int orderedIndexGetHeight(const OrderedIndex *oi) {
    UNUSED(oi);
    return 0; /* B+tree doesn't expose height in debug context */
}

int orderedIndexVerifyIntegrity(const OrderedIndex *oi, char *errmsg, size_t errmsg_len) {
    if (fbtreeDebugValidate((fbtreeIndex *)oi, false)) {
        errmsg[0] = '\0';
        return 1;
    }
    snprintf(errmsg, errmsg_len, "fbtree integrity check failed");
    return 0;
}
