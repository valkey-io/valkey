/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ORDERED_INDEX_H
#define ORDERED_INDEX_H

/* OrderedIndex — a secondary data structure providing ordered access to
 * (score, element) pairs.
 *
 * An OrderedIndex stores items ordered primarily by a double-precision score,
 * with lexicographic ordering of the element string as a tiebreaker. It
 * supports O(log N) insertion, deletion, score update, rank lookup, and
 * range queries by score, rank, or lexicographic bounds.
 *
 * IMPORTANT: An OrderedIndex does NOT enforce element uniqueness. It is
 * designed to be used alongside a companion hashtable that provides O(1)
 * membership testing and prevents duplicate insertions. The caller is
 * responsible for checking the hashtable before inserting.
 *
 * The interface is implementation-agnostic. Currently implemented as a skiplist
 * (see ordered_index.c). A B+ tree implementation is planned. */

#include "sds.h"
#include <stddef.h>

/* Opaque types. The concrete definitions are backend-specific. */
typedef struct OrderedIndex OrderedIndex;
typedef struct OrderedIndexItem OrderedIndexItem;
typedef uint64_t OrderedIndexIterator[2];

/* Callback invoked for each item removed during a range-delete operation.
 * The item pointer is valid for the duration of the callback but will be
 * freed by the index immediately after the callback returns. Do NOT free
 * the item or store the pointer beyond the callback's scope. */
typedef void (*OrderedIndexOnDelete)(OrderedIndexItem *item, void *ctx);

/* Callback invoked during defrag when an item is reallocated. Allows the
 * caller to update external references (e.g. hashtable pointers). */
typedef void (*OrderedIndexDefragCallback)(OrderedIndexItem *old_item, OrderedIndexItem *new_item, void *ctx);

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
 * Returns a pointer to the inserted item (for storing in a companion HT).
 * The caller must ensure the element is not already present. */
OrderedIndexItem *orderedIndexInsert(OrderedIndex *oi, double score, const char *ele, size_t len);

/* Remove an item from the index and free it. */
void orderedIndexDelete(OrderedIndex *oi, OrderedIndexItem *item);

/* Update the score of an existing item. May reposition it in the index.
 * Returns the (possibly new) item pointer — the old pointer may be invalid
 * if the item was repositioned. Always returns a valid pointer; callers can
 * compare old vs returned to detect whether the item moved. */
OrderedIndexItem *orderedIndexUpdateScore(OrderedIndex *oi, OrderedIndexItem *item, double newscore);

/* Remove and return the first (lowest-score) item without freeing it. */
OrderedIndexItem *orderedIndexPopFirst(OrderedIndex *oi);

/* Remove and return the last (highest-score) item without freeing it. */
OrderedIndexItem *orderedIndexPopLast(OrderedIndex *oi);

/* Free a detached item (one that is not in any index). */
void orderedIndexFreeItem(OrderedIndexItem *item);

/* Create an item that is not inserted into any index. Used for multi-set
 * operations (e.g. ZUNIONSTORE) where scores are accumulated in a hashtable
 * before final insertion, avoiding repeated delete+reinsert costs. */
OrderedIndexItem *orderedIndexCreateDetached(double score, const char *ele, size_t len);

/* Set the score on a detached item. Do NOT use on items that are currently
 * in an index — that would corrupt sort order. Use orderedIndexUpdateScore
 * for items that are in an index. */
void orderedIndexDetachedSetScore(OrderedIndexItem *item, double score);

/* Insert a previously-detached item into the index. The index takes ownership. */
OrderedIndexItem *orderedIndexInsertDetached(OrderedIndex *oi, OrderedIndexItem *item);

/* Delete all items with score in [min, max]. If min_ex is set, min is exclusive; if max_ex is set, max is exclusive.
 * Calls on_delete for each removed item. Returns count of items removed. */
unsigned long orderedIndexDeleteRangeByScore(OrderedIndex *oi, double min, double max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx);

/* Delete all items with index in [start, end] (0-based, inclusive).
 * Calls on_delete for each removed item. Returns count of items removed. */
unsigned long orderedIndexDeleteRangeByIndex(OrderedIndex *oi, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *ctx);

/* Delete all items with element in lex range [min, max]. If min_ex is set, min is exclusive; if max_ex is set, max is exclusive.
 * Calls on_delete for each removed item. Returns count of items removed. */
unsigned long orderedIndexDeleteRangeByLex(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx);

/* ============================================================
 * Query
 * ============================================================ */

/* Return the number of items in the index. */
unsigned long orderedIndexLength(OrderedIndex *oi);

/* Return the item at the given 0-based index, or NULL if out of range. */
OrderedIndexItem *orderedIndexGetByIndex(OrderedIndex *oi, unsigned long index);

/* Return the first (lowest) item, or NULL if empty. O(1). */
OrderedIndexItem *orderedIndexGetFirst(OrderedIndex *oi);

/* Return the last (highest) item, or NULL if empty. O(1). */
OrderedIndexItem *orderedIndexGetLast(OrderedIndex *oi);

/* Return the 0-based index of an item. The item must be in the index. */
unsigned long orderedIndexGetIndex(OrderedIndex *oi, const OrderedIndexItem *item);

/* Get the element data from an item as a raw pointer + length. */
void orderedIndexGetElementRaw(const OrderedIndexItem *item, const char **ptr, size_t *len);

/* Get the score of an item. */
double orderedIndexGetScore(const OrderedIndexItem *item);

/* Count items with score in [min, max]. If min_ex is set, min is exclusive; if max_ex is set, max is exclusive. */
unsigned long orderedIndexCountScoreRange(OrderedIndex *oi, double min, double max, int min_ex, int max_ex);

/* Count items with element in lex range [min, max]. If min_ex is set, min is exclusive; if max_ex is set, max is exclusive. */
unsigned long orderedIndexCountLexRange(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex);

/* ============================================================
 * Iterator
 * ============================================================ */

/* Initialize a stack-allocated iterator. If no seek function is called,
 * next() starts from the beginning and prev() starts from the end.
 * Use orderedIndexSeekToIndex/ScoreRange/LexRange to start elsewhere. */
void orderedIndexInitIterator(OrderedIndexIterator *iter, OrderedIndex *oi);

/* Reset iterator position (keeps the index association). */
void orderedIndexResetIterator(OrderedIndexIterator *iter);

/* Advance iterator forward. Returns the next item, or NULL at end. */
OrderedIndexItem *orderedIndexNext(OrderedIndexIterator *iter);

/* Advance iterator backward. Returns the previous item, or NULL at start. */
OrderedIndexItem *orderedIndexPrev(OrderedIndexIterator *iter);

/* Position iterator at the given 0-based index. next() returns index+1, prev() returns index. */
void orderedIndexSeekToIndex(OrderedIndexIterator *iter, unsigned long index);

/* Position iterator within a score range.
 * offset >= 0: next() returns the (offset)th element in range.
 * offset < 0:  prev() returns the (-offset-1)th element from end of range. */
void orderedIndexSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset);

/* Position iterator within a lex range. Offset semantics same as score range. */
void orderedIndexSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset);

/* ============================================================
 * Memory
 * ============================================================ */

/* Hint to the OS that the index memory can be reclaimed (e.g. via madvise). */
void orderedIndexDismissMemory(OrderedIndex *oi);

/* Estimate total memory usage by averaging the specified number of sample elements. */
size_t orderedIndexEstimateMemory(OrderedIndex *oi, size_t sample_size);

/* Defrag data structure internals. Returns new pointer if reallocated. */
OrderedIndex *orderedIndexDefragInternals(OrderedIndex *oi, void *(*defragfn)(void *));

/* Incremental defrag scan. Walks items in batches, calling defragfn on each.
 * When an item is reallocated, callback is invoked to update external refs.
 * Returns next cursor, or 0 when complete. */
unsigned long orderedIndexScanDefrag(OrderedIndex *oi, unsigned long cursor, OrderedIndexDefragCallback callback, void *ctx, void *(*defragfn)(void *));

/* ============================================================
 * Debug / Verification
 * ============================================================ */

/* Return the internal height of the data structure (skiplist levels). */
int orderedIndexGetHeight(OrderedIndex *oi);

/* Verify structural integrity. Returns 1 if valid, 0 if corrupt.
 * On failure, a description is written to errmsg. */
int orderedIndexVerifyIntegrity(OrderedIndex *oi, char *errmsg, size_t errmsg_len);

#endif /* ORDERED_INDEX_H */
