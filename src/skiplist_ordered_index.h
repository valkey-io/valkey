/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SKIPLIST_ORDERED_INDEX_H
#define SKIPLIST_ORDERED_INDEX_H

/* Skiplist backend for the OrderedIndex interface.
 *
 * This file declares the skiplist-specific implementations of all OrderedIndex
 * operations. These are called by ordered_index.c (the dispatch layer) and
 * should not be called directly by application code.
 *
 * The skiplist stores (score, element) pairs in a probabilistic balanced
 * structure providing O(log N) operations with good cache behavior. */

#include "ordered_index.h"

/* Lifecycle */
OrderedIndex *skiplistCreate(void);
void skiplistFree(OrderedIndex *oi);

/* Modification */
OrderedIndexItem *skiplistInsert(OrderedIndex *oi, double score, const char *ele, size_t len);
void skiplistDelete(OrderedIndex *oi, OrderedIndexItem *item);
OrderedIndexItem *skiplistUpdateScore(OrderedIndex *oi, OrderedIndexItem *item, double newscore);
OrderedIndexItem *skiplistPopFirst(OrderedIndex *oi);
OrderedIndexItem *skiplistPopLast(OrderedIndex *oi);
void skiplistFreeItem(OrderedIndexItem *item);
OrderedIndexItem *skiplistCreateDetached(double score, const char *ele, size_t len);
void skiplistDetachedSetScore(OrderedIndexItem *item, double score);
OrderedIndexItem *skiplistInsertDetached(OrderedIndex *oi, OrderedIndexItem *item);
unsigned long skiplistDeleteRangeByScore(OrderedIndex *oi, double min, double max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx);
unsigned long skiplistDeleteRangeByRank(OrderedIndex *oi, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *ctx);
unsigned long skiplistDeleteRangeByLex(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx);

/* Query */
unsigned long skiplistLength(OrderedIndex *oi);
OrderedIndexItem *skiplistGetByRank(OrderedIndex *oi, unsigned long rank);
unsigned long skiplistGetRank(OrderedIndex *oi, const OrderedIndexItem *item);
void skiplistGetElementRaw(const OrderedIndexItem *item, const char **ptr, size_t *len);
double skiplistGetScore(const OrderedIndexItem *item);
unsigned long skiplistCountScoreRange(OrderedIndex *oi, double min, double max, int min_ex, int max_ex);
unsigned long skiplistCountLexRange(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex);

/* Iterator */
void skiplistInitIterator(OrderedIndexIterator *iter, OrderedIndex *oi);
void skiplistResetIterator(OrderedIndexIterator *iter);
OrderedIndexItem *skiplistNext(OrderedIndexIterator *iter);
OrderedIndexItem *skiplistPrev(OrderedIndexIterator *iter);
void skiplistSeekToRank(OrderedIndexIterator *iter, unsigned long rank);
void skiplistSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset);
void skiplistSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset);

/* Memory */
void skiplistDismissMemory(OrderedIndex *oi);
size_t skiplistEstimateMemory(OrderedIndex *oi, size_t sample_size);

/* Defrag */
OrderedIndex *skiplistDefragInternals(OrderedIndex *oi, void *(*defragfn)(void *));
unsigned long skiplistScanDefrag(OrderedIndex *oi, unsigned long cursor, OrderedIndexDefragCallback callback, void *ctx, void *(*defragfn)(void *));

/* Debug (used by unit tests and DEBUG command, not part of public API) */
int skiplistGetHeight(OrderedIndex *oi);
int skiplistVerifyIntegrity(OrderedIndex *oi, char *errmsg, size_t errmsg_len);

#endif /* SKIPLIST_ORDERED_INDEX_H */
