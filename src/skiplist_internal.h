#ifndef SKIPLIST_INTERNAL_H
#define SKIPLIST_INTERNAL_H

/* Internal skiplist node helpers shared between t_zset.c and
 * skiplist_ordered_index.c.  Not for use outside the skiplist
 * implementation. */

#include "server.h"

/* Lifecycle */
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
size_t zslGetAllocSize(void);

/* Skiplist structure accessors */
unsigned long zslGetLength(const zskiplist *zsl);
zskiplistNode *zslGetHeader(zskiplist *zsl);
zskiplistNode *zslGetTail(const zskiplist *zsl);
void zslSetTail(zskiplist *zsl, zskiplistNode *tail);
zskiplistNode *zslGetFirst(const zskiplist *zsl);

/* Modification */
zskiplistNode *zslInsert(zskiplist *zsl, double score, const_sds ele);
void zslDelete(zskiplist *zsl, zskiplistNode *node);
zskiplistNode *zslDetachNode(zskiplist *zsl, zskiplistNode *node);
void zslFreeNode(zskiplistNode *node);
zskiplistNode *zslUpdateScore(zskiplist *zsl, zskiplistNode *node, double newscore);

/* Query */
zskiplistNode *zslGetElementByRank(zskiplist *zsl, unsigned long rank);
unsigned long zslGetRank(zskiplist *zsl, const zskiplistNode *node);

/* Iterator */
void zslInitIterator(zskiplistIterator *iter, zskiplist *zsl);
void zslResetIterator(zskiplistIterator *iter);
zskiplistIterator *zslCreateIterator(zskiplist *zsl);
void zslReleaseIterator(zskiplistIterator *iter);
bool zslNext(zskiplistIterator *iter, zskiplistNode **nodeptr);
bool zslPrev(zskiplistIterator *iter, zskiplistNode **nodeptr);
void zslSeekToRank(zskiplistIterator *iter, unsigned long rank);
void zslSeekToScoreRange(zskiplistIterator *iterator, double min, double max, int min_ex, int max_ex, long offset);
void zslSeekToLexRange(zskiplistIterator *iterator, const_sds min, const_sds max, int min_ex, int max_ex, long offset);

/* Level-0 span stores the node height, so span accessors treat it specially. */
static inline unsigned long zslGetNodeSpanAtLevel(const zskiplistNode *x, int level) {
    if (level > 0) return x->level[level].span;
    return x->level[level].forward ? 1 : 0;
}

static inline void zslSetNodeSpanAtLevel(zskiplistNode *x, int level, unsigned long span) {
    if (level > 0) x->level[level].span = span;
}

static inline void zslIncrNodeSpanAtLevel(zskiplistNode *x, int level, unsigned long incr) {
    if (level > 0) x->level[level].span += incr;
}

static inline void zslDecrNodeSpanAtLevel(zskiplistNode *x, int level, unsigned long decr) {
    if (level > 0) x->level[level].span -= decr;
}

static inline unsigned long zslGetNodeHeight(const zskiplistNode *x) {
    return x->level[0].span;
}

#endif /* SKIPLIST_INTERNAL_H */
