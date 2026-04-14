#ifndef ORDERED_INDEX_TEST_H
#define ORDERED_INDEX_TEST_H

/*
 * Test-only interface for ordered index implementations.
 *
 * Defines an abstract C++ interface that each implementation subclasses.
 * Production code uses compile-time dispatch via the inline wrappers in
 * ordered_index.h instead.
 */

extern "C" {
#include "ordered_index.h"
#include "skiplist_ordered_index.h"
}

#include <string>
#include <utility>
#include <vector>

/* ---- Abstract interface ---- */

class OrderedIndexTestApi {
public:
    virtual ~OrderedIndexTestApi() = default;

    /* Lifecycle */
    virtual OrderedIndex *create() = 0;
    virtual void free(OrderedIndex *idx) = 0;

    /* Modification */
    virtual OrderedIndexItem *insert(OrderedIndex *idx, double score, const char *ele, size_t len) = 0;
    virtual void deleteItem(OrderedIndex *idx, OrderedIndexItem *pos) = 0;
    virtual OrderedIndexItem *updateScore(OrderedIndex *idx, OrderedIndexItem *pos, double newscore) = 0;
    virtual OrderedIndexItem *popFirst(OrderedIndex *idx) = 0;
    virtual OrderedIndexItem *popLast(OrderedIndex *idx) = 0;
    virtual void freeItem(OrderedIndexItem *item) = 0;
    virtual unsigned long deleteRangeByScore(OrderedIndex *idx, double min, double max, int min_ex, int max_ex,
                                             OrderedIndexOnDelete on_delete, void *ctx) = 0;
    virtual unsigned long deleteRangeByRank(OrderedIndex *idx, unsigned long start, unsigned long end,
                                            OrderedIndexOnDelete on_delete, void *ctx) = 0;
    virtual unsigned long deleteRangeByLex(OrderedIndex *idx, const_sds min, const_sds max, int min_ex, int max_ex,
                                           OrderedIndexOnDelete on_delete, void *ctx) = 0;

    /* Query */
    virtual unsigned long length(OrderedIndex *idx) = 0;
    virtual OrderedIndexItem *getByRank(OrderedIndex *idx, unsigned long rank) = 0;
    virtual unsigned long getRank(OrderedIndex *idx, const OrderedIndexItem *pos) = 0;
    virtual void getElementRaw(const OrderedIndexItem *pos, const char **ptr, size_t *len) = 0;
    virtual double getScore(const OrderedIndexItem *pos) = 0;

    /* Memory */
    virtual size_t estimateMemory(OrderedIndex *idx, size_t sample_size) = 0;

    /* Iterator */
    virtual void initIterator(OrderedIndexIterator *iter, OrderedIndex *idx) = 0;
    virtual void resetIterator(OrderedIndexIterator *iter) = 0;
    virtual bool next(OrderedIndexIterator *iter, OrderedIndexItem **pos) = 0;
    virtual bool prev(OrderedIndexIterator *iter, OrderedIndexItem **pos) = 0;
    virtual void seekToRank(OrderedIndexIterator *iter, unsigned long rank) = 0;
    virtual void seekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex,
                                  long offset) = 0;
    virtual void seekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex,
                                long offset) = 0;

    /* Convenience (non-virtual) */
    OrderedIndexItem *insertSds(OrderedIndex *idx, double score, const_sds ele) {
        return insert(idx, score, ele, sdslen(ele));
    }

    std::vector<std::pair<double, std::string>> collectAll(OrderedIndex *idx) {
        std::vector<std::pair<double, std::string>> result;
        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        initIterator(&iter, idx);
        while (next(&iter, &pos)) {
            const char *ptr;
            size_t len;
            getElementRaw(pos, &ptr, &len);
            result.emplace_back(getScore(pos), std::string(ptr, len));
        }
        resetIterator(&iter);
        return result;
    }
};

/* ---- Skiplist implementation ---- */

class SkiplistOrderedIndex : public OrderedIndexTestApi {
public:
    OrderedIndex *create() override { return skiplistCreate(); }
    void free(OrderedIndex *idx) override { skiplistFree(idx); }

    OrderedIndexItem *insert(OrderedIndex *idx, double score, const char *ele, size_t len) override {
        return skiplistInsert(idx, score, ele, len);
    }
    void deleteItem(OrderedIndex *idx, OrderedIndexItem *pos) override { skiplistDelete(idx, pos); }
    OrderedIndexItem *updateScore(OrderedIndex *idx, OrderedIndexItem *pos, double newscore) override {
        return skiplistUpdateScore(idx, pos, newscore);
    }
    OrderedIndexItem *popFirst(OrderedIndex *idx) override { return skiplistPopFirst(idx); }
    OrderedIndexItem *popLast(OrderedIndex *idx) override { return skiplistPopLast(idx); }
    void freeItem(OrderedIndexItem *item) override { skiplistFreeItem(item); }
    unsigned long deleteRangeByScore(OrderedIndex *idx, double min, double max, int min_ex, int max_ex,
                                     OrderedIndexOnDelete on_delete, void *ctx) override {
        return skiplistDeleteRangeByScore(idx, min, max, min_ex, max_ex, on_delete, ctx);
    }
    unsigned long deleteRangeByRank(OrderedIndex *idx, unsigned long start, unsigned long end,
                                    OrderedIndexOnDelete on_delete, void *ctx) override {
        return skiplistDeleteRangeByRank(idx, start, end, on_delete, ctx);
    }
    unsigned long deleteRangeByLex(OrderedIndex *idx, const_sds min, const_sds max, int min_ex, int max_ex,
                                   OrderedIndexOnDelete on_delete, void *ctx) override {
        return skiplistDeleteRangeByLex(idx, min, max, min_ex, max_ex, on_delete, ctx);
    }

    unsigned long length(OrderedIndex *idx) override { return skiplistLength(idx); }
    OrderedIndexItem *getByRank(OrderedIndex *idx, unsigned long rank) override {
        return skiplistGetByRank(idx, rank);
    }
    unsigned long getRank(OrderedIndex *idx, const OrderedIndexItem *pos) override {
        return skiplistGetRank(idx, pos);
    }
    void getElementRaw(const OrderedIndexItem *pos, const char **ptr, size_t *len) override {
        skiplistGetElementRaw(pos, ptr, len);
    }
    double getScore(const OrderedIndexItem *pos) override { return skiplistGetScore(pos); }

    size_t estimateMemory(OrderedIndex *idx, size_t sample_size) override {
        return skiplistEstimateMemory(idx, sample_size);
    }

    void initIterator(OrderedIndexIterator *iter, OrderedIndex *idx) override {
        skiplistInitIterator(iter, idx);
    }
    void resetIterator(OrderedIndexIterator *iter) override { skiplistResetIterator(iter); }
    bool next(OrderedIndexIterator *iter, OrderedIndexItem **pos) override { return skiplistNext(iter, pos); }
    bool prev(OrderedIndexIterator *iter, OrderedIndexItem **pos) override { return skiplistPrev(iter, pos); }
    void seekToRank(OrderedIndexIterator *iter, unsigned long rank) override {
        skiplistSeekToRank(iter, rank);
    }
    void seekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex,
                          long offset) override {
        skiplistSeekToScoreRange(iter, min, max, min_ex, max_ex, offset);
    }
    void seekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex,
                        long offset) override {
        skiplistSeekToLexRange(iter, min, max, min_ex, max_ex, offset);
    }
};

/* ---- Static instances & test parameterization helpers ---- */

static SkiplistOrderedIndex skiplistImpl;

static std::string orderedIndexTestName(const ::testing::TestParamInfo<OrderedIndexTestApi *> &info) {
    if (info.param == &skiplistImpl) return "Skiplist";
    return "Unknown";
}

#endif /* ORDERED_INDEX_TEST_H */
