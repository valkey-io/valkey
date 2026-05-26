/*
 * Copyright (c) 2009-2012, Redis Ltd.
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SKIPLIST_H
#define SKIPLIST_H

#include "server.h"

/*
 * This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 * a) this implementation allows for repeated scores.
 * b) the comparison is not just by key (our 'score') but by satellite data.
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE.
 */

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

/* --- Inline helpers --- */

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

static inline void zslSetNodeHeight(zskiplistNode *x, int height) {
    x->level[0].span = height;
}

static inline size_t zslGetNodeAllocSize(int level) {
    return sizeof(zskiplistNode) + (level - 1) * sizeof(struct zskiplistLevel);
}

/* --- Public API --- */

/* Creation and destruction */
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
size_t zslGetAllocSize(void);

/* Accessors */
int zslGetHeight(const zskiplist *zsl);
unsigned long zslGetLength(const zskiplist *zsl);
zskiplistNode *zslGetTail(const zskiplist *zsl);
void zslSetTail(zskiplist *zsl, zskiplistNode *node);
zskiplistNode *zslGetHeader(zskiplist *zsl);
sds zslGetNodeElement(const zskiplistNode *x);

/* Insertion */
zskiplistNode *zslCreateNode(int height, double score, const_sds ele);
int zslRandomLevel(void);
zskiplistNode *zslInsertNode(zskiplist *zsl, zskiplistNode *node);
zskiplistNode *zslInsert(zskiplist *zsl, double score, const_sds ele);

/* Deletion */
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update);
void zslDelete(zskiplist *zsl, zskiplistNode *node);
void zslFreeNode(zskiplistNode *node);
unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range, hashtable *ht);
unsigned long zslDeleteRangeByLex(zskiplist *zsl, zlexrangespec *range, hashtable *ht);
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, hashtable *ht);

/* Score update */
zskiplistNode *zslUpdateScore(zskiplist *zsl, zskiplistNode *node, double newscore);

/* Queries */
int zslValueGteMin(double value, zrangespec *spec);
int zslValueLteMax(double value, zrangespec *spec);
int zslIsInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslNthInRange(zskiplist *zsl, zrangespec *range, long n, long *rank);
unsigned long zslGetRank(zskiplist *zsl, const zskiplistNode *node);
zskiplistNode *zslGetElementByRank(zskiplist *zsl, unsigned long rank);

/* Lex queries */
int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);
int sdscmplex(sds a, sds b);
zskiplistNode *zslNthInLexRange(zskiplist *zsl, zlexrangespec *range, long n);

#endif /* SKIPLIST_H */
