#include "server.h"
#include "ordered_index.h"

static_assert(sizeof(OrderedIndexIterator) >= sizeof(zskiplistIterator),
              "OrderedIndexIterator must be large enough to hold zskiplistIterator");

/* Skiplist implementation of OrderedIndex interface */

/* Lifecycle */

static OrderedIndex *skiplistCreate(void) {
    return (OrderedIndex *)zslCreate();
}

static void skiplistFree(OrderedIndex *idx) {
    zslFree((zskiplist *)idx);
}

/* Modification */

static OrderedIndexItem *skiplistInsert(OrderedIndex *idx, double score, const_sds ele) {
    return (OrderedIndexItem *)zslInsert((zskiplist *)idx, score, ele);
}

static void skiplistDelete(OrderedIndex *idx, OrderedIndexItem *node) {
    zslDelete((zskiplist *)idx, (zskiplistNode *)node);
}

static OrderedIndexItem *skiplistUpdateScore(OrderedIndex *idx, OrderedIndexItem *node, double newscore) {
    zskiplistNode *result = zslUpdateScore((zskiplist *)idx, (zskiplistNode *)node, newscore);
    return result ? (OrderedIndexItem *)result : (OrderedIndexItem *)node;
}

static OrderedIndexItem *skiplistPopFirst(OrderedIndex *idx) {
    zskiplist *zsl = (zskiplist *)idx;
    zskiplistNode *first = zslGetFirst(zsl);
    if (!first) return NULL;
    zslDetachNode(zsl, first);
    return (OrderedIndexItem *)first;
}

static OrderedIndexItem *skiplistPopLast(OrderedIndex *idx) {
    zskiplist *zsl = (zskiplist *)idx;
    zskiplistNode *last = zslGetTail(zsl);
    if (!last) return NULL;
    zslDetachNode(zsl, last);
    return (OrderedIndexItem *)last;
}

static void skiplistFreeItem(OrderedIndexItem *item) {
    zslFreeNode((zskiplistNode *)item);
}

static unsigned long skiplistDeleteRangeByScore(OrderedIndex *idx, double min, double max, int min_ex, int max_ex) {
    zrangespec range = {.min = min, .max = max, .minex = min_ex, .maxex = max_ex};
    return zslDeleteRangeByScore((zskiplist *)idx, &range, NULL);
}

static unsigned long skiplistDeleteRangeByRank(OrderedIndex *idx, unsigned long start, unsigned long end) {
    return zslDeleteRangeByRank((zskiplist *)idx, start, end, NULL);
}

static unsigned long skiplistDeleteRangeByLex(OrderedIndex *idx, const_sds min, const_sds max, int min_ex, int max_ex) {
    zlexrangespec range = {.min = (sds)min, .max = (sds)max, .minex = min_ex, .maxex = max_ex};
    return zslDeleteRangeByLex((zskiplist *)idx, &range, NULL);
}

/* Query */

static unsigned long skiplistLength(OrderedIndex *idx) {
    return zslGetLength((zskiplist *)idx);
}

static OrderedIndexItem *skiplistGetByRank(OrderedIndex *idx, unsigned long rank) {
    return (OrderedIndexItem *)zslGetElementByRank((zskiplist *)idx, rank);
}

static unsigned long skiplistGetRank(OrderedIndex *idx, const OrderedIndexItem *node) {
    return zslGetRank((zskiplist *)idx, (const zskiplistNode *)node);
}

static void skiplistGetElementRaw(const OrderedIndexItem *node, const char **ptr, size_t *len) {
    const zskiplistNode *znode = (const zskiplistNode *)node;
    sds ele = zslGetNodeElement(znode);
    *ptr = ele;
    *len = sdslen(ele);
}

static double skiplistGetScore(const OrderedIndexItem *node) {
    return zslGetScore((const zskiplistNode *)node);
}

/* Iterator */

static void skiplistInitIterator(OrderedIndexIterator *iter, OrderedIndex *idx) {
    zslInitIterator((zskiplistIterator *)iter, (zskiplist *)idx);
}

static void skiplistResetIterator(OrderedIndexIterator *iter) {
    zslResetIterator((zskiplistIterator *)iter);
}

static bool skiplistNext(OrderedIndexIterator *iter, OrderedIndexItem **pos) {
    return zslNext((zskiplistIterator *)iter, (zskiplistNode **)pos);
}

static bool skiplistPrev(OrderedIndexIterator *iter, OrderedIndexItem **pos) {
    return zslPrev((zskiplistIterator *)iter, (zskiplistNode **)pos);
}

static void skiplistSeekToRank(OrderedIndexIterator *iter, unsigned long rank) {
    zslSeekToRank((zskiplistIterator *)iter, rank);
}

static void skiplistSeekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset) {
    zslSeekToScoreRange((zskiplistIterator *)iter, min, max, min_ex, max_ex, offset);
}

static void skiplistSeekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset) {
    zslSeekToLexRange((zskiplistIterator *)iter, min, max, min_ex, max_ex, offset);
}

/* Skiplist implementation ops table */
const OrderedIndexOps skiplistOrderedIndexOps = {
    /* Lifecycle */
    .create = skiplistCreate,
    .free = skiplistFree,
    /* Modification */
    .insert = skiplistInsert,
    .delete_item = skiplistDelete,
    .update_score = skiplistUpdateScore,
    .pop_first = skiplistPopFirst,
    .pop_last = skiplistPopLast,
    .free_item = skiplistFreeItem,
    .delete_range_by_score = skiplistDeleteRangeByScore,
    .delete_range_by_rank = skiplistDeleteRangeByRank,
    .delete_range_by_lex = skiplistDeleteRangeByLex,
    /* Query */
    .length = skiplistLength,
    .get_by_rank = skiplistGetByRank,
    .get_rank = skiplistGetRank,
    .get_element_raw = skiplistGetElementRaw,
    .get_score = skiplistGetScore,
    /* Iterator */
    .init_iterator = skiplistInitIterator,
    .reset_iterator = skiplistResetIterator,
    .next = skiplistNext,
    .prev = skiplistPrev,
    .seek_to_rank = skiplistSeekToRank,
    .seek_to_score_range = skiplistSeekToScoreRange,
    .seek_to_lex_range = skiplistSeekToLexRange,
};
