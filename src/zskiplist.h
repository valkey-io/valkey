/* Skiplist data structure, used by sorted sets in t_zset.c */

#ifndef __ZSKIPLIST_H
#define __ZSKIPLIST_H

#include "sds.h"

/* Forward declarations */
typedef struct zrangespec zrangespec;
typedef struct zlexrangespec zlexrangespec;
typedef struct hashtable hashtable;

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^64 elements */
#define ZSKIPLIST_MAX_SEARCH 10

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


/* Skiplist API - lifecycle */
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
size_t zslGetAllocSize(void);

/* Skiplist API - accessors */
int zslGetHeight(const zskiplist *zsl);
zskiplistNode *zslGetTail(const zskiplist *zsl);
void zslSetTail(zskiplist *zsl, zskiplistNode *tail);
unsigned long zslGetLength(const zskiplist *zsl);
zskiplistNode *zslGetHeader(zskiplist *zsl);
sds zslGetNodeElement(const zskiplistNode *x);

/* Skiplist API - modification */
zskiplistNode *zslInsert(zskiplist *zsl, double score, const_sds ele);
zskiplistNode *zslInsertNode(zskiplist *zsl, zskiplistNode *node);
zskiplistNode *zslCreateNode(int height, double score, const_sds ele);
int zslRandomLevel(void);
int zslCompareNodes(const zskiplistNode *a, const zskiplistNode *b);
void zslDelete(zskiplist *zsl, zskiplistNode *node);
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update);
void zslFreeNode(zskiplistNode *node);
zskiplistNode *zslUpdateScore(zskiplist *zsl, zskiplistNode *node, double newscore);
unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range, hashtable *ht);
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, hashtable *ht);

/* Skiplist API - range queries */
int zslValueGteMin(double value, zrangespec *spec);
int zslValueLteMax(double value, zrangespec *spec);
zskiplistNode *zslNthInRange(zskiplist *zsl, zrangespec *range, long n, long *rank);
zskiplistNode *zslGetElementByRankFromNode(zskiplistNode *start_node, int start_level, unsigned long rank);
zskiplistNode *zslGetElementByRank(zskiplist *zsl, unsigned long rank);

/* Lex range sentinels for -inf/+inf in lexicographic comparisons */
extern sds zslMinString;
extern sds zslMaxString;

/* Lex range functions */
int sdscmplex(sds a, sds b);
int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);
zskiplistNode *zslNthInLexRange(zskiplist *zsl, zlexrangespec *range, long n);
unsigned long zslDeleteRangeByLex(zskiplist *zsl, zlexrangespec *range, hashtable *ht);
unsigned long zslGetRank(zskiplist *zsl, const zskiplistNode *node);

/*-----------------------------------------------------------------------------
 * Skiplist node helpers (static inline for use in multiple files)
 *----------------------------------------------------------------------------*/

static inline unsigned long zslGetNodeSpanAtLevel(const zskiplistNode *x, int level) {
    /* We use the level 0 span in order to hold the node height, so in case the span is requested on
     * level 0 and this is not the last node we return 1 and 0 otherwise. For the rest of the levels we just return
     * the recorded span in that level. */
    if (level > 0) return x->level[level].span;
    return x->level[level].forward ? 1 : 0;
}

static inline void zslSetNodeSpanAtLevel(zskiplistNode *x, int level, unsigned long span) {
    /* We use the level 0 span in order to hold the node height, so we avoid overriding it. */
    if (level > 0)
        x->level[level].span = span;
}

static inline void zslIncrNodeSpanAtLevel(zskiplistNode *x, int level, unsigned long incr) {
    /* We use the level 0 span in order to hold the node height, so we avoid overriding it. */
    if (level > 0)
        x->level[level].span += incr;
}

static inline void zslDecrNodeSpanAtLevel(zskiplistNode *x, int level, unsigned long decr) {
    /* We use the level 0 span in order to hold the node height, so we avoid overriding it. */
    if (level > 0)
        x->level[level].span -= decr;
}

static inline unsigned long zslGetNodeHeight(const zskiplistNode *x) {
    /* Since the span at level 0 is always 1 (or 0 for the last node), this
     * field is instead used for storing the height of the node. */
    return x->level[0].span;
}

static inline void zslSetNodeHeight(zskiplistNode *x, int height) {
    /* Since the span at level 0 is always 1 (or 0 for the last node), this
     * field is instead used for storing the height of the node. */
    x->level[0].span = height;
}

static inline size_t zslGetNodeAllocSize(int level) {
    /* Calculate the memory size required for a zskiplist node (excluding the sds element).
     * zskiplistLevel is embedded in zskiplistNode, so we calculate based on the struct layout. */
    return sizeof(zskiplistNode) + (level - 1) * sizeof(((zskiplistNode *)0)->level[0]);
}

#endif /* __ZSKIPLIST_H */
