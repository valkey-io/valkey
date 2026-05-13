#ifndef ORDERED_INDEX_TEST_H
#define ORDERED_INDEX_TEST_H

/*
 * Test-only interface for ordered index implementations.
 *
 * Defines an abstract C++ interface that each implementation subclasses.
 * Production code uses the functions declared in ordered_index.h
 * (implemented in ordered_index.c) which delegate to the active backend.
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
    virtual void free(OrderedIndex *oi) = 0;

    /* Modification */
    virtual OrderedIndexItem *insert(OrderedIndex *oi, double score, const char *ele, size_t len) = 0;
    virtual void deleteItem(OrderedIndex *oi, OrderedIndexItem *pos) = 0;
    virtual OrderedIndexItem *updateScore(OrderedIndex *oi, OrderedIndexItem *pos, double newscore) = 0;
    virtual OrderedIndexItem *popFirst(OrderedIndex *oi) = 0;
    virtual OrderedIndexItem *popLast(OrderedIndex *oi) = 0;
    virtual void freeItem(OrderedIndexItem *item) = 0;
    virtual unsigned long deleteRangeByScore(OrderedIndex *oi, double min, double max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) = 0;
    virtual unsigned long deleteRangeByRank(OrderedIndex *oi, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *ctx) = 0;
    virtual unsigned long deleteRangeByLex(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) = 0;

    /* Query */
    virtual unsigned long length(OrderedIndex *oi) = 0;
    virtual OrderedIndexItem *getByRank(OrderedIndex *oi, unsigned long rank) = 0;
    virtual unsigned long getRank(OrderedIndex *oi, const OrderedIndexItem *pos) = 0;
    virtual void getElementRaw(const OrderedIndexItem *pos, const char **ptr, size_t *len) = 0;
    virtual double getScore(const OrderedIndexItem *pos) = 0;

    /* Memory */
    virtual size_t estimateMemory(OrderedIndex *oi, size_t sample_size) = 0;

    /* Debug / verification */
    virtual int verifyIntegrity(OrderedIndex *oi, char *errmsg, size_t errmsg_len) = 0;

    /* Count */
    virtual unsigned long countScoreRange(OrderedIndex *oi, double min, double max, int min_ex, int max_ex) = 0;
    virtual unsigned long countLexRange(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex) = 0;

    /* Iterator */
    virtual void initIterator(OrderedIndexIterator *iter, OrderedIndex *oi) = 0;
    virtual void resetIterator(OrderedIndexIterator *iter) = 0;
    virtual OrderedIndexItem *next(OrderedIndexIterator *iter) = 0;
    virtual OrderedIndexItem *prev(OrderedIndexIterator *iter) = 0;
    virtual void seekToRank(OrderedIndexIterator *iter, unsigned long rank) = 0;
    virtual void seekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset) = 0;
    virtual void seekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset) = 0;

    /* Convenience (non-virtual) */
    OrderedIndexItem *insertSds(OrderedIndex *oi, double score, const_sds ele) {
        return insert(oi, score, ele, sdslen(ele));
    }

    std::vector<std::pair<double, std::string>> collectAll(OrderedIndex *oi) {
        std::vector<std::pair<double, std::string>> result;
        OrderedIndexIterator iter;
        OrderedIndexItem *pos;
        initIterator(&iter, oi);
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
    OrderedIndex *create() override {
        return skiplistCreate();
    }
    void free(OrderedIndex *oi) override {
        skiplistFree(oi);
    }

    OrderedIndexItem *insert(OrderedIndex *oi, double score, const char *ele, size_t len) override {
        return skiplistInsert(oi, score, ele, len);
    }
    void deleteItem(OrderedIndex *oi, OrderedIndexItem *pos) override {
        skiplistDelete(oi, pos);
    }
    OrderedIndexItem *updateScore(OrderedIndex *oi, OrderedIndexItem *pos, double newscore) override {
        return skiplistUpdateScore(oi, pos, newscore);
    }
    OrderedIndexItem *popFirst(OrderedIndex *oi) override {
        return skiplistPopFirst(oi);
    }
    OrderedIndexItem *popLast(OrderedIndex *oi) override {
        return skiplistPopLast(oi);
    }
    void freeItem(OrderedIndexItem *item) override {
        skiplistFreeItem(item);
    }
    unsigned long deleteRangeByScore(OrderedIndex *oi, double min, double max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) override {
        return skiplistDeleteRangeByScore(oi, min, max, min_ex, max_ex, on_delete, ctx);
    }
    unsigned long deleteRangeByRank(OrderedIndex *oi, unsigned long start, unsigned long end, OrderedIndexOnDelete on_delete, void *ctx) override {
        return skiplistDeleteRangeByRank(oi, start, end, on_delete, ctx);
    }
    unsigned long deleteRangeByLex(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex, OrderedIndexOnDelete on_delete, void *ctx) override {
        return skiplistDeleteRangeByLex(oi, min, max, min_ex, max_ex, on_delete, ctx);
    }

    unsigned long length(OrderedIndex *oi) override {
        return skiplistLength(oi);
    }
    OrderedIndexItem *getByRank(OrderedIndex *oi, unsigned long rank) override {
        return skiplistGetByRank(oi, rank);
    }
    unsigned long getRank(OrderedIndex *oi, const OrderedIndexItem *pos) override {
        return skiplistGetRank(oi, pos);
    }
    void getElementRaw(const OrderedIndexItem *pos, const char **ptr, size_t *len) override {
        skiplistGetElementRaw(pos, ptr, len);
    }
    double getScore(const OrderedIndexItem *pos) override {
        return skiplistGetScore(pos);
    }

    size_t estimateMemory(OrderedIndex *oi, size_t sample_size) override {
        return skiplistEstimateMemory(oi, sample_size);
    }

    int verifyIntegrity(OrderedIndex *oi, char *errmsg, size_t errmsg_len) override {
        return skiplistVerifyIntegrity(oi, errmsg, errmsg_len);
    }

    unsigned long countScoreRange(OrderedIndex *oi, double min, double max, int min_ex, int max_ex) override {
        return skiplistCountScoreRange(oi, min, max, min_ex, max_ex);
    }
    unsigned long countLexRange(OrderedIndex *oi, const_sds min, const_sds max, int min_ex, int max_ex) override {
        return skiplistCountLexRange(oi, min, max, min_ex, max_ex);
    }

    void initIterator(OrderedIndexIterator *iter, OrderedIndex *oi) override {
        skiplistInitIterator(iter, oi);
    }
    void resetIterator(OrderedIndexIterator *iter) override {
        skiplistResetIterator(iter);
    }
    OrderedIndexItem *next(OrderedIndexIterator *iter) override {
        return skiplistNext(iter);
    }
    OrderedIndexItem *prev(OrderedIndexIterator *iter) override {
        return skiplistPrev(iter);
    }
    void seekToRank(OrderedIndexIterator *iter, unsigned long rank) override {
        skiplistSeekToRank(iter, rank);
    }
    void seekToScoreRange(OrderedIndexIterator *iter, double min, double max, int min_ex, int max_ex, long offset) override {
        skiplistSeekToScoreRange(iter, min, max, min_ex, max_ex, offset);
    }
    void seekToLexRange(OrderedIndexIterator *iter, const_sds min, const_sds max, int min_ex, int max_ex, long offset) override {
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
