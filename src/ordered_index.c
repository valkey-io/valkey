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
    /* Collapse IEEE negative zero into positive zero: the two compare equal
     * numerically but differ in bit pattern, and equal scores must map to a
     * single tree key for score-range comparisons to match. */
    if (score == 0.0) score = 0.0;
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

    bool max_ex_eff = max_ex;
    if (max == shared.maxstring) {
        /* No single-byte suffix can bound its own continuations under lex
         * order (a strict prefix sorts before them), so bound the range with
         * the next score bucket's prefix, exclusive. Valid scores never map
         * to an all-ones sortable (that bit pattern is a NaN), so the
         * increment cannot wrap. */
        uint64_t native = ntohu64(score_prefix);
        native++;
        uint64_t next_prefix = htonu64(native);
        max_packed = sdsnewlen(NULL, SCORE_SIZE);
        memcpy(max_packed, &next_prefix, SCORE_SIZE);
        max_ex_eff = true;
    } else {
        size_t max_len = sdslen(max);
        max_packed = sdsnewlen(NULL, SCORE_SIZE + max_len);
        memcpy(max_packed, &score_prefix, SCORE_SIZE);
        memcpy(max_packed + SCORE_SIZE, max, max_len);
    }

    rangeDeleteArgs args = {on_delete, privdata};
    unsigned long deleted = fbtreeDeleteRangeByValue(fbt, min_packed, max_packed, min_ex, max_ex_eff, rangeDeleteCallback, &args);
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

    /* Convert the double bounds to the big-endian sortable score prefix used as
     * the tree key, then count in one shared descent. Boundary inclusivity is
     * resolved at the leaf by fbtreeCountRangeByScore (min_ex/max_ex), so no
     * next-representable-score nudging is needed here. */
    uint64_t lo = scoreToSortable(min);
    uint64_t hi = scoreToSortable(max);
    return fbtreeCountRangeByScore(fbt, (const char *)&lo, (const char *)&hi, min_ex, max_ex);
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

    /* Pack the bounds into [score][element] keys, then count in one shared
     * descent. The minstring sentinel packs to the bare score prefix, which
     * sorts before every real element; the maxstring sentinel is bounded by
     * the next score bucket's prefix, exclusive. */
    sds min_packed, max_packed;
    bool max_ex_eff = max_ex;
    if (min == shared.minstring) {
        min_packed = sdsnewlen(NULL, SCORE_SIZE);
        memcpy(min_packed, &score_prefix, SCORE_SIZE);
    } else {
        min_packed = packLexBound(score_prefix, min);
    }
    if (max == shared.maxstring) {
        /* See orderedIndexDeleteRangeByLex: a byte suffix cannot bound its
         * own continuations; use the next score prefix, exclusive. */
        uint64_t native = ntohu64(score_prefix);
        native++;
        uint64_t next_prefix = htonu64(native);
        max_packed = sdsnewlen(NULL, SCORE_SIZE);
        memcpy(max_packed, &next_prefix, SCORE_SIZE);
        max_ex_eff = true;
    } else {
        max_packed = packLexBound(score_prefix, max);
    }

    unsigned long count = fbtreeCountRangeByValue(fbt, min_packed, max_packed, min_ex, max_ex_eff);

    sdsfree(min_packed);
    sdsfree(max_packed);
    return count;
}

/* ==========================================================================
 * Range Seek Helpers
 * ========================================================================== */

/* Unified seek helper: position the iterator at a range boundary so the first
 * subsequent step in the iteration direction yields the first in-range element.
 *
 *   - Forward (reverse=0): after return, fbtreeNext() yields the first element
 *     that is in range (>= min, honoring inclusive/exclusive).
 *   - Reverse (reverse=1): after return, fbtreePrev() yields the last element
 *     that is in range (<= max, honoring inclusive/exclusive).
 *
 * 'packed' is the [score][element] boundary to seek to; 'inclusive' means the
 * boundary element itself is in range.
 *
 * fbtreeSeekToValue lands the cursor on the first element >= packed, i.e. the
 * element fbtreeNext() would return. Two of the four (direction, inclusivity)
 * cases need a one-element nudge from there; the other two are already correct.
 * The nudge peeks the boundary element with fbtreeNext, then either keeps it
 * consumed (exact match) or steps back O(1) with fbtreePrev (no re-seek). */
static void seekForBound(fbtreeIterator *fbt_iter, sds packed, int reverse, int inclusive) {
    fbtreeSeekToValue(packed, fbt_iter);

    if (!reverse && !inclusive) {
        /* Forward + exclusive: peek the first element >= bound. If it IS the
         * bound, leave it consumed so fbtreeNext() returns the element after it
         * (the bound is excluded). If it is already past the bound, step back so
         * fbtreeNext() returns it. */
        const_sds pos = fbtreeNext(fbt_iter);
        if (pos != NULL && sdscmp(pos, packed) != 0) fbtreePrev(fbt_iter);
    } else if (reverse && inclusive) {
        /* Reverse + inclusive: peek the first element >= bound. If it IS the
         * bound, leave it consumed so fbtreePrev() returns the bound. If it is
         * past the bound (bound absent), step back so fbtreePrev() returns the
         * last element < bound. */
        const_sds pos = fbtreeNext(fbt_iter);
        if (pos != NULL && sdscmp(pos, packed) != 0) fbtreePrev(fbt_iter);
    }
    /* Forward + inclusive: cursor already on the first element >= bound, which is
     * exactly what fbtreeNext() should return. No adjustment needed. */
    /* Reverse + exclusive: cursor on the first element >= bound, so fbtreePrev()
     * returns the element just before it -- the last element < bound. No adjustment. */
}

/* Skip N elements in the given direction. */
static void skipElements(fbtreeIterator *fbt_iter, long count, int reverse) {
    for (long i = 0; i < count; i++) {
        if (reverse) {
            if (fbtreePrev(fbt_iter) == NULL) return;
        } else {
            if (fbtreeNext(fbt_iter) == NULL) return;
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
    return (OrderedIndexItem *)fbtreeNext((fbtreeIterator *)iter);
}

OrderedIndexItem *orderedIndexPrev(OrderedIndexIterator *iter) {
    return (OrderedIndexItem *)fbtreePrev((fbtreeIterator *)iter);
}

void orderedIndexSeekToIndex(OrderedIndexIterator *iter, unsigned long index) {
    fbtreeSeekToRank((fbtreeIterator *)iter, index);
}

/* Position the iterator to begin a score-range scan.
 *
 * 'offset' encodes both direction and how many in-range elements to skip:
 *   - offset >= 0: forward. Seek to the min bound, skip 'offset' elements, then
 *     the caller steps with orderedIndexNext().
 *   - offset <  0: reverse. Seek to the max bound, skip (-offset - 1) elements
 *     downward, then the caller steps with orderedIndexPrev(). Thus offset == -1
 *     starts at the max element, -2 at the one below it, and so on. (Callers map
 *     a reverse LIMIT offset N to -N - 1.)
 * If the resulting position is outside [min, max] or the index bounds, the
 * iterator is reset (empty result). */
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
            /* Next representable score: scoreToSortable is order-preserving, so
             * +1 in native byte order is nextafter(score, +inf). */
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

/* Position the iterator to begin a lex-range scan. 'offset' follows the same
 * convention as orderedIndexSeekToScoreRange: offset >= 0 iterates forward from
 * the min bound (skipping 'offset' elements), offset < 0 iterates in reverse
 * from the max bound (skipping -offset - 1 elements down, so -1 starts at max). */
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

size_t orderedIndexEstimateStructureMemory(const OrderedIndex *oi) {
    return fbtreeEstimateStructureMemory((fbtreeIndex *)oi);
}

/* ==========================================================================
 * Defrag
 * ========================================================================== */

OrderedIndex *orderedIndexDefragInternals(OrderedIndex *oi, void *(*defragfn)(void *)) {
    fbtreeIndex *fbt = (fbtreeIndex *)oi;
    void *newptr = defragfn(fbt);
    if (newptr) fbt = (fbtreeIndex *)newptr;
    return (OrderedIndex *)fbt;
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
    return (int)fbtreeHeight((const fbtreeIndex *)oi);
}

int orderedIndexVerifyIntegrity(const OrderedIndex *oi, char *errmsg, size_t errmsg_len) {
    return fbtreeDebugValidate((fbtreeIndex *)oi, false, errmsg, errmsg_len) ? 1 : 0;
}
