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

#include "skiplist.h"
#include "mt19937-64.h"
#include <math.h>


/* Forward declarations */
static zskiplistNode *zslGetElementByRankFromNode(zskiplistNode *start_node, int start_level, unsigned long rank);
/* Create a skiplist node with the specified number of levels.
 * By embedding elements and levels into the skiplist nodes,
 * we achieve good cache-friendliness and a compact memory structure.
 *
 * The memory layout is as follows:
 *
 *   +-------+------------------+---------+-----+---------+-----------------+-------------+
 *   | score | backward-pointer | level-0 | ... | level-N | sds-header-size | element-sds |
 *   +-------+------------------+---------+-----+---------+-----------------+-------------+
 *
 *   sds-header-size and element-sds are only valid for non-header nodes.
 */
zskiplistNode *zslCreateNode(int height, double score, const_sds ele) {
    size_t ele_sds_len = sdslen(ele);
    char ele_sds_type = sdsReqType(ele_sds_len);
    size_t ele_sds_size = sdsReqSize(ele_sds_len, ele_sds_type);
    /* Allocate enough space for the node, levels, and the element sds.
     * We include one extra byte representing the sds header size,
     * which is the offset into the embedded sds data where the
     * string content starts. */
    size_t node_size = zslGetNodeAllocSize(height);
    zskiplistNode *zn = zmalloc(node_size + 1 + ele_sds_size);
    zn->score = score;
    zslSetNodeHeight(zn, height);
    char *data = ((char *)zn) + node_size;
    *data++ = sdsHdrSize(ele_sds_type);
    sdswrite(data, ele_sds_size, ele_sds_type, ele, ele_sds_len);
    return zn;
}

/* Helper function to return the element string from a skip list node. */
sds zslGetNodeElement(const zskiplistNode *x) {
    char *data = ((char *)x) + zslGetNodeAllocSize(zslGetNodeHeight(x));
    int hdr_size = *data;
    data += 1 + hdr_size;
    return (sds)data;
}

/* Helper function to set the height of skiplist. */
static void zslSetHeight(zskiplist *zsl, int height) {
    zsl->header.level[0].span = height;
}

/* Create a new skiplist. */
zskiplist *zslCreate(void) {
    zskiplist *zsl = zcalloc(zslGetAllocSize());
    zslSetHeight(zsl, 1);
    return zsl;
}

/* Helper function to get height of skiplist. */
int zslGetHeight(const zskiplist *zsl) {
    return zsl->header.level[0].span;
}

/* Helper function to get length of skiplist. */
unsigned long zslGetLength(const zskiplist *zsl) {
    return zsl->header.length;
}

/* Helper function to get tail of skiplist. */
zskiplistNode *zslGetTail(const zskiplist *zsl) {
    return zsl->header.tail;
}

/* Helper function to set tail of skiplist. */
void zslSetTail(zskiplist *zsl, zskiplistNode *node) {
    zsl->header.tail = node;
}

/* Helper function to get header of skiplist. */
zskiplistNode *zslGetHeader(zskiplist *zsl) {
    return &zsl->header;
}

/* Free the specified skiplist node. */
void zslFreeNode(zskiplistNode *node) {
    zfree(node);
}

/* Return the size of a zskiplist structure. */
size_t zslGetAllocSize(void) {
    return sizeof(zskiplist) + (ZSKIPLIST_MAXLEVEL - 1) * sizeof(struct zskiplistLevel);
}

/* Free a whole skiplist. */
void zslFree(zskiplist *zsl) {
    zskiplistNode *zheader = zslGetHeader(zsl);
    zskiplistNode *node = zheader->level[0].forward, *next;
    while (node) {
        next = node->level[0].forward;
        zslFreeNode(node);
        node = next;
    }
    zfree(zsl);
}

/* Returns a random level for the new skiplist node we are going to create.
 * The return value of this function is between 1 and ZSKIPLIST_MAXLEVEL
 * (both inclusive), with a powerlaw-alike distribution where higher
 * levels are less likely to be returned. */
int zslRandomLevel(void) {
    uint64_t rand = genrand64_int64();

    /* The probability of gaining 2 additional leading zeros is 0.25.
     * This matches the level calculation logic perfectly: each
     * iteration has a 0.25 probability of increasing the level by 1.
     * Note: __builtin_clzll has undefined behavior when the input is 0. */
    int level = rand == 0 ? ZSKIPLIST_MAXLEVEL : (__builtin_clzll(rand) / 2 + 1);
    return level;
}

/* Compares node and score/ele; defines zset ordering. Return value:
 *     positive if a comes after b.
 *     negative if a comes before b.
 *     0 if a's score and ele are both equal to b's. */
static int zslCompareNodes(const zskiplistNode *a, const zskiplistNode *b) {
    if (a == b) return 0;

    /* null indicates end of list - ordered after any score/ele */
    if (a == NULL) return 1;
    if (b == NULL) return -1;

    if (a->score > b->score) return 1;
    if (a->score < b->score) return -1;

    return sdscmp(zslGetNodeElement(a), zslGetNodeElement(b));
}

/* Insert a node in the skiplist. Assumes the element does not already exist in
 * the skiplist (up to the caller to enforce that). The skiplist takes ownership
 * of the passed node. */
zskiplistNode *zslInsertNode(zskiplist *zsl, zskiplistNode *node) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    unsigned long rank[ZSKIPLIST_MAXLEVEL];
    const int level = zslGetNodeHeight(node);

    serverAssert(!isnan(node->score));
    zskiplistNode *x = zslGetHeader(zsl);
    for (int i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        /* store rank that is crossed to reach the insert position */
        rank[i] = i == (zslGetHeight(zsl) - 1) ? 0 : rank[i + 1];
        while (zslCompareNodes(x->level[i].forward, node) < 0) {
            rank[i] += zslGetNodeSpanAtLevel(x, i);
            x = x->level[i].forward;
        }
        update[i] = x;
    }
    /* we assume the element is not already inside, since we allow duplicated
     * scores, reinserting the same element should never happen since the
     * caller should test in the hash table if the element is
     * already inside or not. */
    if (level > zslGetHeight(zsl)) {
        for (int i = zslGetHeight(zsl); i < level; i++) {
            rank[i] = 0;
            update[i] = zslGetHeader(zsl);
            zslSetNodeSpanAtLevel(update[i], i, zslGetLength(zsl));
        }
        zslSetHeight(zsl, level);
    }
    for (int i = 0; i < level; i++) {
        node->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = node;

        /* update span covered by update[i] as x is inserted here */
        zslSetNodeSpanAtLevel(node, i, zslGetNodeSpanAtLevel(update[i], i) - (rank[0] - rank[i]));
        zslSetNodeSpanAtLevel(update[i], i, (rank[0] - rank[i]) + 1);
    }

    /* increment span for untouched levels */
    for (int i = level; i < zslGetHeight(zsl); i++) {
        zslIncrNodeSpanAtLevel(update[i], i, 1);
    }

    node->backward = (update[0] == zslGetHeader(zsl)) ? NULL : update[0];
    if (node->level[0].forward)
        node->level[0].forward->backward = node;
    else
        zslSetTail(zsl, node);
    zsl->header.length++;
    return node;
}

/* Insert a new node in the skiplist. Assumes the element does not already
 * exist (up to the caller to enforce that). The string 'ele' is copied. */
zskiplistNode *zslInsert(zskiplist *zsl, double score, const_sds ele) {
    const int level = zslRandomLevel();
    zskiplistNode *node = zslCreateNode(level, score, ele);
    zslInsertNode(zsl, node);
    return node;
}

/* Internal function used by zslDelete, zslDeleteRangeByScore and
 * zslDeleteRangeByRank. */
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;
    for (i = 0; i < zslGetHeight(zsl); i++) {
        if (update[i]->level[i].forward == x) {
            zslIncrNodeSpanAtLevel(update[i], i, zslGetNodeSpanAtLevel(x, i) - 1);
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            zslDecrNodeSpanAtLevel(update[i], i, 1);
        }
    }
    if (x->level[0].forward) {
        x->level[0].forward->backward = x->backward;
    } else {
        zslSetTail(zsl, x->backward);
    }

    int level;
    zskiplistNode *zheader = zslGetHeader(zsl);
    while ((level = zslGetHeight(zsl)) > 1 && zheader->level[level - 1].forward == NULL) zslSetHeight(zsl, level - 1);
    zsl->header.length--;
}

/* Delete specified node from the skiplist. */
void zslDelete(zskiplist *zsl, zskiplistNode *node) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    zskiplistNode *x = zslGetHeader(zsl);
    for (int i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (zslCompareNodes(x->level[i].forward, node) < 0) {
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* We should have arrived at the correct node */
    serverAssert(x->level[0].forward == node);

    zslDeleteNode(zsl, node, update);
    zslFreeNode(node);
}

/* Update the score of an element inside the sorted set skiplist.
 * Note that the element must exist in the skiplist.
 *
 * Note that this function attempts to just update the node, in case after
 * the score update, the node would be exactly at the same position. If the old
 * node can be kept it returns NULL.
 * Otherwise the skiplist is modified by removing and re-adding a new
 * element, which is more costly. A pointer to the new node is returned. */
zskiplistNode *zslUpdateScore(zskiplist *zsl, zskiplistNode *node, double newscore) {
    /* If the node, after the score update, would be still exactly
     * at the same position, we can just update the score without
     * actually removing and re-inserting the element in the skiplist. */
    if ((node->backward == NULL || node->backward->score < newscore) &&
        (node->level[0].forward == NULL || node->level[0].forward->score > newscore)) {
        node->score = newscore;
        return NULL;
    }

    /* We need to remove the node from the skiplist and insert a new one */
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    zskiplistNode *x = zslGetHeader(zsl);
    for (int i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (zslCompareNodes(x->level[i].forward, node) < 0) {
            x = x->level[i].forward;
        }
        update[i] = x;
    }
    /* We assume that the node exists in the skiplist */
    serverAssert(x->level[0].forward == node);

    zslDeleteNode(zsl, node, update);
    node->score = newscore; /* reuse existing node to avoid memory allocation */
    zslInsertNode(zsl, node);
    return node;
}

int zslValueGteMin(double value, zrangespec *spec) {
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

int zslValueLteMax(double value, zrangespec *spec) {
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

/* Returns if there is a part of the zset is in range. */
int zslIsInRange(zskiplist *zsl, zrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max || (range->min == range->max && (range->minex || range->maxex))) return 0;
    x = zslGetTail(zsl);
    if (x == NULL || !zslValueGteMin(x->score, range)) return 0;
    zskiplistNode *zheader = zslGetHeader(zsl);
    x = zheader->level[0].forward;
    if (x == NULL || !zslValueLteMax(x->score, range)) return 0;
    return 1;
}

/* Find the Nth node that is contained in the specified range. N should be 0-based.
 * Negative N works for reversed order (-1 represents the last element). Returns
 * NULL when no element is contained in the range.
 * If rank is not NULL it will be set to the element's overall rank */
zskiplistNode *zslNthInRange(zskiplist *zsl, zrangespec *range, long n, long *rank) {
    /* If everything is out of range, return early. */
    if (!zslIsInRange(zsl, range)) return NULL;

    /* Go forward while *OUT* of range at the highest level. */
    zskiplistNode *x = zslGetHeader(zsl);
    int i = zslGetHeight(zsl) - 1;
    long last_highest_level_rank = 0;
    while (x->level[i].forward && !zslValueGteMin(x->level[i].forward->score, range)) {
        last_highest_level_rank += zslGetNodeSpanAtLevel(x, i);
        x = x->level[i].forward;
    }
    /* Remember the last node which has zslGetHeight(zsl)-1 levels */
    zskiplistNode *last_highest_level_node = x;

    if (n >= 0) {
        long start_rank = last_highest_level_rank;
        for (i = zslGetHeight(zsl) - 2; i >= 0; i--) {
            /* Go forward while *OUT* of range. */
            while (x->level[i].forward && !zslValueGteMin(x->level[i].forward->score, range)) {
                /* Count the rank of the last element smaller than the range. */
                start_rank += zslGetNodeSpanAtLevel(x, i);
                x = x->level[i].forward;
            }
        }
        /* Check if zsl is long enough. */
        if ((unsigned long)(start_rank + n) >= zslGetLength(zsl)) return NULL;
        if (n < ZSKIPLIST_MAX_SEARCH) {
            /* If offset is small, we can just jump node by node */
            /* rank+1 is the first element in range, so we need n+1 steps to reach target. */
            for (i = 0; i < n + 1; i++) {
                x = x->level[0].forward;
            }
        } else {
            /* If offset is big, we can jump from the last zslGetHeight(zsl)-1 node. */
            unsigned long rank_diff = start_rank + 1 + n - last_highest_level_rank;
            x = zslGetElementByRankFromNode(last_highest_level_node, zslGetHeight(zsl) - 1, rank_diff);
        }
        /* Check if score <= max. */
        if (x && !zslValueLteMax(x->score, range)) return NULL;
        if (rank) *rank = start_rank + n;
    } else {
        long end_rank = last_highest_level_rank;
        for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
            /* Go forward while *IN* range. */
            while (x->level[i].forward && zslValueLteMax(x->level[i].forward->score, range)) {
                /* Count the rank of the last element in range. */
                end_rank += zslGetNodeSpanAtLevel(x, i);
                x = x->level[i].forward;
            }
        }
        /* Check if the range is big enough. */
        if (end_rank < -n) return NULL;
        if (n + 1 > -ZSKIPLIST_MAX_SEARCH) {
            /* If offset is small, we can just jump node by node */
            /* rank is the -1th element in range, so we need -n-1 steps to reach target. */
            for (i = 0; i < -n - 1; i++) {
                x = x->backward;
            }
        } else {
            /* If offset is big, we can jump from the last zslGetHeight(zsl)-1 node. */
            /* rank is the last element in range, n is -1-based, so we need n+1 to count backwards. */
            unsigned long rank_diff = end_rank + 1 + n - last_highest_level_rank;
            x = zslGetElementByRankFromNode(last_highest_level_node, zslGetHeight(zsl) - 1, rank_diff);
        }
        /* Check if score >= min. */
        if (x && !zslValueGteMin(x->score, range)) return NULL;
        if (rank) *rank = end_rank + n;
    }

    return x;
}

/* Delete all the elements with score between min and max from the skiplist.
 * Both min and max can be inclusive or exclusive (see range->minex and
 * range->maxex). When inclusive a score >= min && score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range, hashtable *ht) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zslGetHeader(zsl);
    for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (x->level[i].forward && !zslValueGteMin(x->level[i].forward->score, range)) x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x && zslValueLteMax(x->score, range)) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl, x, update);
        sds ele = zslGetNodeElement(x);
        hashtablePop(ht, ele, NULL);
        zslFreeNode(x);
        removed++;
        x = next;
    }
    return removed;
}

unsigned long zslDeleteRangeByLex(zskiplist *zsl, zlexrangespec *range, hashtable *ht) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;


    x = zslGetHeader(zsl);
    for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               !zslLexValueGteMin(zslGetNodeElement(x->level[i].forward), range)) {
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x && zslLexValueLteMax(zslGetNodeElement(x), range)) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl, x, update);
        hashtableDelete(ht, zslGetNodeElement(x));
        zslFreeNode(x); /* Here is where x->ele is actually released. */
        removed++;
        x = next;
    }
    return removed;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, hashtable *ht) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    x = zslGetHeader(zsl);
    for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + zslGetNodeSpanAtLevel(x, i)) < start) {
            traversed += zslGetNodeSpanAtLevel(x, i);
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    traversed++;
    x = x->level[0].forward;
    while (x && traversed <= end) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl, x, update);
        hashtableDelete(ht, zslGetNodeElement(x));
        zslFreeNode(x);
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}

/* Find the rank for a specific skiplist member node. Counts nodes after the one
 * specified and subtracts from list length. Note that rank is 1-based.  */
unsigned long zslGetRank(zskiplist *zsl, const zskiplistNode *node) {
    unsigned long count_after_node = 0;
    while (node) { /* note this is never null the first time */
        int highest_node_span = zslGetNodeHeight(node) - 1;
        count_after_node += zslGetNodeSpanAtLevel(node, highest_node_span);
        node = node->level[highest_node_span].forward;
    }

    unsigned long rank = zslGetLength(zsl) - count_after_node;
    return rank;
}

/* Finds an element by its rank from start node. The rank argument needs to be 1-based. */
static zskiplistNode *zslGetElementByRankFromNode(zskiplistNode *start_node, int start_level, unsigned long rank) {
    zskiplistNode *x;
    unsigned long traversed = 0;
    int i;

    x = start_node;
    for (i = start_level; i >= 0; i--) {
        while (x->level[i].forward && (traversed + zslGetNodeSpanAtLevel(x, i)) <= rank) {
            traversed += zslGetNodeSpanAtLevel(x, i);
            x = x->level[i].forward;
        }
        if (traversed == rank) {
            return x;
        }
    }
    return NULL;
}

/* Finds an element by its rank. The rank argument needs to be 1-based. */
zskiplistNode *zslGetElementByRank(zskiplist *zsl, unsigned long rank) {
    return zslGetElementByRankFromNode(zslGetHeader(zsl), zslGetHeight(zsl) - 1, rank);
}

/* Populate the rangespec according to the objects min and max. */

/* ------------------------ Lexicographic ranges ---------------------------- */

/* Parse max or min argument of ZRANGEBYLEX.
 * (foo means foo (open interval)
 * [foo means foo (closed interval)
 * - means the min string possible
 * + means the max string possible
 *
 * If the string is valid the *dest pointer is set to the Object
 * that will be used for the comparison, and ex will be set to 0 or 1
 * respectively if the item is exclusive or inclusive. C_OK will be
 * returned.
 *
 * If the string is not a valid range C_ERR is returned, and the value
 * of *dest and *ex is undefined. */

/* This is just a wrapper to sdscmp() that is able to
 * handle shared.minstring and shared.maxstring as the equivalent of
 * -inf and +inf for strings */
int sdscmplex(sds a, sds b) {
    if (a == b) return 0;
    if (a == shared.minstring || b == shared.maxstring) return -1;
    if (a == shared.maxstring || b == shared.minstring) return 1;
    return sdscmp(a, b);
}

int zslLexValueGteMin(sds value, zlexrangespec *spec) {
    return spec->minex ? (sdscmplex(value, spec->min) > 0) : (sdscmplex(value, spec->min) >= 0);
}

int zslLexValueLteMax(sds value, zlexrangespec *spec) {
    return spec->maxex ? (sdscmplex(value, spec->max) < 0) : (sdscmplex(value, spec->max) <= 0);
}

/* Returns if there is a part of the zset is in the lex range. */
static int zslIsInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    int cmp = sdscmplex(range->min, range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex))) return 0;
    x = zslGetTail(zsl);
    sds ele = zslGetNodeElement(x);
    if (x == NULL || !zslLexValueGteMin(ele, range)) return 0;
    zskiplistNode *zheader = zslGetHeader(zsl);
    x = zheader->level[0].forward;
    ele = zslGetNodeElement(x);
    if (x == NULL || !zslLexValueLteMax(ele, range)) return 0;
    return 1;
}

/* Find the Nth node that is contained in the specified range. N should be 0-based.
 * Negative N works for reversed order (-1 represents the last element). Returns
 * NULL when no element is contained in the range. */
zskiplistNode *zslNthInLexRange(zskiplist *zsl, zlexrangespec *range, long n) {
    zskiplistNode *x;
    int i;
    long edge_rank = 0;
    long last_highest_level_rank = 0;
    zskiplistNode *last_highest_level_node = NULL;
    unsigned long rank_diff;

    /* If everything is out of range, return early. */
    if (!zslIsInLexRange(zsl, range)) return NULL;

    /* Go forward while *OUT* of range at highest level. */
    x = zslGetHeader(zsl);
    i = zslGetHeight(zsl) - 1;
    while (x->level[i].forward && !zslLexValueGteMin(zslGetNodeElement(x->level[i].forward), range)) {
        edge_rank += zslGetNodeSpanAtLevel(x, i);
        x = x->level[i].forward;
    }
    /* Remember the last node which has zslGetHeight(zsl)-1 levels and its rank. */
    last_highest_level_node = x;
    last_highest_level_rank = edge_rank;

    if (n >= 0) {
        for (i = zslGetHeight(zsl) - 2; i >= 0; i--) {
            /* Go forward while *OUT* of range. */
            while (x->level[i].forward && !zslLexValueGteMin(zslGetNodeElement(x->level[i].forward), range)) {
                /* Count the rank of the last element smaller than the range. */
                edge_rank += zslGetNodeSpanAtLevel(x, i);
                x = x->level[i].forward;
            }
        }
        /* Check if zsl is long enough. */
        if ((unsigned long)(edge_rank + n) >= zslGetLength(zsl)) return NULL;
        if (n < ZSKIPLIST_MAX_SEARCH) {
            /* If offset is small, we can just jump node by node */
            /* rank+1 is the first element in range, so we need n+1 steps to reach target. */
            for (i = 0; i < n + 1; i++) {
                x = x->level[0].forward;
            }
        } else {
            /* If offset is big, we caasn jump from the last zslGetHeight(zsl)-1 node. */
            rank_diff = edge_rank + 1 + n - last_highest_level_rank;
            x = zslGetElementByRankFromNode(last_highest_level_node, zslGetHeight(zsl) - 1, rank_diff);
        }
        /* Check if score <= max. */
        if (x && !zslLexValueLteMax(zslGetNodeElement(x), range)) return NULL;
    } else {
        for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
            /* Go forward while *IN* range. */
            while (x->level[i].forward && zslLexValueLteMax(zslGetNodeElement(x->level[i].forward), range)) {
                /* Count the rank of the last element in range. */
                edge_rank += zslGetNodeSpanAtLevel(x, i);
                x = x->level[i].forward;
            }
        }
        /* Check if the range is big enough. */
        if (edge_rank < -n) return NULL;
        if (n + 1 > -ZSKIPLIST_MAX_SEARCH) {
            /* If offset is small, we can just jump node by node */
            for (i = 0; i < -n - 1; i++) {
                x = x->backward;
            }
        } else {
            /* If offset is big, we can jump from the last zslGetHeight(zsl)-1 node. */
            /* rank is the last element in range, n is -1-based, so we need n+1 to count backwards. */
            rank_diff = edge_rank + 1 + n - last_highest_level_rank;
            x = zslGetElementByRankFromNode(last_highest_level_node, zslGetHeight(zsl) - 1, rank_diff);
        }
        /* Check if score >= min. */
        if (x && !zslLexValueGteMin(zslGetNodeElement(x), range)) return NULL;
    }

    return x;
}
