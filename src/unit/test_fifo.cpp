/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "adlist.h"
#include "fifo.h"
#include "monotonic.h"
}

static inline void *intToPointer(intptr_t i) {
    return (void *)i;
}

static inline intptr_t pointerToInt(void *p) {
    return (intptr_t)p;
}

class FifoTest : public ::testing::Test {
  protected:
    fifo *q;

    void SetUp() override {
        q = fifoCreate();
    }

    void TearDown() override {
        fifoRelease(q);
    }

    /* Helper function to push a value and verify the length increases */
    void push(intptr_t value) {
        int len = fifoLength(q);
        fifoPush(q, intToPointer(value));
        EXPECT_EQ(fifoLength(q), len + 1);
    }

    /* Helper function to pop and verify the expected value */
    intptr_t popTest(intptr_t expected) {
        void *peekPtr;
        EXPECT_TRUE(fifoPeek(q, &peekPtr));
        intptr_t peekValue = pointerToInt(peekPtr);
        EXPECT_EQ(peekValue, expected);
        int len = fifoLength(q);
        void *popPtr;
        EXPECT_TRUE(fifoPop(q, &popPtr));
        intptr_t value = pointerToInt(popPtr);
        EXPECT_EQ(fifoLength(q), len - 1);
        EXPECT_EQ(value, expected);
        return value;
    }
};

/* Test: emptyPop - verify that popping from empty fifo returns false */
TEST_F(FifoTest, TestFifoEmptyPop) {
    EXPECT_EQ(fifoLength(q), 0);
    void *result;
    EXPECT_FALSE(fifoPop(q, &result));
}

/* Test: emptyPeek - verify that peeking at empty fifo returns false */
TEST_F(FifoTest, TestFifoEmptyPeek) {
    EXPECT_EQ(fifoLength(q), 0);
    void *result;
    EXPECT_FALSE(fifoPeek(q, &result));
}

/* Test: simplePushPop */
TEST_F(FifoTest, TestFifoSimplePushPop) {
    EXPECT_EQ(fifoLength(q), 0);
    push(1);
    popTest(1);
    EXPECT_EQ(fifoLength(q), 0);
}

/* Test: tryVariousSizes */
TEST_F(FifoTest, TestFifoTryVariousSizes) {
    for (int items = 1; items < 50; items++) {
        EXPECT_EQ(fifoLength(q), 0);
        for (int value = 1; value <= items; value++) push(value);
        for (int value = 1; value <= items; value++) popTest(value);
        EXPECT_EQ(fifoLength(q), 0);
    }
}

/* Test: pushPopTest */
TEST_F(FifoTest, TestFifoPushPopTest) {
    /* In this test, we repeatedly push 2 and pop 1. This hits the list differently than
     * other tests which push a bunch and then pop them all off. */
    int pushVal = 1;
    int popVal = 1;
    for (int i = 0; i < 200; i++) {
        if (i % 3 == 0 || i % 3 == 1) {
            push(pushVal++);
        } else {
            popTest(popVal++);
        }
    }
}

/* Test: joinTest */
TEST_F(FifoTest, TestFifoJoinTest) {
    fifo *q2 = fifoCreate();

    /* In this test, there are 2 fifos Q and Q2.
     * Various sizes are tested. For each size, various amounts are popped off the front first.
     * Q2 is appended to Q. */
    for (int qLen = 0; qLen <= 21; qLen++) {
        for (int qPop = 0; qPop < 6 && qPop <= qLen; qPop++) {
            for (int q2Len = 0; q2Len <= 21; q2Len++) {
                for (int q2Pop = 0; q2Pop < 6 && q2Pop <= q2Len; q2Pop++) {
                    intptr_t pushValue = 1;
                    intptr_t popQValue = 1;

                    for (int i = 0; i < qLen; i++) fifoPush(q, intToPointer(pushValue++));
                    for (int i = 0; i < qPop; i++) {
                        void *ptr;
                        EXPECT_TRUE(fifoPop(q, &ptr) && pointerToInt(ptr) == popQValue++);
                    }

                    intptr_t popQ2Value = pushValue;
                    for (int i = 0; i < q2Len; i++) fifoPush(q2, intToPointer(pushValue++));
                    for (int i = 0; i < q2Pop; i++) {
                        void *ptr;
                        EXPECT_TRUE(fifoPop(q2, &ptr) && pointerToInt(ptr) == popQ2Value++);
                    }

                    fifoJoin(q, q2);
                    EXPECT_EQ(fifoLength(q), (qLen - qPop) + (q2Len - q2Pop));
                    EXPECT_EQ(fifoLength(q2), 0);

                    fifo *temp = fifoPopAll(q); /* Exercise fifoPopAll also */
                    EXPECT_EQ(fifoLength(temp), (qLen - qPop) + (q2Len - q2Pop));
                    EXPECT_EQ(fifoLength(q), 0);

                    for (int i = 0; i < (qLen - qPop); i++) {
                        void *ptr;
                        EXPECT_TRUE(fifoPop(temp, &ptr) && pointerToInt(ptr) == popQValue++);
                    }
                    for (int i = 0; i < (q2Len - q2Pop); i++) {
                        void *ptr;
                        EXPECT_TRUE(fifoPop(temp, &ptr) && pointerToInt(ptr) == popQ2Value++);
                    }
                    EXPECT_EQ(fifoLength(temp), 0);

                    fifoRelease(temp);
                }
            }
        }
    }

    fifoRelease(q2);
}

const int LIST_ITEMS = 10000;

static void exerciseList(void) {
    list *q = listCreate();
    for (intptr_t i = 0; i < LIST_ITEMS; i++) {
        listAddNodeTail(q, intToPointer(i));
    }
    EXPECT_EQ(listLength(q), (unsigned)LIST_ITEMS);
    for (intptr_t i = 0; i < LIST_ITEMS; i++) {
        listNode *node = listFirst(q);
        listDelNode(q, node);
        EXPECT_EQ(listNodeValue(node), intToPointer(i));
    }
    EXPECT_EQ(listLength(q), 0u);
    listRelease(q);
}

static void exerciseFifo(void) {
    fifo *q = fifoCreate();
    for (intptr_t i = 0; i < LIST_ITEMS; i++) {
        fifoPush(q, intToPointer(i));
    }
    EXPECT_EQ(fifoLength(q), LIST_ITEMS);
    for (intptr_t i = 0; i < LIST_ITEMS; i++) {
        void *ptr;
        EXPECT_TRUE(fifoPop(q, &ptr) && ptr == intToPointer(i));
    }
    EXPECT_EQ(fifoLength(q), 0);
    fifoRelease(q);
}

/* This test is disabled by default because it's a performance comparison test.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=FifoTest.DISABLED_TestFifoComparePerformance --gtest_also_run_disabled_tests
 * This test will exercise both FIFO and ADLIST to compare performance.
 * The test will (intentionally) fail, printing the results as failed assertions. */
TEST_F(FifoTest, DISABLED_TestFifoComparePerformance) {
    monotonicInit();
    monotime timer;
    const int iterations = 500;

    exerciseList(); /* Warm up the list before timing */
    elapsedStart(&timer);
    for (int i = 0; i < iterations; i++) exerciseList();
    long listMs = elapsedMs(timer);

    exerciseFifo(); /* Warm up the fifo before timing */
    elapsedStart(&timer);
    for (int i = 0; i < iterations; i++) exerciseFifo();
    long fifoMs = elapsedMs(timer);

    double percentImprovement = (double)(listMs - fifoMs) * 100.0 / listMs;
    printf("List: %ld ms, FIFO: %ld ms, Improvement: %.2f%%\n", listMs, fifoMs, percentImprovement);
}
