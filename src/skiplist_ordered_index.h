#ifndef SKIPLIST_ORDERED_INDEX_H
#define SKIPLIST_ORDERED_INDEX_H

#include "sds.h"

typedef struct OrderedIndex OrderedIndex;
typedef struct OrderedIndexItem OrderedIndexItem;
typedef uint64_t OrderedIndexIterator[3];
typedef void (*OrderedIndexOnDelete)(OrderedIndexItem *item, void *ctx);

/* Lifecycle */
OrderedIndex *skiplistCreate(void);
void skiplistFree(OrderedIndex *idx);

/* Modification */
OrderedIndexItem *skiplistInsert(OrderedIndex *idx, double score, const char *ele, size_t len);
void skiplistDelete(OrderedIndex *idx, OrderedIndexItem *node);
OrderedIndexItem *skiplistUpdateScore(OrderedIndex *idx, OrderedIndexItem *node, double newscore);
OrderedIndexItem *skiplistPopFirst(OrderedIndex *idx);
OrderedIndexItem *skiplistPopLast(OrderedIndex *idx);
void skiplistFreeItem(OrderedIndexItem *item);
OrderedIndexItem *skiplistCreateDetached(double score, const char *ele, size_t len);
void skiplistDetachedSetScore(OrderedIndexItem *item, double score);
OrderedIndexItem *skiplistInsertDetached(OrderedIndex *idx, OrderedIndexItem *item);
unsigned long skiplistDeleteRangeByScore(OrderedIndex *idx, double min, double max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx);
unsigned long skiplistDeleteRangeByRank(OrderedIndex *idx, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *ctx);
unsigned long skiplistDeleteRangeByLex(OrderedIndex *idx, const_sds min, const_sds max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx);

/* Query */
unsigned long skiplistLength(OrderedIndex *idx);
OrderedIndexItem *skiplistGetByRank(OrderedIndex *idx, unsigned long rank);
unsigned long skiplistGetRank(OrderedIndex *idx, const OrderedIndexItem *node);
void skiplistGetElementRaw(const OrderedIndexItem *node, const char **ptr, size_t *len);
double skiplistGetScore(const OrderedIndexItem *node);
size_t skiplistElementLen(const char *ptr);
unsigned long skiplistCountScoreRange(OrderedIndex *idx, double min, double max, int min_ex, int max_ex);
unsigned long skiplistCountLexRange(OrderedIndex *idx, const_sds min, const_sds max, int min_ex, int max_ex);

/* Iterator */
void skiplistInitIterator(OrderedIndexIterator *iter, OrderedIndex *idx);
void skiplistResetIterator(OrderedIndexIterator *iter);
bool skiplistNext(OrderedIndexIterator *iter, OrderedIndexItem **pos);
bool skiplistPrev(OrderedIndexIterator *iter, OrderedIndexItem **pos);
void skiplistSeekToRank(OrderedIndexIterator *iter, unsigned long rank);
void skiplistSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset);
void skiplistSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset);

/* Memory */
void skiplistDismissMemory(OrderedIndex *idx);
size_t skiplistEstimateMemory(OrderedIndex *idx, size_t sample_size);

/* Defrag */
OrderedIndex *skiplistDefragInternals(OrderedIndex *idx, void *(*defragfn)(void *));
unsigned long skiplistScanDefrag(OrderedIndex *idx, unsigned long cursor, void (*callback)(OrderedIndexItem *old_item, OrderedIndexItem *new_item, void *ctx), void *ctx, void *(*defragfn)(void *));

/* Debug */
int skiplistGetHeight(OrderedIndex *idx);
int skiplistVerifyIntegrity(OrderedIndex *idx, char *errmsg, size_t errmsg_len);

#endif /* SKIPLIST_ORDERED_INDEX_H */
