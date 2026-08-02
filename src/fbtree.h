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
size_t fbtreeEstimateStructureMemory(fbtreeIndex *fbt);
unsigned long fbtreeHeight(const fbtreeIndex *fbt);
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
/* Validate tree invariants. Returns true when consistent. On failure, when
 * errmsg is non-NULL, a message describing the first failed check is
 * written into it (truncated to errmsg_len). */
bool fbtreeDebugValidate(fbtreeIndex *fbt, bool verbose, char *errmsg, size_t errmsg_len);

/* Incremental defrag scan over the tree's rank order. cursor=0 to start,
 * returns the next cursor (0 when the sweep is done). Relocates one leaf per
 * call along with its items via defragfn, patching parent links, the leaf
 * chain, caches, and ancestor anchors. Inner nodes are relocated by the call
 * that visits their leftmost descendant leaf — at most tree-depth nodes per
 * call, each exactly once per sweep — so per-call work stays bounded. When an
 * item is reallocated, item_callback is invoked with the old and new
 * pointers. */
unsigned long fbtreeDefragScan(fbtreeIndex *fbt, unsigned long cursor, void (*item_callback)(sds old_item, sds new_item, void *ctx), void *ctx, void *(*defragfn)(void *));

/* Hint to the OS (madvise DONTNEED) that the tree's memory can be reclaimed:
 * walks every leaf with its items and every inner node with any spilled
 * prefix block. Call only when this process will not read the tree again --
 * reclaimed anonymous pages are zero-filled on any later access. */
void fbtreeDismissMemory(fbtreeIndex *fbt);

#endif /* FBTREE_H */
