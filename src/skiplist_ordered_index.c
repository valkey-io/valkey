#include "server.h"
#include "ordered_index.h"
#include "skiplist_internal.h"

static_assert(sizeof(OrderedIndexIterator) >= sizeof(zskiplistIterator),
              "OrderedIndexIterator must be large enough to hold zskiplistIterator");

/* Skiplist implementation of OrderedIndex interface */

/*-----------------------------------------------------------------------------
 * Internal skiplist helpers
 *---------------------------------------------------------------------------*/

/* Internal function to unlink a node from the skiplist (does not free it). */
static void skiplistUnlinkNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
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
    while ((level = zslGetHeight(zsl)) > 1 && zheader->level[level - 1].forward == NULL) {
        /* zslSetHeight is static in t_zset.c, replicate inline: header level[0].span = height */
        zheader->level[0].span = level - 1;
    }
    zsl->header.length--;
}

/* Lifecycle */

OrderedIndex *skiplistCreate(void) {
    return (OrderedIndex *)zslCreate();
}

void skiplistFree(OrderedIndex *idx) {
    zslFree((zskiplist *)idx);
}

/* Modification */

OrderedIndexItem *skiplistInsert(OrderedIndex *idx, double score, const char *ele, size_t len) {
    sds tmp = sdsnewlen(ele, len);
    OrderedIndexItem *node = (OrderedIndexItem *)zslInsert((zskiplist *)idx, score, tmp);
    sdsfree(tmp);
    return node;
}

void skiplistDelete(OrderedIndex *idx, OrderedIndexItem *node) {
    zslDelete((zskiplist *)idx, (zskiplistNode *)node);
}

OrderedIndexItem *skiplistUpdateScore(OrderedIndex *idx, OrderedIndexItem *node, double newscore) {
    zskiplistNode *result = zslUpdateScore((zskiplist *)idx, (zskiplistNode *)node, newscore);
    return result ? (OrderedIndexItem *)result : (OrderedIndexItem *)node;
}

OrderedIndexItem *skiplistPopFirst(OrderedIndex *idx) {
    zskiplist *zsl = (zskiplist *)idx;
    zskiplistNode *first = zslGetFirst(zsl);
    if (!first) return NULL;
    zslDetachNode(zsl, first);
    return (OrderedIndexItem *)first;
}

OrderedIndexItem *skiplistPopLast(OrderedIndex *idx) {
    zskiplist *zsl = (zskiplist *)idx;
    zskiplistNode *last = zslGetTail(zsl);
    if (!last) return NULL;
    zslDetachNode(zsl, last);
    return (OrderedIndexItem *)last;
}

void skiplistFreeItem(OrderedIndexItem *item) {
    zslFreeNode((zskiplistNode *)item);
}

unsigned long skiplistDeleteRangeByScore(OrderedIndex *idx, double min, double max,
                                         int min_ex, int max_ex,
                                         OrderedIndexOnDelete on_delete, void *ctx) {
    zskiplist *zsl = (zskiplist *)idx;
    zrangespec range = {.min = min, .max = max, .minex = min_ex, .maxex = max_ex};
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zslGetHeader(zsl);
    for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (x->level[i].forward && !zslValueGteMin(x->level[i].forward->score, &range))
            x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x && zslValueLteMax(x->score, &range)) {
        zskiplistNode *next = x->level[0].forward;
        skiplistUnlinkNode(zsl, x, update);
        if (on_delete) {
            on_delete((OrderedIndexItem *)x, ctx);
        }
        zslFreeNode(x);
        removed++;
        x = next;
    }
    return removed;
}

unsigned long skiplistDeleteRangeByRank(OrderedIndex *idx, unsigned long start, unsigned long end,
                                        OrderedIndexOnDelete on_delete, void *ctx) {
    zskiplist *zsl = (zskiplist *)idx;
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
        skiplistUnlinkNode(zsl, x, update);
        if (on_delete) {
            on_delete((OrderedIndexItem *)x, ctx);
        }
        zslFreeNode(x);
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}

unsigned long skiplistDeleteRangeByLex(OrderedIndex *idx, const_sds min, const_sds max,
                                       int min_ex, int max_ex,
                                       OrderedIndexOnDelete on_delete, void *ctx) {
    zskiplist *zsl = (zskiplist *)idx;
    zlexrangespec range = {.min = (sds)min, .max = (sds)max, .minex = min_ex, .maxex = max_ex};
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zslGetHeader(zsl);
    for (i = zslGetHeight(zsl) - 1; i >= 0; i--) {
        while (x->level[i].forward) {
            sds fwd_ele = zslGetNodeElement(x->level[i].forward);
            if (zslLexValueGteMin(fwd_ele, sdslen(fwd_ele), &range)) break;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* Current node is the last with element < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x) {
        sds ele = zslGetNodeElement(x);
        if (!zslLexValueLteMax(ele, sdslen(ele), &range)) break;
        zskiplistNode *next = x->level[0].forward;
        skiplistUnlinkNode(zsl, x, update);
        if (on_delete) {
            on_delete((OrderedIndexItem *)x, ctx);
        }
        zslFreeNode(x);
        removed++;
        x = next;
    }
    return removed;
}

/* Query */

unsigned long skiplistLength(OrderedIndex *idx) {
    return zslGetLength((zskiplist *)idx);
}

OrderedIndexItem *skiplistGetByRank(OrderedIndex *idx, unsigned long rank) {
    return (OrderedIndexItem *)zslGetElementByRank((zskiplist *)idx, rank);
}

unsigned long skiplistGetRank(OrderedIndex *idx, const OrderedIndexItem *node) {
    return zslGetRank((zskiplist *)idx, (const zskiplistNode *)node);
}

void skiplistGetElementRaw(const OrderedIndexItem *node, const char **ptr, size_t *len) {
    const zskiplistNode *znode = (const zskiplistNode *)node;
    sds ele = zslGetNodeElement(znode);
    *ptr = ele;
    *len = sdslen(ele);
}

double skiplistGetScore(const OrderedIndexItem *node) {
    return zslGetScore((const zskiplistNode *)node);
}

/* Iterator */

void skiplistInitIterator(OrderedIndexIterator *iter, OrderedIndex *idx) {
    zslInitIterator((zskiplistIterator *)iter, (zskiplist *)idx);
}

void skiplistResetIterator(OrderedIndexIterator *iter) {
    zslResetIterator((zskiplistIterator *)iter);
}

bool skiplistNext(OrderedIndexIterator *iter, OrderedIndexItem **pos) {
    return zslNext((zskiplistIterator *)iter, (zskiplistNode **)pos);
}

bool skiplistPrev(OrderedIndexIterator *iter, OrderedIndexItem **pos) {
    return zslPrev((zskiplistIterator *)iter, (zskiplistNode **)pos);
}

void skiplistSeekToRank(OrderedIndexIterator *iter, unsigned long rank) {
    zslSeekToRank((zskiplistIterator *)iter, rank);
}

void skiplistSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset) {
    zslSeekToScoreRange((zskiplistIterator *)iter, min, max, min_ex, max_ex, offset);
}

void skiplistSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset) {
    zslSeekToLexRange((zskiplistIterator *)iter, min, max, min_ex, max_ex, offset);
}

/* Debug */

int skiplistGetHeight(OrderedIndex *idx) {
    return zslGetHeight((zskiplist *)idx);
}

/* Memory */

void skiplistDismissMemory(OrderedIndex *idx) {
    zskiplist *zsl = (zskiplist *)idx;
    zskiplistNode *zn = zslGetTail(zsl);
    while (zn != NULL) {
        zskiplistNode *prev = zn->backward;
        dismissMemory(zn, 0);
        zn = prev;
    }
}

size_t skiplistEstimateMemory(OrderedIndex *idx, size_t sample_size) {
    zskiplist *zsl = (zskiplist *)idx;
    unsigned long length = zslGetLength(zsl);
    size_t asize = zslGetAllocSize();

    if (length == 0) return asize;

    size_t elesize = 0;
    size_t samples = 0;
    zskiplistNode *znode = zslGetHeader(zsl)->level[0].forward;
    while (znode != NULL && samples < sample_size) {
        elesize += zmalloc_size(znode);
        samples++;
        znode = znode->level[0].forward;
    }
    if (samples) asize += (double)elesize / samples * length;
    return asize;
}

/* Defrag */

OrderedIndex *skiplistDefragInternals(OrderedIndex *idx, void *(*defragfn)(void *)) {
    OrderedIndex *newidx = defragfn(idx);
    return newidx; /* NULL if no move needed */
}

/* Patch skiplist pointers after a node has been reallocated to a new address.
 * update[] contains the predecessor at each level. */
static void skiplistPatchNodePointers(zskiplist *zsl, zskiplistNode *oldnode,
                                      zskiplistNode *newnode, zskiplistNode **update) {
    for (int i = 0; i < zslGetHeight(zsl); i++) {
        if (update[i]->level[i].forward == oldnode)
            update[i]->level[i].forward = newnode;
    }
    if (newnode->level[0].forward) {
        newnode->level[0].forward->backward = newnode;
    } else {
        zslSetTail(zsl, newnode);
    }
}

/* Cursor-based incremental defrag of skiplist nodes.
 *
 * Walks nodes forward from cursor position (a rank), defragging as it
 * goes via defragfn. When an item is reallocated there is a callback
 * for any other pointer updates needed.
 *
 * Processes up to 64 nodes per call to bound latency, returning the
 * next cursor position (or 0 when complete). */
unsigned long skiplistScanDefrag(OrderedIndex *idx, unsigned long cursor,
                                 void (*callback)(OrderedIndexItem *old_item, OrderedIndexItem *new_item, void *ctx),
                                 void *ctx,
                                 void *(*defragfn)(void *)) {
    zskiplist *zsl = (zskiplist *)idx;
    zskiplistNode *header = zslGetHeader(zsl);

    /* cursor is the 1-based rank of the next node to process, 0 means start */
    zskiplistNode *prev = header;
    if (cursor > 0) {
        /* Seek to the node just before cursor position */
        zskiplistNode *target = zslGetElementByRank(zsl, cursor);
        if (target == NULL) return 0; /* past end */
        prev = target->backward ? target->backward : header;
    }

    zskiplistNode *node = prev->level[0].forward;
    unsigned long count = 0;
    unsigned long rank = cursor > 0 ? cursor : 1;

    while (node != NULL && count < 16) {
        zskiplistNode *next = node->level[0].forward;
        zskiplistNode *newnode = defragfn(node);

        if (newnode) {
            /* Node was reallocated. Find predecessors and patch pointers. */
            zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
            zskiplistNode *x = header;
            double score = newnode->score;
            sds ele = zslGetNodeElement(newnode);
            for (int i = zslGetHeight(zsl) - 1; i >= 0; i--) {
                while (x->level[i].forward &&
                       (x->level[i].forward->score < score ||
                        (x->level[i].forward->score == score &&
                         sdscmp(zslGetNodeElement(x->level[i].forward), ele) < 0))) {
                    x = x->level[i].forward;
                }
                update[i] = x;
            }
            skiplistPatchNodePointers(zsl, node, newnode, update);
            callback((OrderedIndexItem *)node, (OrderedIndexItem *)newnode, ctx);
        }

        node = next;
        rank++;
        count++;
    }

    return node ? rank : 0; /* 0 = done */
}

