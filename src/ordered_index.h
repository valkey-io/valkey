/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ORDERED_INDEX_H
#define ORDERED_INDEX_H

/* OrderedIndex  -- a secondary data structure providing ordered access to
 * (score, element) pairs.
 *
 * An OrderedIndex stores items ordered primarily by a double-precision score,
 * with lexicographic ordering of the element string as a tiebreaker. It
 * supports O(log N) insertion, deletion, score update, rank lookup, and
 * range queries by score, rank, or lexicographic bounds.
 *
 * IMPORTANT: An OrderedIndex does NOT enforce element uniqueness --
 * duplicate (score, element) pairs are stored as separate items. It is
 * designed to be used alongside a companion hashtable that provides O(1)
 * membership testing when uniqueness is required (as in Valkey's ZSET).
 *
 * The interface is implementation-agnostic. It is currently implemented as a
 * feature B+ tree (fbtree); see ordered_index.c. */

#include "sds.h"
#include <stdbool.h>
#include <stddef.h>

/* Opaque types. The concrete definitions are backend-specific. */
typedef struct OrderedIndex OrderedIndex;
typedef struct OrderedIndexItem OrderedIndexItem;
typedef uint64_t OrderedIndexIterator[3];

/* Callback invoked for each item removed during a range-delete operation.
 * The item pointer is valid for the duration of the callback but will be
 * freed by the index immediately after the callback returns. Do NOT free
 * the item or store the pointer beyond the callback's scope. */
typedef void (*OrderedIndexOnDelete)(const OrderedIndexItem *item, void *privdata);

/* Callback invoked during defrag when an item is reallocated. Allows the
 * caller to update external references (e.g. hashtable pointers). */
typedef void (*OrderedIndexDefragCallback)(OrderedIndexItem *old_item, OrderedIndexItem *new_item, void *privdata);

/* ============================================================
 * Lifecycle
 * ============================================================ */

/* Create a new empty ordered index. */
OrderedIndex *orderedIndexCreate(void);

/* Free an ordered index and all items it contains. */
void orderedIndexFree(OrderedIndex *oi);

/* ============================================================
 * Modification
 * ============================================================ */

/* Insert a new item with the given score and element (copied).
 * Returns a pointer to the inserted item.
 * Duplicates are allowed -- inserting the same (score, element) pair multiple
 * times results in multiple distinct items stored adjacently. Callers that
 * require uniqueness (e.g. ZSET) enforce it externally via a companion
 * hashtable. */
OrderedIndexItem *orderedIndexInsert(OrderedIndex *oi, double score, const char *ele, size_t len);

/* Remove an item from the index and free it. */
void orderedIndexDelete(OrderedIndex *oi, OrderedIndexItem *item);

/* Update the score of an existing item. May reposition it in the index.
 * Returns the (possibly new) item pointer  -- the old pointer may be invalid
 * if the item was repositioned. Always returns a valid pointer; callers can
 * compare old vs returned to detect whether the item moved. */
OrderedIndexItem *orderedIndexUpdateScore(OrderedIndex *oi, OrderedIndexItem *item, double newscore);

/* Remove and return the first (lowest-score) item without freeing it. */
OrderedIndexItem *orderedIndexPopFirst(OrderedIndex *oi);

/* Remove and return the last (highest-score) item without freeing it. */
OrderedIndexItem *orderedIndexPopLast(OrderedIndex *oi);

/* Free a detached item (one that is not in any index). Typically used after
 * orderedIndexPopFirst/PopLast which return items removed from the index. */
void orderedIndexItemFree(OrderedIndexItem *item);

/* Create an item not yet inserted into any index.
 *
 * Used for batch-insert workflows (e.g. ZUNIONSTORE): create items, store
 * them in a hashtable keyed by element, update scores with ItemSetScore as
 * duplicates are encountered, then bulk-insert all items at the end via
 * orderedIndexInsertItem. This avoids O(log N) repositioning per score
 * update during the aggregation phase. */
OrderedIndexItem *orderedIndexItemCreate(double score, const char *ele, size_t len);

/* Set the score on an item that is NOT in any index. This is O(1) because
 * no repositioning is needed -- the item has no position yet.
 *
 * Do NOT use on items currently in an index; that would silently corrupt
 * sort order. Use orderedIndexUpdateScore for in-index items (which handles
 * repositioning). */
void orderedIndexItemSetScore(OrderedIndexItem *item, double score);

/* Insert a pre-created item into the index (the final step of the
 * batch-insert workflow). The index takes ownership of the item.
 * Returns the item pointer (same as input -- for API consistency with
 * orderedIndexInsert so callers can use either path uniformly). */
OrderedIndexItem *orderedIndexInsertItem(OrderedIndex *oi, OrderedIndexItem *item);

/* Delete all items with score in [min, max] (or exclusive if min_ex/max_ex).
 * For each removed item, on_delete is called (if non-NULL) with the item and
 * privdata before the item is freed. Returns count of items removed. */
unsigned long orderedIndexDeleteRangeByScore(OrderedIndex *oi, double min, double max, bool min_ex, bool max_ex, OrderedIndexOnDelete on_delete, void *privdata);

/* Delete all items with rank in [start, end] (0-based, inclusive).
 * For each removed item, on_delete is called (if non-NULL) with the item and
 * privdata before the item is freed. Returns count of items removed. */
unsigned long orderedIndexDeleteRangeByIndex(OrderedIndex *oi, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *privdata);

/* Delete all items with element in lex range [min, max] (or exclusive if
 * min_ex/max_ex). Only meaningful when all items share the same score (as in
 * ZRANGEBYLEX). For each removed item, on_delete is called (if non-NULL) with
 * the item and privdata before the item is freed. Returns count of items removed. */
unsigned long orderedIndexDeleteRangeByLex(OrderedIndex *oi, const_sds min, const_sds max, bool min_ex, bool max_ex, OrderedIndexOnDelete on_delete, void *privdata);

/* ============================================================
 * Query
 * ============================================================ */

/* Return the number of items in the index. */
unsigned long orderedIndexLength(const OrderedIndex *oi);

/* Return the item at the given 0-based index, or NULL if out of range. */
OrderedIndexItem *orderedIndexGetByIndex(const OrderedIndex *oi, unsigned long index);

/* Return the first (lowest) item, or NULL if empty. O(1). */
OrderedIndexItem *orderedIndexGetFirst(const OrderedIndex *oi);

/* Return the last (highest) item, or NULL if empty. O(1). */
OrderedIndexItem *orderedIndexGetLast(const OrderedIndex *oi);

/* Return the 0-based rank of an item. The item must be in the index;
 * behavior is undefined otherwise. */
unsigned long orderedIndexGetIndex(const OrderedIndex *oi, const OrderedIndexItem *item);

/* Get the element data from an item as a raw pointer + length. */
void orderedIndexItemGetElement(const OrderedIndexItem *item, const char **ptr, size_t *len);

/* Get the score of an item. */
double orderedIndexItemGetScore(const OrderedIndexItem *item);

/* Count items with score in [min, max] (or exclusive if min_ex/max_ex). */
unsigned long orderedIndexCountScoreRange(const OrderedIndex *oi, double min, double max, bool min_ex, bool max_ex);

/* Count items with element in lex range [min, max] (or exclusive if
 * min_ex/max_ex). Only meaningful when all items share the same score. */
unsigned long orderedIndexCountLexRange(const OrderedIndex *oi, const_sds min, const_sds max, bool min_ex, bool max_ex);

/* ============================================================
 * Iterator
 *
 * The iterator uses a cursor-between-items model (Java ListIterator
 * semantics). The cursor is positioned between elements, not on them:
 *
 *   Elements:  [A] [B] [C] [D] [E]
 *   Cursor:   ^   ^   ^   ^   ^   ^
 *   Position: 0   1   2   3   4   5
 *
 * next() returns the element to the right of the cursor and advances it.
 * prev() returns the element to the left of the cursor and moves it back.
 * ============================================================ */

/* Initialize a stack-allocated iterator. If no seek function is called,
 * next() starts from the beginning and prev() starts from the end.
 * Use orderedIndexSeekToIndex/ScoreRange/LexRange to start elsewhere. */
void orderedIndexInitIterator(OrderedIndexIterator *iter, const OrderedIndex *oi);

/* Reset iterator to the initial unseeked state: next() will return the first
 * item and prev() will return the last item. Keeps the index association. */
void orderedIndexResetIterator(OrderedIndexIterator *iter);

/* Advance iterator forward. Returns the next item, or NULL at end. */
OrderedIndexItem *orderedIndexNext(OrderedIndexIterator *iter);

/* Advance iterator backward. Returns the previous item, or NULL at start. */
OrderedIndexItem *orderedIndexPrev(OrderedIndexIterator *iter);

/* Position the cursor before rank N (Java ListIterator semantics). After
 * seeking:
 * - next() returns the item at rank N (or NULL if N >= length).
 * - prev() returns the item at rank N-1 (or NULL if N == 0). */
void orderedIndexSeekToIndex(OrderedIndexIterator *iter, unsigned long index);

/* Position the cursor within a score range. The offset selects which item
 * in the range the cursor is placed before/after:
 * - offset >= 0: cursor is before the (offset)th item in range; next()
 *   returns that item. If offset exceeds the range, next() returns NULL.
 * - offset < 0: cursor is after the (-offset-1)th item from the end of
 *   range; prev() returns that item. If offset exceeds the range, prev()
 *   returns NULL. */
void orderedIndexSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, bool min_ex, bool max_ex, long offset);

/* Position the cursor within a lex range. Offset semantics same as score
 * range. Only meaningful when all items in the range share the same score. */
void orderedIndexSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, bool min_ex, bool max_ex, long offset);

/* ============================================================
 * Memory
 * ============================================================ */

/* Hint to the OS that the index's memory pages can be reclaimed (madvise
 * DONTNEED). Call only when the calling process will not read the index
 * again -- e.g. in the fork child after the value has been serialized, to
 * avoid needless copy-on-write. Reclaimed anonymous pages are zero-filled
 * on any later access. */
void orderedIndexDismissMemory(OrderedIndex *oi);

/* Estimate the memory used by the index structure itself. Item payloads are
 * owned jointly with the caller (e.g. a companion hashtable) and are not
 * included; callers account for them by sampling the items they hold. */
size_t orderedIndexEstimateStructureMemory(const OrderedIndex *oi);

/* Defrag the index's own top-level struct. All node and item allocations are
 * handled incrementally by orderedIndexScanDefrag. Returns the index pointer,
 * updated if the struct was relocated. */
OrderedIndex *orderedIndexDefragInternals(OrderedIndex *oi, void *(*defragfn)(void *));

/* Incremental defrag scan over rank order, one leaf per call. Relocates the
 * leaf and its items via defragfn; when an item is relocated, callback is
 * invoked to update external refs. Inner nodes are relocated by the call that
 * visits their leftmost descendant leaf, bounding per-call work to at most
 * tree-depth node relocations. Returns the next cursor, or 0 when complete. */
unsigned long orderedIndexScanDefrag(OrderedIndex *oi, unsigned long cursor, OrderedIndexDefragCallback callback, void *privdata, void *(*defragfn)(void *));

/* ============================================================
 * Debug / Verification
 * ============================================================ */

/* Return the internal height of the data structure (tree levels). */
int orderedIndexGetHeight(const OrderedIndex *oi);

/* Verify structural integrity. Returns 1 if valid, 0 if corrupt.
 * On failure, a description is written to errmsg. */
int orderedIndexVerifyIntegrity(const OrderedIndex *oi, char *errmsg, size_t errmsg_len);

#endif /* ORDERED_INDEX_H */
