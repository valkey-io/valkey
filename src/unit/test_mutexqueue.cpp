/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <pthread.h>
#include <time.h>

extern "C" {
#include "fifo.h"
#include "mutexqueue.h"
}

class MutexQueueTest : public ::testing::Test {
  protected:
    mutexQueue *q;

    void SetUp() override {
        q = mutexQueueCreate();
    }

    void TearDown() override {
        fifo *fifo = mutexQueuePopAll(q, false);
        if (fifo) fifoRelease(fifo);
        mutexQueueRelease(q);
    }

    /* Helper function to add a value and verify the length increases */
    void add(long value) {
        unsigned long len = mutexQueueLength(q);
        mutexQueueAdd(q, (void *)value);
        EXPECT_EQ(mutexQueueLength(q), len + 1);
    }

    /* Helper function to add a priority value and verify the length increases */
    void priorityAdd(long value) {
        unsigned long len = mutexQueueLength(q);
        mutexQueuePushPriority(q, (void *)value);
        EXPECT_EQ(mutexQueueLength(q), len + 1);
    }

    /* Helper function to pop and verify the expected value */
    void popTest(long expected) {
        unsigned long len = mutexQueueLength(q);
        long value = (long)mutexQueuePop(q, false);
        EXPECT_EQ(mutexQueueLength(q), len - 1);
        EXPECT_EQ(value, expected);
    }
};

/* Test: simplePushPop */
TEST_F(MutexQueueTest, TestMutexQueueSimplePushPop) {
    EXPECT_EQ(mutexQueueLength(q), 0ul);
    add(1);
    popTest(1);
    EXPECT_EQ(mutexQueuePop(q, false), nullptr);
}

/* Test: doublePushPop */
TEST_F(MutexQueueTest, TestMutexQueueDoublePushPop) {
    add(1);
    add(2);
    popTest(1);
    popTest(2);
}

/* Test: priorityOrdering */
TEST_F(MutexQueueTest, TestMutexQueuePriorityOrdering) {
    add(10);
    priorityAdd(1);
    add(11);
    priorityAdd(2);
    popTest(1);
    popTest(2);
    popTest(10);
    popTest(11);
    EXPECT_EQ(mutexQueuePop(q, false), nullptr);
}

/* Test: fifoPopAll */
TEST_F(MutexQueueTest, TestMutexQueueFifoPopAll) {
    add(10);
    priorityAdd(1);
    add(11);
    priorityAdd(2);

    fifo *f = mutexQueuePopAll(q, false);
    ASSERT_NE(f, nullptr); /* Fatal - can't continue if NULL */
    EXPECT_EQ(mutexQueuePop(q, false), nullptr);
    EXPECT_EQ(mutexQueuePopAll(q, false), nullptr);
    EXPECT_EQ(mutexQueueLength(q), 0ul);

    void *ptr;
    EXPECT_TRUE(fifoPop(f, &ptr) && (unsigned long)ptr == 1ul);
    EXPECT_TRUE(fifoPop(f, &ptr) && (unsigned long)ptr == 2ul);
    EXPECT_TRUE(fifoPop(f, &ptr) && (unsigned long)ptr == 10ul);
    EXPECT_TRUE(fifoPop(f, &ptr) && (unsigned long)ptr == 11ul);
    EXPECT_EQ(fifoLength(f), 0);

    fifoRelease(f);
}

/* Test: fifoAddMultiple */
TEST_F(MutexQueueTest, TestMutexQueueFifoAddMultiple) {
    add(1);

    fifo *f = fifoCreate();
    fifoPush(f, (void *)2);
    fifoPush(f, (void *)3);
    mutexQueueAddMultiple(q, f);
    EXPECT_EQ(fifoLength(f), 0L);
    fifoRelease(f);

    add(4);
    priorityAdd(0);
    popTest(0);
    popTest(1);
    popTest(2);
    popTest(3);
    popTest(4);
    EXPECT_EQ(mutexQueuePop(q, false), nullptr);
    EXPECT_EQ(mutexQueueLength(q), 0ul);
}

/* Thread functions for concurrent tests */
static void *queue_writer(void *arg) {
    mutexQueue *queue = (mutexQueue *)arg;
    for (int i = 1; i <= 1000; i++) {
        mutexQueueAdd(queue, (void *)(long)i);
    }
    return nullptr;
}

static void *queue_reader(void *arg) {
    mutexQueue *queue = (mutexQueue *)arg;
    int count = 0;
    while (count < 1000) {
        long value = (long)mutexQueuePop(queue, true);
        EXPECT_NE(value, 0); /* Should never be null if blocking */
        count++;
    }
    return nullptr;
}

/* Test: simpleThread */
TEST_F(MutexQueueTest, TestMutexQueueSimpleThread) {
    int rc;
    pthread_t writer, reader;

    rc = pthread_create(&writer, nullptr, &queue_writer, q);
    ASSERT_EQ(rc, 0); /* Fatal - can't continue if thread creation fails */
    rc = pthread_create(&reader, nullptr, &queue_reader, q);
    ASSERT_EQ(rc, 0);
    rc = pthread_join(writer, nullptr);
    ASSERT_EQ(rc, 0);
    rc = pthread_join(reader, nullptr);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(mutexQueueLength(q), 0ul);

    rc = pthread_create(&reader, nullptr, &queue_reader, q);
    ASSERT_EQ(rc, 0);
    rc = pthread_create(&writer, nullptr, &queue_writer, q);
    ASSERT_EQ(rc, 0);
    rc = pthread_join(writer, nullptr);
    ASSERT_EQ(rc, 0);
    rc = pthread_join(reader, nullptr);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(mutexQueueLength(q), 0ul);
}

/* Test: parallelWriters */
TEST_F(MutexQueueTest, TestMutexQueueParallelWriters) {
    const int num_threads = 20;
    int rc;
    pthread_t writer[num_threads];

    for (int i = 0; i < num_threads; i++) {
        rc = pthread_create(&writer[i], nullptr, &queue_writer, q);
        ASSERT_EQ(rc, 0);
    }

    for (int i = 0; i < num_threads; i++) {
        rc = pthread_join(writer[i], nullptr);
        ASSERT_EQ(rc, 0);
    }

    EXPECT_EQ(mutexQueueLength(q), (unsigned long)(num_threads * 1000));

    fifo *f = mutexQueuePopAll(q, false);
    fifoRelease(f);
}

/* Test: parallelReaders */
TEST_F(MutexQueueTest, TestMutexQueueParallelReaders) {
    const int num_threads = 20;
    int rc;
    pthread_t reader[num_threads];

    /* Start readers in advance - we want them fighting... */
    for (int i = 0; i < num_threads; i++) {
        rc = pthread_create(&reader[i], nullptr, &queue_reader, q);
        ASSERT_EQ(rc, 0); /* Fatal - can't continue if thread creation fails */
    }

    /* Now perform writes serially... */
    struct timespec sleep_time = {0, 10000000L}; /* 10ms */
    for (int i = 0; i < num_threads; i++) {
        queue_writer(q);
        /* make sure other threads get to fight with a short sleep (don't write all at once!) */
        nanosleep(&sleep_time, nullptr);
    }

    /* Readers should finish */
    for (int i = 0; i < num_threads; i++) {
        rc = pthread_join(reader[i], nullptr);
        ASSERT_EQ(rc, 0);
    }

    EXPECT_EQ(mutexQueueLength(q), 0ul);
}

/* Test: parallelReadWrite */
TEST_F(MutexQueueTest, TestMutexQueueParallelReadWrite) {
    const int num_threads = 20;
    int rc;
    pthread_t reader[num_threads];
    pthread_t writer[num_threads];

    for (int i = 0; i < num_threads; i++) {
        rc = pthread_create(&writer[i], nullptr, &queue_writer, q);
        ASSERT_EQ(rc, 0);
        rc = pthread_create(&reader[i], nullptr, &queue_reader, q);
        ASSERT_EQ(rc, 0);
    }

    for (int i = 0; i < num_threads; i++) {
        rc = pthread_join(writer[i], nullptr);
        ASSERT_EQ(rc, 0);
        rc = pthread_join(reader[i], nullptr);
        ASSERT_EQ(rc, 0);
    }

    EXPECT_EQ(mutexQueueLength(q), 0ul);
}
