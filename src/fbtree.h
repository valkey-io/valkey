#ifndef FBTREE_H
#define FBTREE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* Forward declare sds types to avoid C++ issues with sds.h macros.
 * C code will have sds.h included elsewhere; C++ benchmarks get the typedef here. */
typedef char *sds;
typedef const char *const_sds;

typedef struct fbtreeIndex fbtreeIndex;

/* Opaque iterator type that can be stack allocated */
typedef uint64_t fbtreeIterator[3];

/* Internal API for testing */
fbtreeIndex *fbtreeCreate(void);
sds fbtreeInsert(fbtreeIndex *fbt, sds string);
bool fbtreeDelete(fbtreeIndex *fbt, const_sds key);
sds fbtreePopMin(fbtreeIndex *fbt);
sds fbtreePopMax(fbtreeIndex *fbt);
const_sds fbtreePeekMin(fbtreeIndex *fbt);
const_sds fbtreePeekMax(fbtreeIndex *fbt);
void fbtreeEmpty(fbtreeIndex *fbt);
void fbtreeFree(fbtreeIndex *fbt);
unsigned long fbtreeLength(fbtreeIndex *fbt);
void fbtreeInitIterator(fbtreeIterator *iterator, fbtreeIndex *fbt);
void fbtreeResetIterator(fbtreeIterator *iterator);
fbtreeIndex *fbtreeIteratorGetIndex(fbtreeIterator *iterator);
/* Advance/retreat the iterator one position, returning the element at the new
 * position, or NULL when there are no more elements in that direction. */
const_sds fbtreeNext(fbtreeIterator *iterator);
const_sds fbtreePrev(fbtreeIterator *iterator);

void fbtreeSeekToRank(fbtreeIterator *iterator, unsigned long rank);
const_sds fbtreeGetAtRank(fbtreeIndex *fbt, unsigned long rank);
long fbtreeGetIndexOfItem(fbtreeIndex *fbt, const_sds item);

/* Score seek - positions iterator at first element with score >= given score.
 * Always positions the iterator (even if no exact match). Use fbtreeNext to get elements.
 * If all elements have score < given score, iterator is positioned past end.
 * Returns the rank (0-indexed) of the position. If positioned past end,
 * returns the tree length (one past the last valid rank). Returns 0 for
 * an empty tree. */
long fbtreeSeekToScore(const char *score, fbtreeIterator *iterator);

/* Value seek - positions iterator at first element with value >= given value.
 * Uses full sds comparison (not just score prefix).
 * Always positions the iterator (even if no exact match). Use fbtreeNext to get elements.
 * If all elements have value < given value, iterator is positioned past end.
 * Returns the rank (0-indexed) of the position. If positioned past end,
 * returns the tree length (one past the last valid rank). Returns 0 for
 * an empty tree. */
long fbtreeSeekToValue(const_sds value, fbtreeIterator *iterator);

/* Optional callback invoked for each item being deleted, before sdsfree.
 * Pass NULL for callback/callback_ctx to skip. */

/* Range deletion */
unsigned long fbtreeDeleteRangeByRank(fbtreeIndex *fbt, unsigned long start_rank, unsigned long end_rank, void (*callback)(sds item, void *ctx), void *callback_ctx);
unsigned long fbtreeDeleteRangeByScore(fbtreeIndex *fbt, const char *min_score, const char *max_score, int min_ex, int max_ex, void (*callback)(sds item, void *ctx), void *callback_ctx);
unsigned long fbtreeDeleteRangeByValue(fbtreeIndex *fbt, const_sds min_val, const_sds max_val, int min_ex, int max_ex, void (*callback)(sds item, void *ctx), void *callback_ctx);
unsigned long fbtreeCountRangeByScore(fbtreeIndex *fbt, const char *min_score, const char *max_score, int min_ex, int max_ex);
unsigned long fbtreeCountRangeByValue(fbtreeIndex *fbt, const_sds min_val, const_sds max_val, int min_ex, int max_ex);

/* Debug */
bool fbtreeDebugValidate(fbtreeIndex *fbt, bool verbose);

/* Leaf iteration for defrag/dismiss (walks the linked list of leaves).
 * Callback receives each packed sds item. For defrag, return the new pointer
 * if reallocated, or NULL if unchanged. leaf_callback is for the leaf node
 * itself (same contract). */
typedef sds (*fbtreeItemDefragFn)(sds item, void *ctx);
typedef struct leafNode *(*fbtreeLeafDefragFn)(struct leafNode *leaf, void *ctx);

/* Incremental defrag scan. cursor=0 to start, returns 0 when done.
 * Processes up to 16 items per call. When an item is reallocated,
 * item_callback is called with old/new pointers. */
unsigned long fbtreeDefragScan(fbtreeIndex *fbt, unsigned long cursor, void (*item_callback)(sds old_item, sds new_item, void *ctx), void *ctx, void *(*defragfn)(void *));

/* Walk all leaf nodes and call dismissMemory on each. */
void fbtreeDismissMemory(fbtreeIndex *fbt);

#endif /* FBTREE_H */
