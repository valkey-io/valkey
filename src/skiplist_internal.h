#ifndef SKIPLIST_INTERNAL_H
#define SKIPLIST_INTERNAL_H

/* Internal skiplist node helpers shared between t_zset.c and
 * skiplist_ordered_index.c.  Not for use outside the skiplist
 * implementation. */

#include "server.h"

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^64 elements */
#define ZSKIPLIST_MAX_SEARCH 10

/* ZSETs use a specialized version of Skiplists */
typedef struct zskiplistNode {
    union {
        double score;         /* Sorting score for node ordering. */
        unsigned long length; /* Number of elements in the skiplist. */
    };
    union {
        struct zskiplistNode *backward; /* Pointer to previous node for reverse traversal. */
        struct zskiplistNode *tail;     /* Tail element of the skiplist. */
    };
    struct zskiplistLevel {
        struct zskiplistNode *forward;
        /* At each level we keep the span, which is the number of elements which are on the "subtree"
         * from this node at this level to the next node at the same level.
         * One exception is the value at level 0. In level 0 the span can only be 1 or 0 (in case the last elements in the list)
         * So we use it in order to hold the height of the node, which is the number of levels. */
        unsigned long span;
    } level[1]; /* Flexible array member - actual levels determined at node creation. */
    /* For non-header nodes, after the level[], sds header length (1 byte) and an embedded sds element are stored. */
} zskiplistNode;

/* The header node does not store actual data (no score, no backward pointer,
 * and its node height is fixed at ZSKIPLIST_MAXLEVEL).
 * To save memory, we reuse the memory space of these fields in the header node to store:
 *   - skiplist length (number of elements)
 *   - tail pointer to the last element
 *   - maximum current level of the skiplist
 * For detailed memory layout, refer to the zskiplistNode struct definition. */
typedef struct zskiplist {
    zskiplistNode header;
} zskiplist;

/* Skiplist iterator - opaque type that can be stack allocated.
 * Size: 2 x uint64_t = 16 bytes (zsl pointer + node pointer) */
typedef uint64_t zskiplistIterator[2];

/* Lifecycle */
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
size_t zslGetAllocSize(void);

/* Skiplist structure accessors */
int zslGetHeight(const zskiplist *zsl);
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
sds zslGetNodeElement(const zskiplistNode *x);
double zslGetScore(const zskiplistNode *node);
zskiplistNode *zslNthInRange(zskiplist *zsl, zrangespec *range, long n, long *rank);
zskiplistNode *zslNthInLexRange(zskiplist *zsl, zlexrangespec *range, long n);

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

/* Node creation and insertion (used by skiplist_ordered_index.c for detached items) */
zskiplistNode *zslCreateNode(int height, double score, const_sds ele);
int zslRandomLevel(void);
zskiplistNode *zslInsertNode(zskiplist *zsl, zskiplistNode *node);

/* Internal unlink helper (used by skiplist_ordered_index.c for range deletion) */
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update);

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
